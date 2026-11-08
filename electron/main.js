'use strict';
// Noctara's protected-playback helper: a headless castlabs Electron (Widevine) process that plays DRM-protected
// tracks through the standard EME path, controlled by Noctara over stdin/stdout (JSON lines, protocol.json).
//
// Kept minimal on purpose: no visible window, no devtools, no GPU unless asked for, an in-memory session (no HTTP
// or media cache on disk), a network allowlist (SoundCloud's media and license hosts only), and nothing that could
// record, extract or store media, keys or licenses. userData holds only Chromium's profile and the Widevine CDM
// component. See docs/drm-helper.md and docs/ipc.md.
const { app, BrowserWindow, components, ipcMain, session } = require('electron');
const path = require('node:path');
const readline = require('node:readline');
const protocol = require('./protocol');

const ALLOWED_HOSTS = [/\.media-streaming\.soundcloud\.cloud$/];  // playback.* (media), license.* (Widevine)

function argument(name) {
  const prefix = `--${name}=`;
  const found = process.argv.find((a) => a.startsWith(prefix));
  return found ? found.slice(prefix.length) : undefined;
}

// Only protocol messages go to stdout; anything else Electron prints goes to stderr.
function send(message) {
  const problem = protocol.validate('fromHelper', message);
  if (problem) {
    process.stderr.write(`noctara-drm: refusing to send an invalid message: ${problem}\n`);
    return;
  }
  process.stdout.write(`${JSON.stringify(message)}\n`);
}
function log(level, message) {
  send({ type: 'log', level, message });
}

const userData = argument('noctara-user-data');
if (userData) {
  app.setPath('userData', userData);
  app.setPath('sessionData', userData);
}
// Audio only: no GPU unless Noctara asks for it, no disk cache, no throttling of the hidden page.
if (!process.argv.includes('--noctara-gpu')) {
  app.disableHardwareAcceleration();
  app.commandLine.appendSwitch('in-process-gpu');
}
// 0 would mean "the default size" to Chromium: 1 byte leaves the default session's disk cache unusable (the
// player's session is in memory anyway).
app.commandLine.appendSwitch('disk-cache-size', '1');
app.commandLine.appendSwitch('media-cache-size', '1');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-background-timer-throttling');
// Memory: the network and audio services have no sandbox of their own here, so a separate process buys nothing. The
// renderer and the Widevine CDM stay in their own sandboxed processes.
app.commandLine.appendSwitch('enable-features', 'NetworkServiceInProcess');
app.commandLine.appendSwitch('disable-features',
  'MediaSessionService,HardwareMediaKeyHandling,Translate,AutofillServerCommunication,SpareRendererForSitePerProcess,'
  + 'AudioServiceOutOfProcess');

let player = null;     // the hidden BrowserWindow
let pageReady = false;
const pending = [];    // page commands that arrived before the page could take them
let proxyCredentials = null;
let drmSession = null;
let resolveConfigured;
// Resolves once the first "configure" has been applied.
const configured = new Promise((resolve) => { resolveConfigured = resolve; });

function toPage(message) {
  if (pageReady && player) {
    player.webContents.send('noctara:command', message);
  } else {
    pending.push(message);
  }
}

async function applyNetwork(config) {
  const proxy = config.proxy || {};
  let proxyConfig = { mode: 'direct' };
  let proxyDescription = 'direct';
  if (proxy.mode === 'system') {
    proxyConfig = { mode: 'system' };
    proxyDescription = 'system';
  } else if (proxy.mode === 'manual' && proxy.rules) {
    proxyConfig = { mode: 'fixed_servers', proxyRules: proxy.rules, proxyBypassRules: proxy.bypass || '' };
    proxyDescription = proxy.display || 'manual';
  }
  // Both sessions: the player's and the default one (Chromium's own requests, such as the CDM component check).
  await Promise.all([drmSession.setProxy(proxyConfig), session.defaultSession.setProxy(proxyConfig)]);
  proxyCredentials = proxy.username || proxy.password
    ? { username: proxy.username || '', password: proxy.password || '' } : null;
  await Promise.all([drmSession.closeAllConnections(), session.defaultSession.closeAllConnections()]);

  const dns = config.dns || {};
  let dnsDescription = 'system';
  if (dns.mode === 'doh' && dns.template) {
    app.configureHostResolver({ secureDnsMode: dns.strict ? 'secure' : 'automatic', secureDnsServers: [dns.template] });
    dnsDescription = dns.strict ? 'doh-strict' : 'doh';
  } else {
    app.configureHostResolver({ secureDnsMode: 'off', secureDnsServers: [] });
  }
  send({ type: 'configured', proxy: proxyDescription, dns: dnsDescription });
}

