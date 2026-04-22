const { app, BrowserWindow, ipcMain, shell } = require('electron');
const { autoUpdater } = require('electron-updater');
const path = require('path');
const { execFile, exec } = require('child_process');
const { injectDll, InjectError } = require('./injector');
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
        icon: path.join(__dirname, '..', 'assets', 'icon.ico')
    });

    mainWindow.loadFile(path.join(__dirname, '..', 'renderer', 'index.html'));
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

function normalizeNotes(notes) {
    if (Array.isArray(notes)) {
        return notes.map((n) => n?.note || '').filter(Boolean).join('\n\n').trim()
            || 'Bug fixes, quality improvements, and packaging updates.';
    }
    const text = String(notes || '').replace(/\r/g, '').trim();
    return text || 'Bug fixes, quality improvements, and packaging updates.';
}

function fetchReleaseNotesFromGitHub(version) {
    return new Promise((resolve) => {
        const tag = `v${String(version).replace(/^v/i, '')}`;
        const url = `https://api.github.com/repos/${GITHUB_REPO}/releases/tags/${tag}`;
        const req = https.get(url, { headers: { 'User-Agent': UPDATE_USER_AGENT, Accept: 'application/vnd.github+json' } }, (res) => {
            if (res.statusCode && res.statusCode >= 400) { res.resume(); return resolve(null); }
            let data = '';
            res.on('data', (c) => { data += c; });
            res.on('end', () => {
                try { resolve(JSON.parse(data)); } catch { resolve(null); }
            });
        });
        req.on('error', () => resolve(null));
        req.setTimeout(10000, () => { req.destroy(); resolve(null); });
    });
}

autoUpdater.autoDownload = false;
autoUpdater.autoInstallOnAppQuit = false;
autoUpdater.allowPrerelease = false;
autoUpdater.allowDowngrade = false;

autoUpdater.on('checking-for-update', () => {
    setUpdateState({ status: 'checking', progress: 0, error: null });
});

autoUpdater.on('update-available', async (info) => {
    const latestVersion = String(info?.version || APP_VERSION);
    const release = await fetchReleaseNotesFromGitHub(latestVersion);
    setUpdateState({
        status: 'available',
        latestVersion,
        releaseTag: `v${latestVersion}`,
        notes: normalizeNotes(release?.body || info?.releaseNotes),
        releaseUrl: release?.html_url || `https://github.com/${GITHUB_REPO}/releases/tag/v${latestVersion}`,
        progress: 0,
        checkedAt: new Date().toISOString(),
        error: null
    });
});

autoUpdater.on('update-not-available', (info) => {
    setUpdateState({
        status: 'up-to-date',
        latestVersion: String(info?.version || APP_VERSION),
        releaseTag: `v${info?.version || APP_VERSION}`,
        progress: 0,
        checkedAt: new Date().toISOString(),
        error: null
    });
});

autoUpdater.on('download-progress', (progress) => {
    setUpdateState({
        status: 'downloading',
        progress: Math.min(100, Math.max(1, Math.round(progress?.percent || 0)))
    });
});

autoUpdater.on('update-downloaded', () => {
    setUpdateState({ status: 'downloaded', progress: 100, error: null });
});

autoUpdater.on('error', (error) => {
    setUpdateState({
        status: 'error',
        progress: 0,
        error: error?.message || String(error) || 'Update error.',
        checkedAt: new Date().toISOString()
    });
});

async function checkForUpdates() {
    if (updateCheckInFlight) return updateCheckInFlight;

    updateCheckInFlight = (async () => {
        if (!app.isPackaged) {
            setUpdateState({
                status: 'up-to-date',
                latestVersion: APP_VERSION,
                releaseTag: `v${APP_VERSION}`,
                checkedAt: new Date().toISOString(),
                progress: 0,
                error: null
            });
            return serializeUpdateState();
        }
        try {
            await autoUpdater.checkForUpdates();
        } catch (error) {
            setUpdateState({
                status: 'error',
                checkedAt: new Date().toISOString(),
                progress: 0,
                error: error?.message || 'Unable to check for updates.'
            });
        } finally {
            updateCheckInFlight = null;
        }
        return serializeUpdateState();
    })();

    return updateCheckInFlight;
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

// Resolve `processName` (e.g. "Icarus-Win64-Shipping.exe") to a PID.
// Returns 0 if not running. Uses tasklist /FO CSV so parsing is robust
// against locale differences (unlike the old `ps.ProcessName` approach
// in PowerShell which varies by language pack).
function findProcessPid(processName) {
    return new Promise((resolve) => {
        exec(`tasklist /FI "IMAGENAME eq ${processName}" /FO CSV /NH`,
            { windowsHide: true },
            (err, stdout) => {
                if (err || !stdout) return resolve(0);
                // CSV line: "Name","PID","Session","SessionNum","Mem"
                const line = String(stdout).split(/\r?\n/).find(l => l.includes(processName));
                if (!line) return resolve(0);
                const m = line.match(/"[^"]*","(\d+)"/);
                resolve(m ? parseInt(m[1], 10) : 0);
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

// Native in-process DLL injection. Uses the koffi FFI binding in
// ./injector.js — no PowerShell, no child process, no Add-Type JIT.
// Typical end-to-end latency: 50-150 ms (versus ~1 s for the old
// PowerShell path).
ipcMain.handle('game:inject', async () => {
    const baseDir = app.isPackaged ? process.resourcesPath : path.join(__dirname, '..', '..');
    const dllPath = path.join(baseDir, 'bin', 'IcarusInternal.dll');

    if (!fs.existsSync(dllPath)) {
        return { success: false, error: 'IcarusInternal.dll was not found in bin/.' };
    }

    const pid = await findProcessPid(ICARUS_PROCESS);
    if (!pid) {
        return { success: false, error: `${ICARUS_PROCESS} is not running — launch Icarus first.` };
    }

    try {
        const result = injectDll(pid, dllPath);
        return {
            success: true,
            output: `OK: module base 0x${result.moduleBase.toString(16)}`,
            pid,
            moduleBase: result.moduleBase
        };
    } catch (err) {
        const msg = err instanceof InjectError
            ? `${err.code}: ${err.message}`
            : String(err && err.message || err);
        return { success: false, error: msg };
    }
});

ipcMain.handle('update:getState', () => serializeUpdateState());
ipcMain.handle('update:check', async () => checkForUpdates());
ipcMain.handle('update:openReleasePage', async () => {
    const targetUrl = updateState.releaseUrl || GITHUB_RELEASES_URL;
    await shell.openExternal(targetUrl);
    return { success: true };
});

ipcMain.handle('update:install', async () => {
    if (!app.isPackaged) {
        return { success: false, error: 'Updates only work in the packaged app.' };
    }
    if (updateState.status !== 'available' && updateState.status !== 'downloaded' && updateState.status !== 'error') {
        return { success: false, error: `Not ready to install (status: ${updateState.status}).` };
    }

    try {
        if (updateState.status !== 'downloaded') {
            setUpdateState({ status: 'downloading', progress: 0, error: null });
            await autoUpdater.downloadUpdate();
        }
        setTimeout(() => {
            try {
                autoUpdater.quitAndInstall(true, true);
            } catch (err) {
                setUpdateState({
                    status: 'error',
                    progress: 0,
                    error: err?.message || 'Failed to launch the installer.'
                });
            }
        }, 600);
        return { success: true };
    } catch (error) {
        setUpdateState({
            status: 'error',
            progress: 0,
            error: error?.message || 'Failed to download the update.'
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
