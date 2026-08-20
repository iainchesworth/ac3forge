// ac3forge WASM decode demo - glue between the Embind module (ac3forge_decode.js/.wasm,
// built from decoder_bindings.cpp) and the page (index.html).
//
// Everything on this page is real. A fetched .ec3/.ac3 file is handed
// straight to the WASM module's Decoder.decode() - the actual ac3::forge
// decoder, compiled to WASM - and every number shown (sample rate, channel
// labels, per-channel energy, the audio you hear) comes out of that real
// decode, not a canned animation. That now includes Atmos objects: real
// decoded OAMD positions (ac3::forge PR #168) drive the room-plan/elevation
// dots below, and real JOC-reconstructed per-object audio (PR #169) is what
// "Solo object N" actually plays - not that object's panned slice of the
// bed, its own isolated waveform. A plain (non-Atmos) stream simply has zero
// objects and only shows the speaker-ring/bed panel.

// Ear-level ring: ac3::spatial's kSpeakerAzimuthDeg
// (src/forge/include/ac3/spatial/spatial.hpp), ITU-R BS.775, degrees CCW from
// front, left positive. Ceiling ring: the same azimuth convention extended to
// Table E2.5's height locations, matching apps/gui/qml/SoundfieldView.qml's own
// extension (its location_azimuth_deg()) - a second, smaller, dashed ring for
// genuinely elevated channels, not a fabricated height axis. A plain 5.1/7.1
// stream (like the bundled demo) never populates it; a real 7.1.4 stream does.
const EAR_LEVEL_AZIMUTH_DEG = { L: 30, C: 0, R: -30, Ls: 110, Rs: -110 };
const CEILING_AZIMUTH_DEG = { Vhl: 45, Vhr: -45, Vhc: 0, Lts: 110, Rts: -110 };

// Distinct per-object colors - the desktop GUI's own room view uses one flat
// color (only the currently-SELECTED object gets an accent), but this page
// has no per-object selection concept, so each object gets its own hue to
// stay visually distinguishable with several moving at once.
const OBJECT_COLORS = ['248,113,113', '52,211,153', '96,165,250', '250,204,21', '192,132,252'];

let soloObject = -1; // -1 = play the bed downmix; >=0 = that object's own isolated audio

let audioCtx = null;
let sourceNode = null;
let playStartCtxTime = 0;
let playStartOffset = 0;
let playing = false;

let decoded = null; // { streamKind, sampleRate, channelCount, labels, pcm[], energy[], energyBlockSize, durationSeconds }

const el = (id) => document.getElementById(id);

function setStatus(text, isError) {
    const status = el('status');
    status.textContent = text;
    status.classList.toggle('error', Boolean(isError));
}

async function loadModule() {
    // createAc3ForgeModule is the global factory MODULARIZE+EXPORT_NAME
    // produces (see wasm_decode_demo/CMakeLists.txt's link options).
    return await createAc3ForgeModule();
}

function copyOut(float32View) {
    // The view Decoder.channelPcm()/channelEnergy() return points straight
    // into the WASM heap and is only valid until the next call into the
    // module (ALLOW_MEMORY_GROWTH can move it under us on the very next
    // allocation) - copy it out immediately rather than holding the view.
    return Float32Array.from(float32View);
}

