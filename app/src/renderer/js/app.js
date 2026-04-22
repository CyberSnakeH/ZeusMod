// ============================================================================
// ZeusMod renderer — sidebar-driven category nav, live status strip, cheat
// cards, give-items panel. All IPC round-trips go through window.zeusmod.*
// exposed by preload.js.
// ============================================================================

let isInjected       = false;
let speedMultiplier  = 2.0;
let lockedTimeIdx    = 2;           // 12:00 default
const TIME_PRESETS   = [
    { v: 0,  label: '00:00' },
    { v: 6,  label: '06:00' },
    { v: 12, label: '12:00' },
    { v: 18, label: '18:00' }
];
let detectionTimer   = null;
let activeUpdateState = null;

const UPDATE_LABELS = {
    idle:          'Waiting for update checks',
    checking:      'Checking for updates…',
    'up-to-date':  'You are on the latest version',
    available:     'A new version is available',
    downloading:   'Downloading the latest installer…',
    downloaded:    'Download complete — restarting to install',
    error:         'Update check failed'
};

const $  = (id) => document.getElementById(id);
const $$ = (sel, root = document) => root.querySelectorAll(sel);

// ═════════════════════════ INIT ═════════════════════════
async function init() {
    const v = await window.zeusmod.getVersion();
    $('app-version').textContent = `v${v}`;

    wireTitlebar();
    wireSidebarNav();
    wireAttachButton();
    wireCheatToggles();
    wireSpeedStepper();
    wireTimeStepper();
    wireUpdateButtons();

    await refreshGameStatus();
    startDetection();

    renderUpdate(await window.zeusmod.getUpdateState());
    window.zeusmod.onUpdateState((s) => renderUpdate(s));
}

// ═════════════════════════ TITLEBAR ═════════════════════════
function wireTitlebar() {
    $('btn-min').addEventListener('click',   () => window.zeusmod.minimize());
    $('btn-max').addEventListener('click',   () => window.zeusmod.maximize());
    $('btn-close').addEventListener('click', () => window.zeusmod.close());
}

// ═════════════════════════ SIDEBAR NAVIGATION ═════════════════════════
function wireSidebarNav() {
    $$('.nav-item').forEach((btn) => {
        btn.addEventListener('click', () => {
            $$('.nav-item').forEach((b) => b.classList.toggle('active', b === btn));
            const cat = btn.dataset.cat;
            $$('.cat-panel').forEach((p) => {
                p.toggleAttribute('hidden', p.dataset.panel !== cat);
                p.classList.toggle('active', p.dataset.panel === cat);
            });
            // Progression is a placeholder that mirrors character cat for now
            if (cat === 'progression') mirrorCategory('character', 'progression');
        });
    });
}

function mirrorCategory(srcKey, dstKey) {
    const src = document.querySelector(`.cat-panel[data-panel="${srcKey}"] .card-grid`);
    const dst = document.querySelector(`.cat-panel[data-panel="${dstKey}"] .card-grid`);
    if (!src || !dst || dst.childElementCount > 0) return;
    // Clone the cards so toggles remain bound to the same data-cheat-toggle
    // but in distinct DOM nodes. We re-wire listeners for the clones below.
    src.querySelectorAll('.cheat-card').forEach((card) => {
        const clone = card.cloneNode(true);
        const input = clone.querySelector('input[data-cheat-toggle]');
        if (input) {
            input.addEventListener('change', (ev) => handleToggleChange(ev.target));
            // Mirror the live state
            const original = document.querySelector(
                `input[data-cheat-toggle="${input.dataset.cheatToggle}"]`);
            if (original) input.checked = original.checked;
            if (input.checked) clone.classList.add('active');
        }
        dst.appendChild(clone);
    });
}

// ═════════════════════════ CHEAT TOGGLES ═════════════════════════
function wireCheatToggles() {
    $$('input[data-cheat-toggle]').forEach((input) => {
        input.addEventListener('change', (ev) => handleToggleChange(ev.target));
    });
}

