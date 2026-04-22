const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('zeusmod', {
    minimize: () => ipcRenderer.send('window:minimize'),
    maximize: () => ipcRenderer.send('window:maximize'),
    close: () => ipcRenderer.send('window:close'),

    getGameStatus: () => ipcRenderer.invoke('game:getStatus'),
    injectGame: () => ipcRenderer.invoke('game:inject'),

    getUpdateState: () => ipcRenderer.invoke('update:getState'),
    checkForUpdates: () => ipcRenderer.invoke('update:check'),
    installUpdate: () => ipcRenderer.invoke('update:install'),
    openReleasePage: () => ipcRenderer.invoke('update:openReleasePage'),
    onUpdateState: (callback) => ipcRenderer.on('update:state', (_, data) => callback(data)),

    toggleCheat: (name, value) => ipcRenderer.invoke('cheat:toggle', name, value),
    getVersion: () => ipcRenderer.invoke('app:version')
});