async function decodeBytes(bytes, moduleInstance) {
    const decoder = new moduleInstance.Decoder();
    const ok = decoder.decode(bytes);
    if (!ok) {
        const message = decoder.error();
        decoder.delete();
        throw new Error(message || 'decode failed');
    }

    const channelCount = decoder.channelCount();
    const labelsVal = decoder.channelLabels();
    const labels = [];
    for (let i = 0; i < channelCount; i++) labels.push(labelsVal[i]);

    const pcm = [];
    const energy = [];
    for (let ch = 0; ch < channelCount; ch++) {
        pcm.push(copyOut(decoder.channelPcm(ch)));
        energy.push(copyOut(decoder.channelEnergy(ch)));
    }

    const objectCount = decoder.objectCount();
    const objectPositions = []; // per object: Float32Array of [x,y,z,gain_db] * frames
    const objectAudio = [];     // per object: Float32Array, real isolated reconstructed audio
    for (let obj = 0; obj < objectCount; obj++) {
        objectPositions.push(copyOut(decoder.objectPositions(obj)));
        objectAudio.push(copyOut(decoder.objectAudioPcm(obj)));
    }

    const result = {
        streamKind: decoder.streamKind(),
        sampleRate: decoder.sampleRate(),
        channelCount,
        labels,
        pcm,
        energy,
        energyBlockSize: decoder.energyBlockSize(),
        durationSeconds: channelCount > 0 ? pcm[0].length / decoder.sampleRate() : 0,
        objectCount,
        objectPositions,
        objectAudio,
        objectFrameSize: decoder.objectFrameSize(),
        objectFrameCount: decoder.objectFrameCount(),
        objectStartSeconds: decoder.objectStartSeconds(),
    };
    decoder.delete();
    return result;
}

// Looks up object `obj`'s [x, y, z, gain_db] at playback time `t`, clamped to
// the decoded range - real decoded OAMD data, one entry per real decoded
// frame, not interpolated/fabricated between frames.
function objectStateAt(result, obj, t) {
    const positions = result.objectPositions[obj];
    const frameDuration = result.objectFrameSize / result.sampleRate;
    const relative = t - result.objectStartSeconds;
    const frame = Math.max(0, Math.min(result.objectFrameCount - 1, Math.floor(relative / frameDuration)));
    const base = frame * 4;
    return { x: positions[base], y: positions[base + 1], z: positions[base + 2], gainDb: positions[base + 3] };
}

// A simple demo downmix to stereo for actual playback - NOT a spec Lo/Ro or
// Lt/Rt matrix, just enough to make a 5.1 bed audible on ordinary stereo
// speakers/headphones, which is what almost every visitor to this page has.
// The visualization panel still reflects the real per-channel decode above.
function downmixToStereo(result) {
    const n = result.channelCount > 0 ? result.pcm[0].length : 0;
    const left = new Float32Array(n);
    const right = new Float32Array(n);
    const gain = { L: [1, 0], R: [0, 1], C: [0.707, 0.707], Ls: [0.6, 0], Rs: [0, 0.6], LFE: [0.5, 0.5] };
    result.labels.forEach((label, ch) => {
        const g = gain[label] || [0.35, 0.35]; // any channel this demo doesn't special-case
        const pcm = result.pcm[ch];
        const [gl, gr] = g;
        if (gl === 0 && gr === 0) return;
        for (let i = 0; i < n; i++) {
            left[i] += pcm[i] * gl;
            right[i] += pcm[i] * gr;
        }
    });
    // Soft clip rather than hard clip - three or four simultaneously loud
    // objects panned wide can exceed 1.0 after the downmix above.
    for (let i = 0; i < n; i++) {
        left[i] = Math.tanh(left[i]);
        right[i] = Math.tanh(right[i]);
    }
    return { left, right };
}

// Builds the buffer actually played: either the bed downmix (default) or,
// when soloObject >= 0, that object's own real isolated JOC-reconstructed
// audio (mono, copied to both channels - deliberately no position-based pan
// here, so what you hear is unambiguously that object's own waveform and
// nothing else, not a re-panned approximation).
function buildPlaybackBuffer(result) {
    if (soloObject >= 0 && soloObject < result.objectCount) {
        const mono = result.objectAudio[soloObject];
        const left = new Float32Array(mono.length);
        const right = new Float32Array(mono.length);
        for (let i = 0; i < mono.length; i++) {
            const s = Math.tanh(mono[i]);
            left[i] = s;
            right[i] = s;
        }
        return { left, right };
    }
    return downmixToStereo(result);
}