async function handleToggleChange(input) {
    const name    = input.dataset.cheatToggle;
    const enabled = input.checked;
    const card    = input.closest('.cheat-card');
    card?.classList.toggle('active', enabled);

    const res = await window.zeusmod.toggleCheat(name, enabled ? '1' : '0');
    if (!res?.success) {
        input.checked = false;
        card?.classList.remove('active');
        setStatus('warning', res?.error || 'Unable to reach the injected module');
    }
    // Keep mirrored clones (progression) in sync
    $$(`input[data-cheat-toggle="${name}"]`).forEach((el) => {
        if (el !== input) {
            el.checked = input.checked;
            el.closest('.cheat-card')?.classList.toggle('active', input.checked);
        }
    });
}

// ═════════════════════════ SPEED STEPPER ═════════════════════════
function wireSpeedStepper() {
    const apply = async () => {
        $('speed-value').textContent = `x${speedMultiplier.toFixed(1)}`;
        $('speed-subtitle').textContent = `Current: x${speedMultiplier.toFixed(1)}`;
        await window.zeusmod.toggleCheat('speed_mult', speedMultiplier.toString());
    };
    $('speed-down')?.addEventListener('click', () => {
        speedMultiplier = Math.max(0.5, +(speedMultiplier - 0.5).toFixed(1));
        apply();
    });
    $('speed-up')?.addEventListener('click', () => {
        speedMultiplier = Math.min(10.0, +(speedMultiplier + 0.5).toFixed(1));
        apply();
    });
}

// ═════════════════════════ TIME STEPPER ═════════════════════════
function wireTimeStepper() {
    const apply = async () => {
        const p = TIME_PRESETS[lockedTimeIdx];
        $('time-value').textContent = p.label;
        $('time-subtitle').textContent = `Pinned at ${p.label}`;
        await window.zeusmod.toggleCheat('time_val', p.v.toString());
    };
    $('time-prev')?.addEventListener('click', () => {
        lockedTimeIdx = (lockedTimeIdx - 1 + TIME_PRESETS.length) % TIME_PRESETS.length;
        apply();
    });
    $('time-next')?.addEventListener('click', () => {
        lockedTimeIdx = (lockedTimeIdx + 1) % TIME_PRESETS.length;
        apply();
    });
}

// ═════════════════════════ STATUS STRIP + FOOTER ═════════════════════════
function setStatus(kind, primary, secondary) {
    const dot  = $('status-dot');
    const pri  = $('status-primary') || $('status-text');
    const sec  = $('install-path');
    dot.className = 'status-dot';
    if (kind === 'connected')  dot.classList.add('connected');
    if (kind === 'searching')  dot.classList.add('searching');
    if (kind === 'warning')    dot.classList.add('warning');
    if (pri && primary !== undefined)   pri.textContent = primary;
    if (sec && secondary !== undefined) sec.textContent = secondary;

    const foot = $('footer-status');
    if (foot) {
        foot.className = 'footer-chip';
        if (kind === 'connected') { foot.textContent = 'Injected'; foot.classList.add('on'); }
        else if (kind === 'searching') { foot.textContent = 'Ready'; foot.classList.add('ok'); }
        else if (kind === 'warning') { foot.textContent = 'Offline'; foot.classList.add('warn'); }
        else { foot.textContent = 'Idle'; }
    }
}

function setAttachButton({ label, injected = false, disabled = false }) {
    const b = $('btn-play');
    b.classList.toggle('injected', injected);
    b.disabled = !!disabled;
    b.querySelector('.attach-label').textContent = label;
}

