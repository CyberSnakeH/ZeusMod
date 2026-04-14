let isInjected = false;
let speedMultiplier = 2.0;
let lockedTime = 12.0;
let detectionInterval = null;
let activeUpdateState = null;

const UPDATE_STATUS_LABELS = {
    idle: 'Waiting for update checks.',
    checking: 'Checking for updates...',
    'up-to-date': 'You are running the latest version.',
    available: 'A new version is available.',
    downloading: 'Downloading the latest installer...',
    downloaded: 'Installer downloaded. Restarting to install...',
    error: 'Update check failed.'
};

function setAttachButton({ label, icon, injected = false, disabled = false }) {
    const button = document.getElementById('btn-play');
    button.innerHTML = `<span class="play-icon">${icon}</span><span>${label}</span>`;
    button.classList.toggle('injected', injected);
    button.disabled = disabled;
}

function setStatus(kind, text) {
    const dot = document.getElementById('status-dot');
    const label = document.getElementById('status-text');
    dot.className = 'status-dot';
    if (kind === 'connected') dot.classList.add('connected');
    if (kind === 'searching') dot.classList.add('searching');
    if (kind === 'warning') dot.classList.add('warning');
    label.textContent = text;
}

function formatReleaseNotes(notes) {
    return String(notes || '')
        .split('\n')
        .map((line) => line.trim())
        .filter(Boolean)
        .slice(0, 12)
        .join('\n');
}

function renderUpdateState(state) {
    activeUpdateState = state;
    const statusLabel = document.getElementById('update-status');
    const checkButton = document.getElementById('btn-check-updates');
    const versionBadge = document.getElementById('update-chip');

    statusLabel.textContent = state.error || UPDATE_STATUS_LABELS[state.status] || 'Update status unavailable.';
    versionBadge.textContent = state.releaseTag || `v${state.latestVersion || ''}`;
    versionBadge.className = 'update-chip';

    if (state.status === 'available') versionBadge.classList.add('available');
    if (state.status === 'downloading') versionBadge.classList.add('downloading');
    if (state.status === 'up-to-date') versionBadge.classList.add('current');
    if (state.status === 'error') versionBadge.classList.add('error');

    checkButton.disabled = state.status === 'checking' || state.status === 'downloading';

    if (state.status === 'available' || state.status === 'downloading' || state.status === 'downloaded') {
        showUpdateModal(state);
    }
}

async function init() {
    const version = await window.zeusmod.getVersion();
    document.getElementById('app-version').textContent = `v${version}`;

    await refreshGameStatus();
    startDetection();

    renderUpdateState(await window.zeusmod.getUpdateState());
    window.zeusmod.onUpdateState((state) => renderUpdateState(state));
}

document.getElementById('btn-min').addEventListener('click', () => window.zeusmod.minimize());
document.getElementById('btn-max').addEventListener('click', () => window.zeusmod.maximize());
document.getElementById('btn-close').addEventListener('click', () => window.zeusmod.close());

document.getElementById('btn-check-updates').addEventListener('click', async () => {
    renderUpdateState({ ...(activeUpdateState || {}), status: 'checking', error: null });
    await window.zeusmod.checkForUpdates();
});

async function refreshGameStatus() {
    const status = await window.zeusmod.getGameStatus();

    document.getElementById('panel-game-title').textContent = 'Icarus';
    document.getElementById('install-path').textContent = status.installed
        ? status.installDir
        : 'Steam installation not detected.';

    if (!status.installed) {
        setStatus('warning', 'Icarus is not installed or could not be located.');
        setAttachButton({ label: 'ATTACH TO ICARUS', icon: '&#x25B6;', disabled: true });
        return;
    }

    if (status.running) {
        setStatus(isInjected ? 'connected' : 'searching', isInjected
            ? 'Connected. Cheats are live.'
            : 'Game detected. Ready to attach.');
        setAttachButton({
            label: isInjected ? 'CONNECTED' : 'ATTACH TO ICARUS',
            icon: isInjected ? '&#x2714;' : '&#x25B6;',
            injected: isInjected,
            disabled: isInjected
        });
        return;
    }

    setStatus('warning', 'Launch Icarus first, then attach.');
    setAttachButton({ label: 'ATTACH TO ICARUS', icon: '&#x25B6;' });
}