function stop() {
    if (sourceNode) {
        try { sourceNode.stop(); } catch (e) { /* already stopped */ }
        sourceNode.disconnect();
        sourceNode = null;
    }
    playing = false;
    el('playPause').textContent = 'Play';
}

function play() {
    if (!decoded) return;
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    stop();

    const { left, right } = buildPlaybackBuffer(decoded);
    const buffer = audioCtx.createBuffer(2, left.length, decoded.sampleRate);
    buffer.copyToChannel(left, 0);
    buffer.copyToChannel(right, 1);

    sourceNode = audioCtx.createBufferSource();
    sourceNode.buffer = buffer;
    sourceNode.connect(audioCtx.destination);
    sourceNode.onended = () => { if (playing) stop(); };

    const offset = playStartOffset % decoded.durationSeconds;
    sourceNode.start(0, offset);
    playStartCtxTime = audioCtx.currentTime - offset;
    playing = true;
    el('playPause').textContent = 'Pause';
    requestAnimationFrame(tick);
}

function pause() {
    if (!playing) return;
    playStartOffset = currentPlaybackSeconds();
    stop();
}

function currentPlaybackSeconds() {
    if (!playing || !audioCtx) return playStartOffset;
    return audioCtx.currentTime - playStartCtxTime;
}

// --- Visualization: ported from apps/gui/qml/SoundfieldView.qml -----------
// (screenX/screenY azimuth->pixel mapping, ring radius, opacity-by-RMS,
// energy-vector arrow - see that file for the original QML this was ported
// from, and the PR description for why it's channel energy, not objects.)

function drawRing(ctx, cx, cy, radius, dashed) {
    ctx.save();
    if (dashed) ctx.setLineDash([4, 4]);
    ctx.strokeStyle = 'rgba(148,163,184,0.35)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(cx, cy, radius, 0, 2 * Math.PI);
    ctx.stroke();
    ctx.restore();
}

function drawSpeaker(ctx, cx, cy, radius, az, label, level, color) {
    const azRad = (az * Math.PI) / 180;
    const sx = cx - Math.sin(azRad) * radius;
    const sy = cy - Math.cos(azRad) * radius;
    const dotRadius = 6 + level * 10;

    ctx.beginPath();
    ctx.arc(sx, sy, dotRadius, 0, 2 * Math.PI);
    ctx.fillStyle = `rgba(${color},${0.35 + 0.65 * level})`;
    ctx.fill();
    ctx.strokeStyle = 'rgba(226,232,240,0.8)';
    ctx.lineWidth = 1.5;
    ctx.stroke();

    ctx.fillStyle = 'rgba(226,232,240,0.9)';
    ctx.font = '12px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(label, sx, sy - dotRadius - 8);

    return { x: Math.sin(azRad) * level, y: Math.cos(azRad) * level };
}