async function refreshGameStatus() {
    const status = await window.zeusmod.getGameStatus();
    $('game-tile-meta').textContent = status.installed
        ? `Steam · ${status.installDir ? 'installed' : 'not found'}`
        : 'Not installed';

    if (!status.installed) {
        setStatus('warning',
            'Icarus is not installed',
            'The Steam install for Icarus could not be located on this machine.');
        setAttachButton({ label: 'ATTACH TO ICARUS', disabled: true });
        return;
    }

    if (status.running) {
        setStatus(isInjected ? 'connected' : 'searching',
            isInjected ? 'Attached — cheats are live' : 'Game detected — ready to attach',
            status.installDir);
        setAttachButton({
            label: isInjected ? 'CONNECTED' : 'ATTACH TO ICARUS',
            injected: isInjected,
            disabled: isInjected
        });
        return;
    }

    setStatus('warning',
        'Launch Icarus first',
        status.installDir || '');
    setAttachButton({ label: 'ATTACH TO ICARUS' });
}

function startDetection() {
    if (detectionTimer) clearInterval(detectionTimer);
    detectionTimer = setInterval(() => { if (!isInjected) refreshGameStatus(); }, 3000);
}

// ═════════════════════════ ATTACH BUTTON ═════════════════════════
function wireAttachButton() {
    $('btn-play').addEventListener('click', async () => {
        if (isInjected || $('btn-play').disabled) return;

        setAttachButton({ label: 'INJECTING…', disabled: true });
        setStatus('searching', 'Injecting into Icarus-Win64-Shipping.exe…', '');

        try {
            const r = await window.zeusmod.injectGame();
            if (r.success) {
                isInjected = true;
                setAttachButton({ label: 'CONNECTED', injected: true, disabled: true });
                setStatus('connected', 'Attached — cheats are live', r.output || '');
            } else {
                setAttachButton({ label: 'ATTACH TO ICARUS' });
                setStatus('warning', 'Injection failed', r.error || '');
            }
        } catch (e) {
            setAttachButton({ label: 'ATTACH TO ICARUS' });
            setStatus('warning', 'Injection error', e.message);
        }
    });
}

// ═════════════════════════ UPDATES ═════════════════════════
function wireUpdateButtons() {
    $('btn-check-updates').addEventListener('click', async () => {
        renderUpdate({ ...(activeUpdateState || {}), status: 'checking', error: null });
        await window.zeusmod.checkForUpdates();
    });
}

function renderUpdate(state) {
    activeUpdateState = state;
    const chip = $('update-chip');
    const btn  = $('btn-check-updates');

    chip.textContent = state.releaseTag || (state.latestVersion ? `v${state.latestVersion}` : '—');
    chip.className = 'footer-chip';
    if (state.status === 'available')    chip.classList.add('on');
    if (state.status === 'downloading')  chip.classList.add('on');
    if (state.status === 'up-to-date')   chip.classList.add('ok');
    if (state.status === 'error')        chip.classList.add('err');

    btn.disabled = state.status === 'checking' || state.status === 'downloading';
    btn.textContent = state.status === 'checking'
        ? 'Checking…'
        : state.status === 'downloading'
            ? 'Downloading…'
            : 'Check for Updates';

    if (['available', 'downloading', 'downloaded'].includes(state.status)) showUpdateModal(state);
}

function showUpdateModal(state) {
    const modal = $('update-modal');
    $('update-version').textContent = state.releaseTag || (state.latestVersion ? `v${state.latestVersion}` : '');
    $('update-notes').textContent   = String(state.notes || '').trim();
    $('update-progress').textContent = state.status === 'downloading'
        ? `Downloading ${state.progress || 0}%`
        : state.status === 'downloaded'
            ? 'Download complete — restarting to install'
            : state.error || UPDATE_LABELS[state.status] || '';

    const dl = $('update-download');
    dl.disabled = state.status === 'downloading' || state.status === 'downloaded';
    dl.textContent = state.status === 'downloading'
        ? 'Downloading…' : state.status === 'downloaded' ? 'Installing…' : 'Download & Install';

    dl.onclick = async () => {
        const r = await window.zeusmod.installUpdate();
        if (!r.success) $('update-progress').textContent = r.error || 'Update download failed.';
    };
    $('update-open-page').onclick = () => window.zeusmod.openReleasePage();
    $('update-later').onclick     = () => modal.style.display = 'none';

    modal.style.display = 'flex';
}

// ═════════════════════════ BOOTSTRAP ═════════════════════════
init();