async function handle(message) {
  switch (message.type) {
    case 'hello':
      if (message.version !== protocol.VERSION) {
        send({ type: 'error', id: '', code: 'protocol', detail: `protocol ${message.version} is not ${protocol.VERSION}` });
      }
      break;
    case 'configure':
      try {
        await applyNetwork(message);
      } catch (error) {
        send({ type: 'error', id: '', code: 'network', detail: String(error) });
      }
      resolveConfigured();
      break;
    case 'ping':
      send({ type: 'pong', seq: message.seq });
      break;
    case 'shutdown':
      app.quit();
      break;
    default:
      toPage(message);
  }
}

app.on('login', (event, _webContents, _details, authInfo, callback) => {
  // Proxy authentication only, with the credentials Noctara hands over in "configure" (Chromium may ask before
  // they arrive, for the CDM component check); never for web sites.
  if (!authInfo || !authInfo.isProxy) return;
  event.preventDefault();
  configured.then(() => {
    if (proxyCredentials) callback(proxyCredentials.username, proxyCredentials.password);
    else callback();
  });
});

// Spell checking is off in every session from its creation: otherwise Chromium downloads a dictionary for the
// system language from Google right away. The page has no text input.
app.on('session-created', (created) => {
  created.setSpellCheckerEnabled(false);
  created.setSpellCheckerDictionaryDownloadURL('http://127.0.0.1:9/');  // never Google's CDN
});

let quitting = false;
app.on('before-quit', () => { quitting = true; });
app.on('window-all-closed', () => app.quit());
// Nothing may open windows, navigate or attach devtools.
app.on('web-contents-created', (_event, contents) => {
  contents.setWindowOpenHandler(() => ({ action: 'deny' }));
  contents.on('will-navigate', (event) => event.preventDefault());
  contents.on('devtools-opened', () => contents.closeDevTools());
});

app.whenReady().then(async () => {
  // DNS over HTTPS before anything else: Chromium checks its CDM component right after start (the proxy and host
  // mappings are command-line switches already).
  const doh = argument('noctara-doh');
  if (doh) {
    app.configureHostResolver({
      secureDnsMode: process.argv.includes('--noctara-doh-strict') ? 'secure' : 'automatic',
      secureDnsServers: [doh],
    });
  }
  // In-memory partition (no "persist:" prefix): no HTTP cache, cookies or storage on disk.
  drmSession = session.fromPartition('noctara-drm', { cache: false });
  drmSession.webRequest.onBeforeRequest((details, callback) => {
    const url = new URL(details.url);
    // Local: the helper's own page (file), Media Source buffers (blob), key data in playlists (data).
    const local = url.protocol === 'file:' || url.protocol === 'blob:' || url.protocol === 'data:';
    const allowed = local || (url.protocol === 'https:' && ALLOWED_HOSTS.some((host) => host.test(url.hostname)));
    callback({ cancel: !allowed });
  });
  drmSession.setPermissionRequestHandler((_contents, permission, callback) => callback(permission === 'mediaKeySystem'));

  // Commands run strictly in order, so the network configuration is in place before anything loads.
  let queue = Promise.resolve();
  const input = readline.createInterface({ input: process.stdin });
  input.on('line', (line) => {
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      log('warning', 'a line that is not JSON was ignored');
      return;
    }
    const problem = protocol.validate('toHelper', message);
    if (problem) {
      log('warning', `invalid message ignored: ${problem}`);
      return;
    }
    queue = queue.then(() => handle(message)).catch((error) => log('error', String(error)));
  });
  // Noctara gone: so is the helper (no orphans).
  input.on('close', () => app.quit());

  // The Widevine component check is network traffic too: it waits for Noctara's network settings.
  await configured;
  let widevine = false;
  let detail;
  try {
    await components.whenReady();
    widevine = true;
  } catch (error) {
    detail = String(error && error.message ? error.message : error);
  }

  player = new BrowserWindow({
    show: false,
    width: 320,
    height: 200,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      session: drmSession,
      contextIsolation: true,
      sandbox: true,
      nodeIntegration: false,
      devTools: false,
      backgroundThrottling: false,
      spellcheck: false,
    },
  });

  // The page's renderer died (killed, crashed): this process would keep answering pings for a page that no longer
  // plays. Exiting lets Noctara see a crash, restart the helper and resume the track.
  player.webContents.on('render-process-gone', (_event, details) => {
    if (quitting) return;
    log('error', `the player page is gone: ${details.reason}`);
    app.exit(1);
  });
  ipcMain.on('noctara:event', (event, message) => {
    if (event.sender === player.webContents) send(message);
  });
  ipcMain.once('noctara:page-ready', () => {
    pageReady = true;
    for (const message of pending.splice(0)) player.webContents.send('noctara:command', message);
  });
  await player.loadFile(path.join(__dirname, 'player.html'));

  send({ type: 'ready', version: protocol.VERSION, widevine, electron: process.versions.electron,
         ...(detail ? { detail } : {}) });
}).catch((error) => {
  process.stderr.write(`noctara-drm: start failed: ${error}\n`);
  app.exit(1);
});
