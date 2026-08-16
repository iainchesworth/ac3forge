// ac3forge WASM decode demo - glue between the Embind module (ac3forge_decode.js/.wasm,
// built from decoder_bindings.cpp) and the page (index.html).
//
// What is real here: the decode. A fetched .ec3/.ac3 file is handed straight
// to the WASM module's Decoder.decode() - the actual ac3::forge decoder,
// compiled to WASM, doing real AC-3/E-AC-3 bitstream parsing - and every
// number this page shows (sample rate, channel labels, per-channel energy,
// the audio you hear) comes out of that real decode, not a canned animation.
//
// What is NOT real (yet): individual Atmos object positions. ac3::forge has
// no decode-side OAMD/JOC parser today (only the encoder can write object
// metadata; nothing in the library can read it back) - see the PR this demo
// shipped in for the full explanation, and ROADMAP.md for the follow-up
// tracking real object-position decode. The visualization below is the
// speaker-ring / per-channel energy model from the desktop GUI's
// SoundfieldView.qml, ported to Canvas and driven by genuinely decoded
// per-channel RMS - it shows real bed energy, not object motion.

// Table 5.8 order (matches decoder_bindings.cpp's channelLabels()); azimuths
// are ac3::spatial's kSpeakerAzimuthDeg (src/lib/include/ac3/spatial/spatial.hpp),
// ITU-R BS.775, degrees CCW from front, left positive.
const SPEAKER_AZIMUTH_DEG = { L: 30, C: 0, R: -30, Ls: 110, Rs: -110 };

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

    const result = {
        streamKind: decoder.streamKind(),
        sampleRate: decoder.sampleRate(),
        channelCount,
        labels,
        pcm,
        energy,
        energyBlockSize: decoder.energyBlockSize(),
        durationSeconds: channelCount > 0 ? pcm[0].length / decoder.sampleRate() : 0,
    };
    decoder.delete();
    return result;
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

    const { left, right } = downmixToStereo(decoded);
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

// --- Visualization: ported from src/gui/qml/SoundfieldView.qml -----------
// (screenX/screenY azimuth->pixel mapping, ring radius, opacity-by-RMS,
// energy-vector arrow - see that file for the original QML this was ported
// from, and the PR description for why it's channel energy, not objects.)

function draw() {
    const canvas = el('ring');
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;
    const cx = w / 2, cy = h / 2;
    const radius = Math.min(w, h) / 2 - 28;

    ctx.clearRect(0, 0, w, h);

    // Room bounds: crosshair + ring, same motif as SoundfieldView.qml.
    ctx.strokeStyle = 'rgba(148,163,184,0.35)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(cx, cy - h / 2 + 8); ctx.lineTo(cx, cy + h / 2 - 8);
    ctx.moveTo(cx - w / 2 + 8, cy); ctx.lineTo(cx + w / 2 - 8, cy);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(cx, cy, radius, 0, 2 * Math.PI);
    ctx.stroke();

    // Listener.
    ctx.beginPath();
    ctx.arc(cx, cy, 10, 0, 2 * Math.PI);
    ctx.strokeStyle = 'rgba(148,163,184,0.6)';
    ctx.stroke();

    if (!decoded) {
        requestAnimationFrame(draw);
        return;
    }

    const t = currentPlaybackSeconds();
    const blockDuration = decoded.energyBlockSize / decoded.sampleRate;

    let vecX = 0, vecY = 0;
    let lfeLevel = 0;

    decoded.labels.forEach((label, ch) => {
        const energyTrace = decoded.energy[ch];
        const blockIndex = Math.max(0, Math.min(energyTrace.length - 1, Math.floor(t / blockDuration)));
        const rms = energyTrace.length > 0 ? energyTrace[blockIndex] : 0;
        // 0..~0.4 RMS covers this demo's material comfortably; clamp to keep
        // a silent channel visibly at rest rather than pinned mid-scale.
        const level = Math.max(0, Math.min(1, rms / 0.35));

        if (label === 'LFE') {
            lfeLevel = level;
            return; // LFE has no direction - never drawn spatially (matches SoundfieldView.qml).
        }
        const az = SPEAKER_AZIMUTH_DEG[label];
        if (az === undefined) return; // height/rear channels: no ring position in this simple demo.

        const azRad = (az * Math.PI) / 180;
        const sx = cx - Math.sin(azRad) * radius;
        const sy = cy - Math.cos(azRad) * radius;

        vecX += Math.sin(azRad) * level;
        vecY += Math.cos(azRad) * level;

        const dotRadius = 6 + level * 10;
        ctx.beginPath();
        ctx.arc(sx, sy, dotRadius, 0, 2 * Math.PI);
        ctx.fillStyle = `rgba(96,165,250,${0.35 + 0.65 * level})`;
        ctx.fill();
        ctx.strokeStyle = 'rgba(226,232,240,0.8)';
        ctx.lineWidth = 1.5;
        ctx.stroke();

        ctx.fillStyle = 'rgba(226,232,240,0.9)';
        ctx.font = '12px system-ui, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(label, sx, sy - dotRadius - 8);
    });

    // Energy-vector arrow: the soundfield's overall instantaneous direction.
    const vecLen = Math.hypot(vecX, vecY);
    if (vecLen > 0.02) {
        const scale = radius * Math.min(1, vecLen / 1.5);
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

function tick() {
    if (!playing) return;
    const t = currentPlaybackSeconds();
    el('scrubTime').textContent = `${t.toFixed(1)}s / ${decoded.durationSeconds.toFixed(1)}s`;
    if (t < decoded.durationSeconds) requestAnimationFrame(tick);
}

async function handleDecoded(bytes, label) {
    setStatus(`Decoding ${label}...`, false);
    try {
        const moduleInstance = await loadModule();
        const result = await decodeBytes(bytes, moduleInstance);
        decoded = result;
        playStartOffset = 0;
        stop();
        el('streamInfo').textContent =
            `${result.streamKind}, ${result.sampleRate} Hz, ${result.channelCount} ch ` +
            `(${result.labels.join(', ')}), ${result.durationSeconds.toFixed(1)}s`;
        el('playPause').disabled = false;
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
    requestAnimationFrame(draw);
});
