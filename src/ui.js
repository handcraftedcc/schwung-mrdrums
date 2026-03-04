/* mrdrums semi-custom UI with dynamic per-pad bindings */

const PAD_NOTE_MIN = 36;
const PAD_NOTE_MAX = 51;
const PAD_NOTE_FALLBACK_MIN = 68;
const PAD_NOTE_FALLBACK_MAX = 83;

const PAGES = {
    GLOBAL: 0,
    PAD_SETTINGS: 1,
};

const PAD_PAGE_MAIN = 0;
const PAD_PAGE_RANDOM = 1;

const GLOBAL_CONTROLS = [
    { key: 'g_master_vol', label: 'Master Vol', type: 'float', min: 0.0, max: 1.0, step: 0.01, fineStep: 0.005 },
    { key: 'g_polyphony', label: 'Polyphony', type: 'int', min: 1, max: 64, step: 1, fineStep: 1 },
    { key: 'g_vel_curve', label: 'Vel Curve', type: 'enum', options: ['linear', 'soft', 'hard'] },
    { key: 'g_humanize_ms', label: 'Humanize', type: 'float', min: 0.0, max: 50.0, step: 0.1, fineStep: 0.05 },
    { key: 'g_rand_seed', label: 'Rand Seed', type: 'int', min: 1, max: 2147483647, step: 1, fineStep: 1 },
    { key: 'g_rand_loop_steps', label: 'Loop Steps', type: 'int', min: 1, max: 128, step: 1, fineStep: 1 },
];

const PAD_MAIN_CONTROLS = [
    { suffix: 'sample_path', label: 'Sample', type: 'filepath' },
    { suffix: 'vol', label: 'Vol', type: 'float', min: 0.0, max: 1.0, step: 0.01, fineStep: 0.005 },
    { suffix: 'pan', label: 'Pan', type: 'float', min: -1.0, max: 1.0, step: 0.1, fineStep: 0.05 },
    { suffix: 'tune', label: 'Tune', type: 'float', min: -24.0, max: 24.0, step: 1.0, fineStep: 0.5 },
    { suffix: 'start', label: 'Start', type: 'float', min: 0.0, max: 1.0, step: 0.01, fineStep: 0.005 },
    { suffix: 'attack_ms', label: 'Attack', type: 'float', min: 0.0, max: 5000.0, step: 1.0, fineStep: 0.5 },
    { suffix: 'decay_ms', label: 'Decay', type: 'float', min: 0.0, max: 5000.0, step: 5.0, fineStep: 1.0 },
    { suffix: 'choke_group', label: 'Choke', type: 'int', min: 0, max: 16, step: 1, fineStep: 1 },
    { suffix: 'mode', label: 'Mode', type: 'enum', options: ['gate', 'oneshot'] },
];

const PAD_RANDOM_CONTROLS = [
    { suffix: 'rand_pan_amt', label: 'Rand Pan', type: 'float', min: 0.0, max: 1.0, step: 0.01, fineStep: 0.005 },
    { suffix: 'rand_vol_amt', label: 'Rand Vol', type: 'float', min: 0.0, max: 1.0, step: 0.01, fineStep: 0.005 },
    { suffix: 'rand_decay_amt', label: 'Rand Decay', type: 'float', min: 0.0, max: 1.0, step: 0.01, fineStep: 0.005 },
    { suffix: 'chance_pct', label: 'Chance', type: 'float', min: 0.0, max: 100.0, step: 1.0, fineStep: 0.5 },
];

const state = {
    page: PAGES.GLOBAL,
    currentPad: 1,
    selectedGlobalIndex: 0,
    selectedPadParamIndex: 0,
    padControlPage: PAD_PAGE_MAIN,
    shiftHeld: false,
    needsRedraw: true,
};