// Ported from apps/gui/qml/SoundfieldView.qml: an ear-level ring plus a
// smaller, dashed ceiling ring for genuinely elevated channels (its own
// visual cue for "a conceptually different, flattened-height plane"), driven
// throughout by real per-channel RMS from the decoder - not a fabricated
// height axis; channels this stream doesn't have simply don't light up.
function draw() {
    const canvas = el('ring');
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;
    const cx = w / 2, cy = h / 2 + 20;
    const earRadius = Math.min(w, h) / 2 - 44;
    const ceilingRadius = earRadius * 0.6;

    ctx.clearRect(0, 0, w, h);
    drawRing(ctx, cx, cy, earRadius, false);
    drawRing(ctx, cx, cy, ceilingRadius, true);

    // Listener.
    ctx.beginPath();
    ctx.arc(cx, cy, 8, 0, 2 * Math.PI);
    ctx.strokeStyle = 'rgba(148,163,184,0.6)';
    ctx.stroke();

    if (!decoded) {
        requestAnimationFrame(draw);
        return;
    }

    const t = currentPlaybackSeconds();
    const blockDuration = decoded.energyBlockSize / decoded.sampleRate;
    let vecX = 0, vecY = 0, lfeLevel = 0;

    decoded.labels.forEach((label, ch) => {
        const energyTrace = decoded.energy[ch];
        const blockIndex = Math.max(0, Math.min(energyTrace.length - 1, Math.floor(t / blockDuration)));
        const rms = energyTrace.length > 0 ? energyTrace[blockIndex] : 0;
        // 0..~0.4 RMS covers this demo's material comfortably; clamp to keep
        // a silent channel visibly at rest rather than pinned mid-scale.
        const level = Math.max(0, Math.min(1, rms / 0.35));

        if (label === 'LFE') { lfeLevel = level; return; } // no direction (SoundfieldView.qml's own rule).

        if (label in EAR_LEVEL_AZIMUTH_DEG) {
            const v = drawSpeaker(ctx, cx, cy, earRadius, EAR_LEVEL_AZIMUTH_DEG[label], label, level, '96,165,250');
            vecX += v.x; vecY += v.y;
        } else if (label in CEILING_AZIMUTH_DEG) {
            drawSpeaker(ctx, cx, cy, ceilingRadius, CEILING_AZIMUTH_DEG[label], label, level, '167,139,250');
        } // any other channel (Cs, Lrs/Rrs, Lw/Rw, ...): no ring position in this simple demo.
    });

    // Energy-vector arrow: the ear-level soundfield's overall instantaneous
    // direction (not a height indicator - see the ceiling ring for that).
    const vecLen = Math.hypot(vecX, vecY);
    if (vecLen > 0.02) {
        const scale = earRadius * Math.min(1, vecLen / 1.5);
        const ex = cx - (vecX / vecLen) * scale;
        const ey = cy - (vecY / vecLen) * scale;
        ctx.beginPath();
        ctx.moveTo(cx, cy);
        ctx.lineTo(ex, ey);
        ctx.strokeStyle = 'rgba(251,191,36,0.9)';
        ctx.lineWidth = 2.5;
        ctx.stroke();
    }

    // LFE: text-only badge, no direction (SoundfieldView.qml's own rule).
    ctx.fillStyle = `rgba(248,113,113,${0.4 + 0.6 * lfeLevel})`;
    ctx.font = 'bold 12px system-ui, sans-serif';
    ctx.textAlign = 'left';
    ctx.fillText('LFE', 12, h - 14);

    requestAnimationFrame(draw);
}

// Real per-object level at playback time t: a short RMS window over that
// object's own real reconstructed audio (JOC, PR #169) - the same "pulse
// with real energy" language the speaker-ring panel already uses, just
// windowed in JS instead of precomputed in C++ since only a handful of
// objects ever need it, each over a small window, once per animation frame.
function objectAudioLevelAt(result, obj, t) {
    const audio = result.objectAudio[obj];
    if (!audio || audio.length === 0) return 0;
    const center = Math.floor(t * result.sampleRate);
    const half = 512;
    const start = Math.max(0, center - half);
    const end = Math.min(audio.length, center + half);
    if (end <= start) return 0;
    let sumSq = 0;
    for (let i = start; i < end; i++) sumSq += audio[i] * audio[i];
    return Math.max(0, Math.min(1, Math.sqrt(sumSq / (end - start)) / 0.35));
}

function drawRoomBox(ctx, x, y, w, h, label) {
    ctx.strokeStyle = 'rgba(148,163,184,0.3)';
    ctx.strokeRect(x + 1, y + 1, w - 2, h - 2);
    ctx.fillStyle = 'rgba(148,163,184,0.6)';
    ctx.font = '10px system-ui, sans-serif';
    ctx.textAlign = 'left';
    ctx.fillText(label, x + 6, 12);
}

