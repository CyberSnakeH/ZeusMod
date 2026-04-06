const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron');
const path = require('path');
const { execFile, exec } = require('child_process');
const fs = require('fs');
const https = require('https');

const APP_VERSION = '1.0.0';
const GITHUB_REPO = 'CyberSnakeH/ZeusMod';

let mainWindow;

// ============================================================================
// Window
// ============================================================================

function createWindow() {
    mainWindow = new BrowserWindow({
        width: 960,
        height: 680,
        minWidth: 850,
        minHeight: 600,
        frame: false,
        backgroundColor: '#08080f',
        webPreferences: {
            preload: path.join(__dirname, 'preload.js'),
            contextIsolation: true,
            nodeIntegration: false
        },
        icon: path.join(__dirname, 'src/assets/icon.png')
    });

    mainWindow.loadFile('src/index.html');
    mainWindow.setMenuBarVisibility(false);

    // Check for updates after window loads
    mainWindow.webContents.on('did-finish-load', () => {
        checkForUpdates();
    });
}

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());

// ============================================================================
// Window Controls
// ============================================================================

ipcMain.on('window:minimize', () => mainWindow?.minimize());
ipcMain.on('window:maximize', () => {
    if (mainWindow?.isMaximized()) mainWindow.unmaximize();
    else mainWindow?.maximize();
});
ipcMain.on('window:close', () => mainWindow?.close());

// ============================================================================
// Steam Game Detection
// ============================================================================

const SUPPORTED_GAMES = {
    '1149460': { name: 'Icarus', supported: true, process: 'Icarus-Win64-Shipping.exe' }
};

function findSteamPath() {
    const possiblePaths = [
        'C:\\Program Files (x86)\\Steam',
        'C:\\Program Files\\Steam',
        'D:\\Steam',
        'D:\\SteamLibrary',
        'E:\\Steam',
        'E:\\SteamLibrary'
    ];
    for (const p of possiblePaths) {
        if (fs.existsSync(path.join(p, 'steam.exe')) || fs.existsSync(path.join(p, 'steamapps'))) {
            return p;
        }
    }
    return null;
}

function findSteamLibraries(steamPath) {
    const libraries = [steamPath];
    const vdfPath = path.join(steamPath, 'steamapps', 'libraryfolders.vdf');

    if (fs.existsSync(vdfPath)) {
        try {
            const content = fs.readFileSync(vdfPath, 'utf-8');
            const pathMatches = content.match(/"path"\s+"([^"]+)"/g);
            if (pathMatches) {
                for (const match of pathMatches) {
                    const libPath = match.match(/"path"\s+"([^"]+)"/)[1].replace(/\\\\/g, '\\');
                    if (!libraries.includes(libPath)) libraries.push(libPath);
                }
            }
        } catch (e) {}
    }
    return libraries;
}

function scanInstalledGames() {
    const steamPath = findSteamPath();
    if (!steamPath) return [];

    const libraries = findSteamLibraries(steamPath);
    const games = [];

    for (const lib of libraries) {
        const appsDir = path.join(lib, 'steamapps');
        if (!fs.existsSync(appsDir)) continue;

        try {
            const files = fs.readdirSync(appsDir).filter(f => f.startsWith('appmanifest_') && f.endsWith('.acf'));

            for (const file of files) {
                try {
                    const content = fs.readFileSync(path.join(appsDir, file), 'utf-8');
                    const appId = content.match(/"appid"\s+"(\d+)"/)?.[1];
                    const name = content.match(/"name"\s+"([^"]+)"/)?.[1];
                    const installDir = content.match(/"installdir"\s+"([^"]+)"/)?.[1];

                    if (appId && name) {
                        const supportInfo = SUPPORTED_GAMES[appId];
                        games.push({
                            appId,
                            name,
                            installDir: installDir ? path.join(appsDir, 'common', installDir) : null,
                            supported: supportInfo?.supported || false,
                            process: supportInfo?.process || null
                        });
                    }
                } catch (e) {}
            }
        } catch (e) {}
    }

    // Sort: supported first, then alphabetical
    games.sort((a, b) => {
        if (a.supported !== b.supported) return b.supported - a.supported;
        return a.name.localeCompare(b.name);
    });

    return games;
}

ipcMain.handle('steam:getGames', async () => {
    return scanInstalledGames();
});

// ============================================================================
// Game Detection (running process)
// ============================================================================

ipcMain.handle('game:detect', async (_, processName) => {
    return new Promise((resolve) => {
        exec(`tasklist /FI "IMAGENAME eq ${processName}" /FO CSV /NH`, (err, stdout) => {
            resolve(stdout.includes(processName));
        });
    });
});

// ============================================================================
// Injection
// ============================================================================

ipcMain.handle('game:inject', async () => {
    const binDir = app.isPackaged
        ? path.join(process.resourcesPath, 'bin')
        : path.join(__dirname, 'bin');

    const injectorPath = path.join(binDir, 'IcarusInjector.exe');

    if (!fs.existsSync(injectorPath)) {
        return { success: false, error: 'IcarusInjector.exe not found in bin/' };
    }

    return new Promise((resolve) => {
        execFile(injectorPath, { timeout: 30000 }, (err, stdout, stderr) => {
            if (err) resolve({ success: false, error: err.message });
            else resolve({ success: true, output: stdout });
        });
    });
});

// ============================================================================
// Auto-Update via GitHub Releases
// ============================================================================

function checkForUpdates() {
    const options = {
        hostname: 'api.github.com',
        path: `/repos/${GITHUB_REPO}/releases/latest`,
        headers: { 'User-Agent': 'ZeusMod-Updater' }
    };

    https.get(options, (res) => {
        let data = '';
        res.on('data', chunk => data += chunk);
        res.on('end', () => {
            try {
                const release = JSON.parse(data);
                const latestVersion = release.tag_name?.replace('v', '') || '0';

                if (isNewerVersion(latestVersion, APP_VERSION)) {
                    const downloadUrl = release.assets?.[0]?.browser_download_url;
                    mainWindow?.webContents.send('update:available', {
                        version: release.tag_name,
                        notes: release.body,
                        downloadUrl: downloadUrl || release.html_url
                    });
                }
            } catch (e) {}
        });
    }).on('error', () => {});
}

function isNewerVersion(latest, current) {
    const l = latest.split('.').map(Number);
    const c = current.split('.').map(Number);
    for (let i = 0; i < 3; i++) {
        if ((l[i] || 0) > (c[i] || 0)) return true;
        if ((l[i] || 0) < (c[i] || 0)) return false;
    }
    return false;
}

ipcMain.on('update:download', (_, url) => {
    shell.openExternal(url);
});

ipcMain.handle('app:version', () => APP_VERSION);

// ============================================================================
// Named Pipe Communication with DLL
// ============================================================================

const net = require('net');

ipcMain.handle('cheat:toggle', async (_, cheatName, value) => {
    return new Promise((resolve) => {
        const pipePath = '\\\\.\\pipe\\ZeusModPipe';
        const client = net.connect(pipePath, () => {
            const msg = `${cheatName}:${value}`;
            client.write(msg);
        });

        client.on('data', (data) => {
            client.end();
            resolve({ success: true, response: data.toString() });
        });

        client.on('error', (err) => {
            resolve({ success: false, error: err.message });
        });

        setTimeout(() => {
            client.destroy();
            resolve({ success: false, error: 'timeout' });
        }, 2000);
    });
});