function startDetection() {
    if (detectionInterval) clearInterval(detectionInterval);
    detectionInterval = setInterval(() => {
        if (!isInjected) {
            void refreshGameStatus();
        }
    }, 3000);
}

document.getElementById('btn-play').addEventListener('click', async () => {
    const button = document.getElementById('btn-play');
    if (isInjected || button.disabled) return;

    setAttachButton({ label: 'INJECTING...', icon: '&#x23F3;', disabled: true });
    setStatus('searching', 'Injection in progress...');

    try {
        const result = await window.zeusmod.injectGame();
        if (result.success) {
            isInjected = true;
            setAttachButton({ label: 'CONNECTED', icon: '&#x2714;', injected: true, disabled: true });
            setStatus('connected', 'Connected. Cheats are live.');
        } else {
            setAttachButton({ label: 'ATTACH TO ICARUS', icon: '&#x25B6;' });
            setStatus('warning', result.error || 'Injection failed.');
        }
    } catch (error) {
        setAttachButton({ label: 'ATTACH TO ICARUS', icon: '&#x25B6;' });
        setStatus('warning', `Error: ${error.message}`);
    }
});

document.querySelectorAll('[data-cheat-toggle]').forEach((toggle) => {
    toggle.addEventListener('change', async (event) => {
        const cheat = event.target.dataset.cheatToggle;
        const enabled = event.target.checked;
        const row = event.target.closest('.cheat-row');
        row.classList.toggle('active', enabled);

        const result = await window.zeusmod.toggleCheat(cheat, enabled ? '1' : '0');
        if (!result.success) {
            event.target.checked = false;
            row.classList.remove('active');
            setStatus('warning', result.error || 'Unable to reach the injected module.');
        }
    });
});

document.getElementById('speed-down').addEventListener('click', async () => {
    speedMultiplier = Math.max(0.5, speedMultiplier - 0.5);
    document.getElementById('speed-value').textContent = `x${speedMultiplier.toFixed(1)}`;
    await window.zeusmod.toggleCheat('speed_mult', speedMultiplier.toString());
});

document.getElementById('speed-up').addEventListener('click', async () => {
    speedMultiplier = Math.min(10.0, speedMultiplier + 0.5);
    document.getElementById('speed-value').textContent = `x${speedMultiplier.toFixed(1)}`;
    await window.zeusmod.toggleCheat('speed_mult', speedMultiplier.toString());
});

document.getElementById('time-value').addEventListener('click', async () => {
    const times = [0, 6, 12, 18];
    const labels = ['00:00', '06:00', '12:00', '18:00'];
    let index = times.indexOf(lockedTime);
    index = (index + 1) % times.length;
    lockedTime = times[index];
    document.getElementById('time-value').textContent = labels[index];
    await window.zeusmod.toggleCheat('time_val', lockedTime.toString());
});

function showUpdateModal(state) {
    const modal = document.getElementById('update-modal');
    const version = document.getElementById('update-version');
    const notes = document.getElementById('update-notes');
    const progress = document.getElementById('update-progress');
    const downloadButton = document.getElementById('update-download');
    const openPageButton = document.getElementById('update-open-page');

    version.textContent = state.releaseTag || `v${state.latestVersion || ''}`;
    notes.textContent = formatReleaseNotes(state.notes);
    progress.textContent = state.status === 'downloading'
        ? `Downloading ${state.progress || 0}%`
        : state.status === 'downloaded'
            ? 'Download complete. Restarting...'
            : state.error || '';

    downloadButton.disabled = state.status === 'downloading' || state.status === 'downloaded';
    downloadButton.textContent = state.status === 'downloading'
        ? 'Downloading...'
        : state.status === 'downloaded'
            ? 'Installing...'
            : 'Download & Install';

    downloadButton.onclick = async () => {
        const result = await window.zeusmod.installUpdate();
        if (!result.success) {
            progress.textContent = result.error || 'Update download failed.';
        }
    };

    openPageButton.onclick = async () => {
        await window.zeusmod.openReleasePage();
    };

    document.getElementById('update-later').onclick = () => {
        modal.style.display = 'none';
    };

    modal.style.display = 'flex';
}

init();
