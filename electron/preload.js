'use strict';
// The page's only bridge to the helper: receive commands, send events. Nothing else of Node or Electron is exposed.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('noctara', {
  onCommand(handler) {
    ipcRenderer.on('noctara:command', (_event, message) => handler(message));
  },
  emit(message) {
    ipcRenderer.send('noctara:event', message);
  },
  ready() {
    ipcRenderer.send('noctara:page-ready');
  },
});
