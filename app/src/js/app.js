// ============================================================================
// ZeusMod App - Frontend Logic
// ============================================================================

let isInjected = false;
let speedMultiplier = 2.0;
let lockedTime = 12.0;
let selectedGame = null;
let allGames = [];

// ── Init ──
async function init() {
    const version = await window.zeusmod.getVersion();
    document.getElementById('app-version').textContent = `v${version}`;

    // Load Steam games
    await loadGames();

    // Setup update listener
    window.zeusmod.onUpdateAvailable((data) => showUpdateModal(data));
}

// ── Window Controls ──
document.getElementById('btn-min').addEventListener('click', () => window.zeusmod.minimize());
document.getElementById('btn-max').addEventListener('click', () => window.zeusmod.maximize());
document.getElementById('btn-close').addEventListener('click', () => window.zeusmod.close());

// ============================================================================
// Steam Game Library
// ============================================================================

async function loadGames() {
    const games = await window.zeusmod.getGames();
    allGames = games;
    renderGameList(games);
    document.getElementById('game-count').textContent = games.length;

    // Auto-select Icarus if found
    const icarus = games.find(g => g.supported);
    if (icarus) selectGame(icarus);
}

function renderGameList(games) {
    const container = document.getElementById('games-list');
    container.innerHTML = '';

    if (games.length === 0) {
        container.innerHTML = '<div class="loading-games">No Steam games found</div>';
        return;
    }

    games.forEach(game => {
        const el = document.createElement('div');
        el.className = `game-item ${selectedGame?.appId === game.appId ? 'active' : ''}`;
        el.dataset.appId = game.appId;

        const badgeClass = game.supported ? 'supported' : 'unsupported';
        const badgeText = game.supported ? 'READY' : '';

        el.innerHTML = `
            <div class="game-icon">${game.supported ? '&#x1F3AE;' : '&#x1F4C1;'}</div>
            <div class="game-info">
                <div class="game-name">${escapeHtml(game.name)}</div>
                <div class="game-status">${game.supported ?
                    '<span class="game-badge-small supported">SUPPORTED</span>' :
                    '<span class="game-badge-small unsupported">NOT SUPPORTED</span>'
                }</div>
            </div>
        `;

        el.addEventListener('click', () => selectGame(game));
        container.appendChild(el);
    });
}

function selectGame(game) {
    selectedGame = game;

    // Update sidebar active state
    document.querySelectorAll('.game-item').forEach(el => {
        el.classList.toggle('active', el.dataset.appId === game.appId);
    });

    if (game.supported) {
        document.getElementById('empty-state').style.display = 'none';
        document.getElementById('unsupported-state').style.display = 'none';
        document.getElementById('game-panel').style.display = 'flex';
        document.getElementById('panel-game-title').textContent = `⚡ ZeusMod for ${game.name}`;

        // Start process detection
        if (game.process) startDetection(game.process);
    } else {
        document.getElementById('empty-state').style.display = 'none';
        document.getElementById('game-panel').style.display = 'none';
        document.getElementById('unsupported-state').style.display = 'flex';
        document.getElementById('unsupported-name').textContent = game.name;
    }
}

// ── Search ──
document.getElementById('game-search').addEventListener('input', (e) => {
    const query = e.target.value.toLowerCase();
    const filtered = allGames.filter(g => g.name.toLowerCase().includes(query));
    renderGameList(filtered);
});

// ============================================================================
// Game Detection
// ============================================================================

let detectionInterval = null;

function startDetection(processName) {
    if (detectionInterval) clearInterval(detectionInterval);

    async function check() {
        if (isInjected) return;
        const running = await window.zeusmod.detectGame(processName);
        const dot = document.getElementById('status-dot');
        const text = document.getElementById('status-text');

        if (running) {
            dot.className = 'status-dot connected';
            text.textContent = 'Game detected - Click ATTACH';
        } else {
            dot.className = 'status-dot';
            text.textContent = 'Launch the game first';
        }
    }

    check();
    detectionInterval = setInterval(check, 3000);
}

// ============================================================================
// Injection
// ============================================================================

document.getElementById('btn-play').addEventListener('click', async () => {
    const btn = document.getElementById('btn-play');
    if (isInjected) return;

    btn.innerHTML = '<span class="play-icon">&#x23F3;</span> <span>INJECTING...</span>';
    document.getElementById('status-dot').className = 'status-dot searching';
    document.getElementById('status-text').textContent = 'Injecting...';

    try {
        const result = await window.zeusmod.injectGame();
        if (result.success) {
            isInjected = true;
            btn.innerHTML = '<span class="play-icon">&#x2714;</span> <span>CONNECTED</span>';
            btn.classList.add('injected');
            document.getElementById('status-dot').className = 'status-dot connected';
            document.getElementById('status-text').textContent = 'Connected - Cheats active';
        } else {
            btn.innerHTML = '<span class="play-icon">&#x25B6;</span> <span>ATTACH</span>';
            document.getElementById('status-dot').className = 'status-dot';
            document.getElementById('status-text').textContent = result.error || 'Injection failed';
        }
    } catch (err) {
        btn.innerHTML = '<span class="play-icon">&#x25B6;</span> <span>ATTACH</span>';
        document.getElementById('status-text').textContent = 'Error: ' + err.message;
    }
});

// ============================================================================
// Cheat Toggles
// ============================================================================

document.querySelectorAll('[data-cheat-toggle]').forEach(toggle => {
    toggle.addEventListener('change', async (e) => {
        const cheat = e.target.dataset.cheatToggle;
        const enabled = e.target.checked;
        const row = e.target.closest('.cheat-row');
        row.classList.toggle('active', enabled);

        // Send to DLL via named pipe
        const result = await window.zeusmod.toggleCheat(cheat, enabled ? '1' : '0');
        if (!result.success) {
            console.warn(`[CHEAT] Failed to toggle ${cheat}: ${result.error}`);
            // Revert toggle if pipe failed
            if (!isInjected) {
                e.target.checked = false;
                row.classList.remove('active');
            }
        }
        console.log(`[CHEAT] ${cheat}: ${enabled ? 'ON' : 'OFF'} (${result.success ? 'sent' : 'failed'})`);
    });
});

// ── Speed Controls ──
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

// ── Time Control ──
document.getElementById('time-value').addEventListener('click', async () => {
    const times = [0, 6, 12, 18];
    const labels = ['00:00', '06:00', '12:00', '18:00'];
    let idx = times.indexOf(lockedTime);
    idx = (idx + 1) % times.length;
    lockedTime = times[idx];
    document.getElementById('time-value').textContent = labels[idx];
    await window.zeusmod.toggleCheat('time_val', lockedTime.toString());
});

// ============================================================================
// Update Modal
// ============================================================================

function showUpdateModal(data) {
    document.getElementById('update-version').textContent = data.version;
    document.getElementById('update-notes').textContent = data.notes || 'Bug fixes and improvements';
    document.getElementById('update-modal').style.display = 'flex';

    document.getElementById('update-download').onclick = () => {
        window.zeusmod.downloadUpdate(data.downloadUrl);
        document.getElementById('update-modal').style.display = 'none';
    };

    document.getElementById('update-later').onclick = () => {
        document.getElementById('update-modal').style.display = 'none';
    };
}

// ============================================================================
// Utils
// ============================================================================

function escapeHtml(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
}

// ── Start ──
init();
