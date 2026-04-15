const { app, BrowserWindow, ipcMain, shell } = require('electron');
const path = require('path');
const { execFile, exec } = require('child_process');
const fs = require('fs');
const os = require('os');
const https = require('https');
const net = require('net');

const APP_VERSION = app.getVersion();
const APP_NAME = app.getName();
const GITHUB_REPO = 'CyberSnakeH/ZeusMod';
const GITHUB_RELEASES_URL = `https://github.com/${GITHUB_REPO}/releases`;
const GITHUB_LATEST_API = `https://api.github.com/repos/${GITHUB_REPO}/releases/latest`;
const UPDATE_CHECK_INTERVAL_MS = 6 * 60 * 60 * 1000;
const UPDATE_USER_AGENT = `${APP_NAME}-Updater`;

const ICARUS_APP_ID = '1149460';
const ICARUS_PROCESS = 'Icarus-Win64-Shipping.exe';

let mainWindow = null;
let updateCheckTimer = null;
let updateCheckInFlight = null;

const updateState = {
    status: 'idle',
    currentVersion: APP_VERSION,
    latestVersion: APP_VERSION,
    releaseTag: `v${APP_VERSION}`,
    notes: '',
    releaseUrl: GITHUB_RELEASES_URL,
    downloadUrl: null,
    fileName: null,
    progress: 0,
    checkedAt: null,
    error: null
};

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
        icon: path.join(__dirname, 'src/assets/icon.ico')
    });

    mainWindow.loadFile('src/index.html');
    mainWindow.setMenuBarVisibility(false);
    mainWindow.webContents.on('did-finish-load', () => {
        emitUpdateState();
        void checkForUpdates();
        scheduleUpdateChecks();
    });
}

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());
app.on('before-quit', () => {
    if (updateCheckTimer) {
        clearInterval(updateCheckTimer);
        updateCheckTimer = null;
    }
});

ipcMain.on('window:minimize', () => mainWindow?.minimize());
ipcMain.on('window:maximize', () => {
    if (mainWindow?.isMaximized()) {
        mainWindow.unmaximize();
    } else {
        mainWindow?.maximize();
    }
});
ipcMain.on('window:close', () => mainWindow?.close());

function serializeUpdateState() {
    return { ...updateState };
}

function emitUpdateState() {
    mainWindow?.webContents.send('update:state', serializeUpdateState());
}

function setUpdateState(patch) {
    Object.assign(updateState, patch);
    emitUpdateState();
    return serializeUpdateState();
}

function scheduleUpdateChecks() {
    if (updateCheckTimer) clearInterval(updateCheckTimer);
    updateCheckTimer = setInterval(() => {
        void checkForUpdates();
    }, UPDATE_CHECK_INTERVAL_MS);
}

function normalizeVersion(version) {
    return String(version || '')
        .trim()
        .replace(/^v/i, '')
        .replace(/[^\d.].*$/, '');
}

function isNewerVersion(latest, current) {
    const lhs = normalizeVersion(latest).split('.').map((value) => Number(value) || 0);
    const rhs = normalizeVersion(current).split('.').map((value) => Number(value) || 0);
    const max = Math.max(lhs.length, rhs.length, 3);

    for (let i = 0; i < max; i++) {
        const l = lhs[i] || 0;
        const r = rhs[i] || 0;
        if (l > r) return true;
        if (l < r) return false;
    }
    return false;
}

function normalizeNotes(notes) {
    const text = String(notes || '').replace(/\r/g, '').trim();
    return text || 'Bug fixes, quality improvements, and packaging updates.';
}

function pickInstallerAsset(release) {
    const assets = Array.isArray(release.assets) ? release.assets : [];
    const executables = assets.filter((asset) => {
        const name = String(asset.name || '').toLowerCase();
        return name.endsWith('.exe') && !name.endsWith('.blockmap') && !name.includes('portable');
    });

    const setupMatch = executables.find((asset) =>
        String(asset.name || '').toLowerCase().includes('setup')
    );
    return setupMatch || executables[0] || null;
}

function requestJson(url, headers = {}) {
    return new Promise((resolve, reject) => {
        const request = https.get(url, { headers }, (response) => {
            if (response.statusCode && response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
                response.resume();
                resolve(requestJson(response.headers.location, headers));
                return;
            }

            let data = '';
            response.on('data', (chunk) => {
                data += chunk;
            });
            response.on('end', () => {
                if (response.statusCode && response.statusCode >= 400) {
                    reject(new Error(`HTTP ${response.statusCode}`));
                    return;
                }

                try {
                    resolve(JSON.parse(data));
                } catch (error) {
                    reject(error);
                }
            });
        });

        request.on('error', reject);
    });
}