function clamp(v, min, max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

function decodeDelta(value) {
    if (value === 64) return 0;
    if (value > 64) return value - 128;
    return value;
}

function encoderIndexForCc(cc) {
    if (cc >= 14 && cc <= 21) return cc - 14;
    if (cc >= 71 && cc <= 78) return cc - 71;
    return -1;
}

function getParamRaw(key) {
    if (typeof host_module_get_param !== 'function') return '';
    const val = host_module_get_param(key);
    if (val === undefined || val === null) return '';
    return String(val);
}

function setParamRaw(key, value) {
    if (typeof host_module_set_param !== 'function') return;
    host_module_set_param(key, String(value));
}

function noteToPad(note) {
    if (note >= PAD_NOTE_MIN && note <= PAD_NOTE_MAX) {
        return note - PAD_NOTE_MIN + 1;
    }
    if (note >= PAD_NOTE_FALLBACK_MIN && note <= PAD_NOTE_FALLBACK_MAX) {
        return note - PAD_NOTE_FALLBACK_MIN + 1;
    }
    return 0;
}

function activePadKey(suffix) {
    return `p${String(state.currentPad).padStart(2, '0')}_${suffix}`;
}

function getPadControls() {
    return state.padControlPage === PAD_PAGE_MAIN ? PAD_MAIN_CONTROLS : PAD_RANDOM_CONTROLS;
}

function openSampleFileBrowser() {
    /* Use dynamic alias so browser metadata (root/start/filter/start_path) comes from chain_params. */
    setParamRaw('ui_current_pad', state.currentPad);
    if (typeof host_open_file_browser === 'function') {
        host_open_file_browser('pad_sample_path');
    }
}

function formatValue(control, raw) {
    if (control.type === 'filepath') {
        if (!raw) return '--';
        const slash = raw.lastIndexOf('/');
        return slash >= 0 ? raw.slice(slash + 1) : raw;
    }
    if (control.type === 'enum') return raw || control.options[0];
    if (control.type === 'int') {
        const n = parseInt(raw || '0', 10);
        return Number.isFinite(n) ? String(n) : '0';
    }
    const f = parseFloat(raw || '0');
    if (!Number.isFinite(f)) return '0';
    return f.toFixed(3);
}

function adjustControl(control, key, delta) {
    if (delta === 0) return;

    if (control.type === 'filepath') {
        if (delta > 0) openSampleFileBrowser();
        if (delta < 0) {
            setParamRaw('ui_current_pad', state.currentPad);
            setParamRaw('pad_sample_path', '');
        }
        return;
    }

    const fine = state.shiftHeld;

    if (control.type === 'enum') {
        const current = getParamRaw(key) || control.options[0];
        let idx = control.options.indexOf(current);
        if (idx < 0) idx = 0;
        idx += delta > 0 ? 1 : -1;
        while (idx < 0) idx += control.options.length;
        while (idx >= control.options.length) idx -= control.options.length;
        setParamRaw(key, control.options[idx]);
        return;
    }

    if (control.type === 'int') {
        const step = fine ? (control.fineStep || 1) : (control.step || 1);
        const current = parseInt(getParamRaw(key) || '0', 10);
        const next = clamp((Number.isFinite(current) ? current : 0) + step * (delta > 0 ? 1 : -1), control.min, control.max);
        setParamRaw(key, Math.round(next));
        return;
    }

    const step = fine ? (control.fineStep || control.step || 0.01) : (control.step || 0.01);
    const current = parseFloat(getParamRaw(key) || '0');
    const base = Number.isFinite(current) ? current : 0;
    const next = clamp(base + step * (delta > 0 ? 1 : -1), control.min, control.max);
    setParamRaw(key, next);
}

function applyPadNoteSelection(note) {
    if (state.page !== PAGES.PAD_SETTINGS) return false;
    const pad = noteToPad(note);
    if (pad <= 0) return false;

    state.currentPad = pad;
    setParamRaw('ui_current_pad', pad);
    state.needsRedraw = true;
    return true;
}

function switchPage() {
    if (state.page === PAGES.GLOBAL) {
        state.page = PAGES.PAD_SETTINGS;
    } else {
        state.page = PAGES.GLOBAL;
    }
    state.needsRedraw = true;
}

function switchPadSubPage() {
    state.padControlPage = state.padControlPage === PAD_PAGE_MAIN ? PAD_PAGE_RANDOM : PAD_PAGE_MAIN;
    state.selectedPadParamIndex = 0;
    state.needsRedraw = true;
}

function handleEncoder(cc, value) {
    const idx = encoderIndexForCc(cc);
    if (idx < 0 || idx > 7) return;

    const delta = decodeDelta(value);
    if (delta === 0) return;

    if (state.page === PAGES.GLOBAL) {
        if (idx >= GLOBAL_CONTROLS.length) return;
        const control = GLOBAL_CONTROLS[idx];
        state.selectedGlobalIndex = idx;
        adjustControl(control, control.key, delta);
        state.needsRedraw = true;
        return;
    }

    const controls = getPadControls();
    if (idx >= controls.length) return;

    const control = controls[idx];
    state.selectedPadParamIndex = idx;
    const key = activePadKey(control.suffix);
    adjustControl(control, key, delta);
    state.needsRedraw = true;
}

function drawLine(y, text, selected) {
    const prefix = selected ? '> ' : '  ';
    print(2, y, `${prefix}${text}`, 1);
}

function drawGlobalPage() {
    print(2, 2, 'mrdrums - Global', 2);
    for (let i = 0; i < GLOBAL_CONTROLS.length; i++) {
        const c = GLOBAL_CONTROLS[i];
        drawLine(16 + i * 10, `${c.label}: ${formatValue(c, getParamRaw(c.key))}`, i === state.selectedGlobalIndex);
    }
    print(2, 120, 'Btn23:PadSettings  Shift:Fine', 1);
}

function drawPadPage() {
    print(2, 2, 'mrdrums - Pad Settings', 2);
    print(2, 14, `Current pad: ${state.currentPad}`, 1);

    const sampleKey = activePadKey('sample_path');
    const sampleName = formatValue({ type: 'filepath' }, getParamRaw(sampleKey));
    print(2, 24, `Sample: ${sampleName}`, 1);

    const controls = getPadControls();
    for (let i = 0; i < controls.length && i < 9; i++) {
        const c = controls[i];
        const key = activePadKey(c.suffix);
        drawLine(36 + i * 9, `${c.label}: ${formatValue(c, getParamRaw(key))}`, i === state.selectedPadParamIndex);
    }

    const pageLabel = state.padControlPage === PAD_PAGE_MAIN ? 'Main' : 'Random';
    print(2, 120, `Page:${pageLabel}  Btn23:Global  Btn24:Toggle`, 1);
}

function redraw() {
    clear_screen();
    if (state.page === PAGES.GLOBAL) {
        drawGlobalPage();
    } else {
        drawPadPage();
    }
}

function onMidiMessageCommon(data) {
    if (!data || data.length < 3) return;

    const status = data[0] & 0xf0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0x90 && d2 > 0) {
        if (applyPadNoteSelection(d1)) return;

        if (d1 === 23) {
            switchPage();
            return;
        }
        if (d1 === 24 && state.page === PAGES.PAD_SETTINGS) {
            switchPadSubPage();
            return;
        }
        if (d1 === 49) {
            state.shiftHeld = true;
            return;
        }
    }

    if ((status === 0x80) || (status === 0x90 && d2 === 0)) {
        if (d1 === 49) {
            state.shiftHeld = false;
            return;
        }
    }

    if (status === 0xb0) {
        handleEncoder(d1, d2);
    }
}

