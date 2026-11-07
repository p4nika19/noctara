'use strict';
// Plays one protected track at a time: hls.js feeds Media Source Extensions, Widevine (EME) decrypts inside the
// CDM. Reports state and position to Noctara; stores nothing.
const audio = document.getElementById('audio');
let hls = null;
let current = '';  // id of the loaded track
let lastPosition = -1;
let wantPlay = false;  // what Noctara asked for last: play once the stream is ready, or stay paused

function emit(message) {
  window.noctara.emit(message);
}
function state(value) {
  if (current) emit({ type: 'state', id: current, state: value });
}
function position() {
  if (!current) return;
  const ms = Math.round(audio.currentTime * 1000);
  const durationMs = Number.isFinite(audio.duration) ? Math.round(audio.duration * 1000) : 0;
  if (ms !== lastPosition) {
    lastPosition = ms;
    emit({ type: 'position', id: current, ms, durationMs });
  }
}
function fail(code, detail) {
  emit({ type: 'error', id: current, code, detail: String(detail || '') });
}

// A pause or a new load interrupts a pending play() with an AbortError: that is no failure, and a late
// rejection must never be reported against the track loaded since.
function playNow() {
  const id = current;
  audio.play().catch((e) => {
    if (e && e.name === 'AbortError') return;
    if (id === current) fail('media', e);
  });
}

function unload() {
  if (hls) {
    hls.destroy();
    hls = null;
  }
  audio.removeAttribute('src');
  audio.load();
  current = '';
  lastPosition = -1;
}

function load(command) {
  unload();
  current = command.id;
  wantPlay = command.autoplay;
  audio.volume = Math.max(0, Math.min(1, command.volume));
  if (typeof Hls === 'undefined' || !Hls.isSupported()) {
    fail('unsupported', 'Media Source Extensions are not available');
    return;
  }
  state('loading');
  hls = new Hls({
    enableWorker: false,
    emeEnabled: true,
    // Only what playback needs: no persistent licenses, no stored media.
    drmSystems: { 'com.widevine.alpha': { licenseUrl: command.licenseUrl } },
    drmSystemOptions: { persistentState: 'not-allowed', distinctiveIdentifier: 'not-allowed' },
    licenseXhrSetup: (xhr, url) => {
      const target = new URL(url);
      target.searchParams.set('license_token', command.licenseToken);
      xhr.open('POST', target.toString(), true);
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    },
    maxBufferLength: 30,
    startPosition: command.startMs > 0 ? command.startMs / 1000 : -1,
  });
  hls.on(Hls.Events.ERROR, (_event, data) => {
    if (!data.fatal) return;
    const code = data.type === Hls.ErrorTypes.KEY_SYSTEM_ERROR ? 'drm'
      : data.type === Hls.ErrorTypes.NETWORK_ERROR ? 'network' : 'media';
    fail(code, data.details);
  });
  hls.loadSource(command.url);
  hls.attachMedia(audio);
  hls.once(Hls.Events.MANIFEST_PARSED, () => {
    if (wantPlay) playNow();
    else state('paused');
  });
}

audio.addEventListener('playing', () => state('playing'));
audio.addEventListener('pause', () => { if (!audio.ended) state('paused'); });
audio.addEventListener('waiting', () => state('buffering'));
audio.addEventListener('ended', () => { position(); state('ended'); });
audio.addEventListener('durationchange', position);
audio.addEventListener('seeked', position);
audio.addEventListener('error', () => fail('media', audio.error ? audio.error.message || audio.error.code : 'error'));
setInterval(() => { if (!audio.paused) position(); }, 250);

window.noctara.onCommand((command) => {
  switch (command.type) {
    case 'load': load(command); break;
    case 'play':
      wantPlay = true;
      // Also before any media data (a paused load whose manifest is parsed): play() then waits for it.
      if (current && hls) playNow();
      break;
    case 'pause': wantPlay = false; audio.pause(); break;
    case 'seek': audio.currentTime = command.ms / 1000; position(); break;
    case 'volume': audio.volume = Math.max(0, Math.min(1, command.value)); break;
    case 'stop': unload(); break;
    default: break;
  }
});
window.noctara.ready();