async function checkForUpdates() {
    if (updateCheckInFlight) return updateCheckInFlight;

    updateCheckInFlight = (async () => {
        setUpdateState({
            status: 'checking',
            progress: 0,
            error: null
        });

        try {
            const release = await requestJson(GITHUB_LATEST_API, {
                'User-Agent': UPDATE_USER_AGENT,
                Accept: 'application/vnd.github+json'
            });

            const latestVersion = normalizeVersion(release.tag_name);
            if (!latestVersion) {
                throw new Error('Invalid release version received from GitHub.');
            }

            const installerAsset = pickInstallerAsset(release);
            const commonPatch = {
                latestVersion,
                releaseTag: String(release.tag_name || `v${latestVersion}`),
                notes: normalizeNotes(release.body),
                releaseUrl: release.html_url || GITHUB_RELEASES_URL,
                downloadUrl: installerAsset?.browser_download_url || null,
                fileName: installerAsset?.name || null,
                checkedAt: new Date().toISOString(),
                error: null
            };

            if (isNewerVersion(latestVersion, APP_VERSION)) {
                return setUpdateState({
                    ...commonPatch,
                    status: 'available',
                    progress: 0
                });
            }

            return setUpdateState({
                ...commonPatch,
                status: 'up-to-date',
                downloadUrl: null,
                fileName: null,
                progress: 0
            });
        } catch (error) {
            return setUpdateState({
                status: 'error',
                checkedAt: new Date().toISOString(),
                progress: 0,
                error: error.message || 'Unable to check for updates.'
            });
        } finally {
            updateCheckInFlight = null;
        }
    })();

    return updateCheckInFlight;
}

function downloadFile(url, destination) {
    return new Promise((resolve, reject) => {
        const temporaryDestination = `${destination}.download`;

        try {
            if (fs.existsSync(temporaryDestination)) fs.unlinkSync(temporaryDestination);
            if (fs.existsSync(destination)) fs.unlinkSync(destination);
        } catch {}

        const request = https.get(url, { headers: { 'User-Agent': UPDATE_USER_AGENT } }, (response) => {
            if (response.statusCode && response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
                response.resume();
                resolve(downloadFile(response.headers.location, destination));
                return;
            }

            if (response.statusCode && response.statusCode >= 400) {
                reject(new Error(`HTTP ${response.statusCode}`));
                return;
            }

            const totalBytes = Number(response.headers['content-length'] || 0);
            let receivedBytes = 0;
            const fileStream = fs.createWriteStream(temporaryDestination);

            response.on('data', (chunk) => {
                receivedBytes += chunk.length;
                if (totalBytes > 0) {
                    setUpdateState({
                        status: 'downloading',
                        progress: Math.min(100, Math.max(1, Math.round((receivedBytes / totalBytes) * 100)))
                    });
                }
            });

            response.pipe(fileStream);

            fileStream.on('finish', () => {
                fileStream.close(() => {
                    fs.rename(temporaryDestination, destination, (renameError) => {
                        if (renameError) {
                            reject(renameError);
                            return;
                        }
                        resolve(destination);
                    });
                });
            });

            fileStream.on('error', (streamError) => {
                try {
                    fileStream.close(() => {
                        if (fs.existsSync(temporaryDestination)) fs.unlinkSync(temporaryDestination);
                    });
                } catch {}
                reject(streamError);
            });
        });

        request.on('error', (error) => {
            try {
                if (fs.existsSync(temporaryDestination)) fs.unlinkSync(temporaryDestination);
            } catch {}
            reject(error);
        });
    });
}

function findSteamPath() {
    const possiblePaths = [
        'C:\\Program Files (x86)\\Steam',
        'C:\\Program Files\\Steam',
        'D:\\Steam',
        'D:\\SteamLibrary',
        'E:\\Steam',
        'E:\\SteamLibrary'
    ];

    for (const steamPath of possiblePaths) {
        if (fs.existsSync(path.join(steamPath, 'steam.exe')) || fs.existsSync(path.join(steamPath, 'steamapps'))) {
            return steamPath;
        }
    }

    return null;
}

function findSteamLibraries(steamPath) {
    const libraries = [steamPath];
    const vdfPath = path.join(steamPath, 'steamapps', 'libraryfolders.vdf');

    if (!fs.existsSync(vdfPath)) return libraries;

    try {
        const content = fs.readFileSync(vdfPath, 'utf-8');
        const pathMatches = content.match(/"path"\s+"([^"]+)"/g);
        if (pathMatches) {
            for (const match of pathMatches) {
                const libraryPath = match.match(/"path"\s+"([^"]+)"/)?.[1]?.replace(/\\\\/g, '\\');
                if (libraryPath && !libraries.includes(libraryPath)) libraries.push(libraryPath);
            }
        }
    } catch {}

    return libraries;
}