function drawObjectDot(ctx, x, y, radius, color, label, highlighted) {
    ctx.beginPath();
    ctx.arc(x, y, radius, 0, 2 * Math.PI);
    ctx.fillStyle = `rgba(${color},${highlighted ? 1 : 0.85})`;
    ctx.fill();
    if (highlighted) {
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.stroke();
    }
    ctx.fillStyle = 'rgba(226,232,240,0.85)';
    ctx.font = '10px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(label, x, y - radius - 6);
}

// Ported from apps/gui/qml/Main.qml's Objects tab (top-down + elevation room
// panels, side by side - see docs/platforms/android.md's own description of
// the same layout in the Shield app). Unlike that GUI's own version, which
// previews positions about to be ENCODED, this one is driven entirely by
// real decoded OAMD positions (ac3::forge PR #168) read back out of the
// bitstream - what a real object actually did, not what it was told to do.
function drawObjects() {
    const canvas = el('objects');
    if (canvas) {
        const ctx = canvas.getContext('2d');
        const w = canvas.width, h = canvas.height;
        ctx.clearRect(0, 0, w, h);

        if (decoded && decoded.objectCount > 0) {
            const gap = 16;
            const padW = (w - gap) / 2;
            const elevX = padW + gap;
            drawRoomBox(ctx, 0, 0, padW, h, 'PLAN (top-down)');
            drawRoomBox(ctx, elevX, 0, padW, h, 'ELEVATION (side-on)');

            // Main.qml's own piecewise z->y mapping: ear level sits at 66% of
            // the pad's height, not the middle, and the floor/ceiling margins
            // differ (14px top, 10px bottom) - ported verbatim.
            const earY = h * 0.66;
            const zToY = (z) => (z >= 0 ? earY - z * (earY - 14) : earY + (-z) * ((h - 10) - earY));
            ctx.strokeStyle = 'rgba(148,163,184,0.25)';
            ctx.beginPath();
            ctx.moveTo(elevX + 4, earY);
            ctx.lineTo(elevX + padW - 4, earY);
            ctx.stroke();

            const t = currentPlaybackSeconds();
            for (let obj = 0; obj < decoded.objectCount; obj++) {
                const s = objectStateAt(decoded, obj, t);
                const level = objectAudioLevelAt(decoded, obj, t);
                const color = OBJECT_COLORS[obj % OBJECT_COLORS.length];
                const radius = 5 + level * 7;
                const label = `obj ${obj + 1}`;
                const highlighted = soloObject === obj;

                // Plan: x -> right wall, y -> rear wall (top-down).
                drawObjectDot(ctx, s.x * padW, s.y * h, radius, color, label, highlighted);
                // Elevation: x-axis stays depth (y); y-axis is height via zToY.
                drawObjectDot(ctx, elevX + s.y * padW, zToY(s.z), radius, color, label, highlighted);
            }
        }
    }
    requestAnimationFrame(drawObjects);
}

let seekDragging = false;

function tick() {
    if (!playing) return;
    const t = currentPlaybackSeconds();
    el('scrubTime').textContent = `${t.toFixed(1)}s / ${decoded.durationSeconds.toFixed(1)}s`;
    if (!seekDragging) el('seek').value = String((t / decoded.durationSeconds) * 1000);
    if (t < decoded.durationSeconds) requestAnimationFrame(tick);
}

// Lets a viewer scrub to any point in the real decoded audio/energy - moving
// the slider while playing restarts playback from there; while paused it just
// repositions the static preview frame draw() already reads from playStartOffset.
function seekTo(fraction) {
    if (!decoded) return;
    playStartOffset = Math.max(0, Math.min(decoded.durationSeconds, fraction * decoded.durationSeconds));
    el('scrubTime').textContent = `${playStartOffset.toFixed(1)}s / ${decoded.durationSeconds.toFixed(1)}s`;
    if (playing) play();
}