globalThis.init = function init() {
    const savedPad = parseInt(getParamRaw('ui_current_pad') || '1', 10);
    state.currentPad = clamp(Number.isFinite(savedPad) ? savedPad : 1, 1, 16);
    setParamRaw('ui_current_pad', state.currentPad);
    state.needsRedraw = true;
};

globalThis.tick = function tick() {
    const paramPad = parseInt(getParamRaw('ui_current_pad') || String(state.currentPad), 10);
    if (Number.isFinite(paramPad)) {
        const clamped = clamp(paramPad, 1, 16);
        if (clamped !== state.currentPad) {
            state.currentPad = clamped;
            state.needsRedraw = true;
        }
    }

    if (!state.needsRedraw) return;
    redraw();
    state.needsRedraw = false;
};

globalThis.onMidiMessageInternal = function onMidiMessageInternal(data) {
    onMidiMessageCommon(data);
};

globalThis.onMidiMessageExternal = function onMidiMessageExternal(data) {
    onMidiMessageCommon(data);
};

/* Test-only hooks to validate dynamic key binding logic with static checks. */
globalThis.__mrdrums_test_hooks = {
    activePadKey,
    applyPadNoteSelection,
    getState: () => ({
        page: state.page,
        currentPad: state.currentPad,
        selectedPadParamIndex: state.selectedPadParamIndex,
        padControlPage: state.padControlPage,
    }),
};