function locateIcarusInstall() {
    const steamPath = findSteamPath();
    if (!steamPath) return null;

    for (const libraryPath of findSteamLibraries(steamPath)) {
        const steamAppsPath = path.join(libraryPath, 'steamapps');
        const manifestPath = path.join(steamAppsPath, `appmanifest_${ICARUS_APP_ID}.acf`);
        if (!fs.existsSync(manifestPath)) continue;

        try {
            const content = fs.readFileSync(manifestPath, 'utf-8');
            const installDir = content.match(/"installdir"\s+"([^"]+)"/)?.[1];
            if (!installDir) continue;

            const fullPath = path.join(steamAppsPath, 'common', installDir);
            if (fs.existsSync(fullPath)) return fullPath;
        } catch {}
    }

    return null;
}

function detectRunningProcess(processName) {
    return new Promise((resolve) => {
        exec(`tasklist /FI "IMAGENAME eq ${processName}" /FO CSV /NH`, (error, stdout) => {
            resolve(String(stdout || '').includes(processName));
        });
    });
}

ipcMain.handle('game:getStatus', async () => {
    const installDir = locateIcarusInstall();
    const running = await detectRunningProcess(ICARUS_PROCESS);
    return {
        appId: ICARUS_APP_ID,
        name: 'Icarus',
        process: ICARUS_PROCESS,
        installDir,
        installed: !!installDir,
        running
    };
});

ipcMain.handle('game:inject', async () => {
    const baseDir = app.isPackaged ? process.resourcesPath : __dirname;
    const dllPath = path.join(baseDir, 'bin', 'IcarusInternal.dll');
    const scriptPath = path.join(baseDir, 'scripts', 'inject.ps1');

    return new Promise((resolve) => {
        if (!fs.existsSync(dllPath)) {
            resolve({ success: false, error: 'IcarusInternal.dll was not found in bin/.' });
            return;
        }
        if (!fs.existsSync(scriptPath)) {
            resolve({ success: false, error: 'inject.ps1 was not found in scripts/.' });
            return;
        }

        const args = [
            '-NoProfile',
            '-ExecutionPolicy', 'Bypass',
            '-File', scriptPath,
            '-ProcessName', ICARUS_PROCESS,
            '-DllPath', dllPath
        ];

        execFile('powershell.exe', args, { timeout: 30000, windowsHide: true }, (error, stdout, stderr) => {
            const output = String(stdout || stderr || '').trim();
            if (error) {
                resolve({ success: false, error: output || error.message });
                return;
            }
            if (!output.startsWith('OK:')) {
                resolve({ success: false, error: output || 'Injection failed.' });
                return;
            }
            resolve({ success: true, output });
        });
    });
});

ipcMain.handle('update:getState', () => serializeUpdateState());
ipcMain.handle('update:check', async () => checkForUpdates());
ipcMain.handle('update:openReleasePage', async () => {
    const targetUrl = updateState.releaseUrl || GITHUB_RELEASES_URL;
    await shell.openExternal(targetUrl);
    return { success: true };
});

ipcMain.handle('update:install', async () => {
    if (!updateState.downloadUrl) {
        return { success: false, error: 'No downloadable installer is available for this release.' };
    }

    try {
        const tempDir = path.join(os.tmpdir(), 'ZeusMod');
        fs.mkdirSync(tempDir, { recursive: true });
        const fileName = updateState.fileName || `${APP_NAME}-Setup-${updateState.latestVersion}.exe`;
        const installerPath = path.join(tempDir, fileName);

        setUpdateState({
            status: 'downloading',
            progress: 0,
            error: null
        });

        await downloadFile(updateState.downloadUrl, installerPath);
        setUpdateState({
            status: 'downloaded',
            progress: 100,
            error: null
        });

        setTimeout(() => {
            const child = execFile(installerPath, [], {
                detached: true,
                stdio: 'ignore'
            });
            child.unref();
            app.quit();
        }, 400);

        return { success: true };
    } catch (error) {
        setUpdateState({
            status: 'error',
            progress: 0,
            error: error.message || 'Failed to download the update.'
        });
        return { success: false, error: error.message || 'Failed to download the update.' };
    }
});

ipcMain.handle('app:version', () => APP_VERSION);

ipcMain.handle('cheat:toggle', async (_, cheatName, value) => {
    return new Promise((resolve) => {
        const pipePath = '\\\\.\\pipe\\ZeusModPipe';
        const client = net.connect(pipePath, () => {
            client.write(`${cheatName}:${value}`);
        });

        client.on('data', (data) => {
            client.end();
            resolve({ success: true, response: data.toString() });
        });

        client.on('error', (error) => {
            resolve({ success: false, error: error.message });
        });

        setTimeout(() => {
            client.destroy();
            resolve({ success: false, error: 'timeout' });
        }, 2000);
    });
});