function setSoloObject(index) {
    soloObject = index;
    document.querySelectorAll('#soloControls button').forEach((btn) => {
        btn.classList.toggle('active', Number(btn.dataset.object) === index);
    });
    if (playing) { pause(); play(); } // restart on the newly-selected source, from the same position
}

function buildSoloControls(objectCount) {
    const container = el('soloControls');
    container.innerHTML = '';
    if (objectCount === 0) {
        el('objectsPanel').style.display = 'none';
        return;
    }
    el('objectsPanel').style.display = '';

    const bedBtn = document.createElement('button');
    bedBtn.textContent = 'Bed (all channels)';
    bedBtn.dataset.object = '-1';
    bedBtn.className = 'active';
    bedBtn.addEventListener('click', () => setSoloObject(-1));
    container.appendChild(bedBtn);

    for (let obj = 0; obj < objectCount; obj++) {
        const btn = document.createElement('button');
        btn.textContent = `Solo object ${obj + 1}`;
        btn.dataset.object = String(obj);
        btn.style.setProperty('--dot-color', `rgb(${OBJECT_COLORS[obj % OBJECT_COLORS.length]})`);
        btn.classList.add('object-btn');
        btn.addEventListener('click', () => setSoloObject(obj));
        container.appendChild(btn);
    }
}

async function handleDecoded(bytes, label) {
    setStatus(`Decoding ${label}...`, false);
    try {
        const moduleInstance = await loadModule();
        const result = await decodeBytes(bytes, moduleInstance);
        decoded = result;
        playStartOffset = 0;
        soloObject = -1;
        stop();
        const objectNote = result.objectCount > 0 ? `, ${result.objectCount} Atmos object(s)` : '';
        el('streamInfo').textContent =
            `${result.streamKind}, ${result.sampleRate} Hz, ${result.channelCount} ch ` +
            `(${result.labels.join(', ')})${objectNote}, ${result.durationSeconds.toFixed(1)}s`;
        el('playPause').disabled = false;
        el('seek').disabled = false;
        el('seek').value = '0';
        el('scrubTime').textContent = `0.0s / ${result.durationSeconds.toFixed(1)}s`;
        buildSoloControls(result.objectCount);
        setStatus(`Decoded ${label}.`, false);
    } catch (err) {
        decoded = null;
        el('playPause').disabled = true;
        setStatus(`Decode failed: ${err.message}`, true);
    }
}

async function loadBundledDemo() {
    const response = await fetch('assets/demo.ec3');
    const buffer = await response.arrayBuffer();
    await handleDecoded(new Uint8Array(buffer), 'the bundled demo stream (assets/demo.ec3)');
}

function loadFile(file) {
    const reader = new FileReader();
    reader.onload = () => handleDecoded(new Uint8Array(reader.result), file.name);
    reader.readAsArrayBuffer(file);
}

window.addEventListener('DOMContentLoaded', () => {
    el('loadDemo').addEventListener('click', loadBundledDemo);
    el('fileInput').addEventListener('change', (e) => {
        if (e.target.files.length > 0) loadFile(e.target.files[0]);
    });
    el('playPause').addEventListener('click', () => {
        if (playing) pause(); else play();
    });
    const seek = el('seek');
    seek.addEventListener('pointerdown', () => { seekDragging = true; });
    seek.addEventListener('input', () => {
        // Live-update the static frame while dragging, without restarting
        // audio on every intermediate tick - only on release (see 'change').
        if (!decoded) return;
        playStartOffset = (Number(seek.value) / 1000) * decoded.durationSeconds;
        el('scrubTime').textContent = `${playStartOffset.toFixed(1)}s / ${decoded.durationSeconds.toFixed(1)}s`;
    });
    seek.addEventListener('change', () => {
        seekDragging = false;
        seekTo(Number(seek.value) / 1000);
    });
    requestAnimationFrame(draw);
    requestAnimationFrame(drawObjects);
});
