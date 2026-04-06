const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('zeusmod', {
    // Window
    minimize: () => ipcRenderer.send('window:minimize'),
    maximize: () => ipcRenderer.send('window:maximize'),
    close: () => ipcRenderer.send('window:close'),

    // Steam
    getGames: () => ipcRenderer.invoke('steam:getGames'),

    // Game
    detectGame: (processName) => ipcRenderer.invoke('game:detect', processName),
    injectGame: () => ipcRenderer.invoke('game:inject'),

    // Updates
    onUpdateAvailable: (callback) => ipcRenderer.on('update:available', (_, data) => callback(data)),
    downloadUpdate: (url) => ipcRenderer.send('update:download', url),

    // Cheat control
    toggleCheat: (name, value) => ipcRenderer.invoke('cheat:toggle', name, value),

    // App info
    getVersion: () => ipcRenderer.invoke('app:version'),
});
