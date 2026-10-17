/* SPDX-License-Identifier: MIT */
'use strict';
const $ = id => document.getElementById(id);
let token = '', state = null, busy = false, lastSequence = 0, current = null;
let view = {start: -0.5, stop: 0.5}, peak = null, cursor = null, drag = null;
let noticeTimer, frameBusy = false, connected = true, frameCount = 0, fpsTime = performance.now();
const spec = $('spectrum'), waterfall = $('waterfall');
const sc = spec.getContext('2d'), wc = waterfall.getContext('2d');
const pad = {left: 57, right: 22, top: 34, bottom: 32};

function notify(text, error = false) {
  clearTimeout(noticeTimer);
  $('notice').textContent = text;
  $('notice').className = 'notice' + (error ? ' error' : '');
  $('notice').hidden = false;
  noticeTimer = setTimeout(() => $('notice').hidden = true, error ? 12000 : 4000);
}
async function get(path) {
  const result = await fetch(path, {cache: 'no-store'});
  if (!result.ok) throw new Error((await result.json()).error || `HTTP ${result.status}`);
  return result.json();
}
async function post(path, body = {}) {
  const result = await fetch(path, {method: 'POST', headers: {'Content-Type': 'application/json', 'X-Birria-Token': token}, body: JSON.stringify(body)});
  if (!result.ok) throw new Error((await result.json()).error || `HTTP ${result.status}`);
  return result;
}
function setBusy(on) {
  busy = on;
  document.querySelectorAll('button').forEach(button => button.disabled = on);
  $('run').textContent = on ? 'Please wait…' : state?.active ? '■ Stop receiver' : '▶ Start receiver';
}
async function command(path, body = {}) {
  if (busy) return;
  setBusy(true);
  try {
    const response = await post(path, body);
    state = await response.json();
    syncInputs(state.settings);
    updateState();
  } catch (e) {notify(e.message, true);}
  finally {setBusy(false);}
}
function clearHistory() {
  peak = null;
  wc.fillStyle = '#030c16'; wc.fillRect(0, 0, waterfall.width, waterfall.height);
}
function syncInputs(settings) {
  if (!settings) return;
  $('frequency').value = (settings.center_hz / 1e6).toFixed(6);
  for (const id of ['rate', 'bandwidth', 'lna', 'lpf', 'digital', 'fft_size', 'averaging']) $(id).value = settings[id];
  $('manual').checked = settings.manual; $('dc').checked = settings.dc;
  updateGain();
}
function updateGain() {
  const manual = $('manual').checked;
  for (const id of ['lna', 'lpf', 'digital']) {$(id).disabled = !manual; $(id + 'Value').textContent = $(id).value;}
  $('gainFields').style.opacity = manual ? '1' : '.45';
  $('gainHint').textContent = manual ? 'Raw stage codes, not dB. Higher gain can overload the receiver.' : 'Hardware overrides released. Full AGC control is not qualified. Manual values are raw codes, not dB.';
}
function updateState() {
  if (!state) return;
  $('modeBadge').textContent = state.mode === 'demo' ? 'DEMO / SYNTHETIC' : 'MT7961 / USB';
  $('liveDot').classList.toggle('live', state.status === 'running');
  $('usbDot').classList.toggle('live', state.status === 'running');
  $('runLabel').textContent = ({starting: 'Starting receiver…', running: state.mode === 'demo' ? 'Demo spectrum' : 'Receiving', error: 'Receiver error', stopped: 'Receiver stopped'})[state.status];
  $('run').classList.toggle('running', state.active);
  if (!busy) $('run').textContent = state.active ? '■ Stop receiver' : '▶ Start receiver';
  $('plotInfo').textContent = `${state.settings.rate / 1e6} MSps · ${(state.settings.rate / state.settings.fft_size).toFixed(1)} Hz / bin`;
  $('transportLabel').textContent = state.mode === 'demo' ? 'Synthetic I/Q' : state.active ? 'USB streaming' : 'USB released';
  $('throughput').textContent = state.average_msps.toFixed(1);
  $('totalSamples').textContent = new Intl.NumberFormat('en', {notation: 'compact', maximumFractionDigits: 2}).format(state.samples);
  $('health').textContent = state.error ? 'Capture failed' : state.final_stats ? 'Clean stop' : state.active ? 'Continuity checked*' : 'No capture yet';
  if (state.error_kind === 'host_overrun') $('health').textContent = 'Host queue overrun';
  if (state.error_kind === 'device_overrun') $('health').textContent = 'Device ring overrun';
  $('health').style.color = state.error ? '#ff826e' : '#a9d8ca';
  $('health').title = state.error || 'Native packet checks run throughout capture. Final success is established when the read ends. Display windows are sampled, not every sample is transformed.';
  if (state.mode === 'demo') $('health').textContent = 'Demo data';
  document.querySelectorAll('[data-freq]').forEach(b => b.classList.toggle('selected', Math.abs(Number(b.dataset.freq) * 1e6 - state.settings.center_hz) < 1e6));
  if (state.error && state.error !== updateState.lastError) {notify(state.error, true); updateState.lastError = state.error;}
  if (!state.error) updateState.lastError = null;
}
async function pollState() {
  try {
    const value = await get('/api/state');
    token = value.token;
    const first = !state;
    state = value;
    if (first) syncInputs(state.settings);
    connected = true; $('connection').hidden = true;
    updateState();
  } catch (e) {connected = false; $('connection').hidden = false;}
}
async function refreshDevices() {
  try {
    const devices = await get('/api/devices');
    $('device').replaceChildren();
    if (!devices.length) $('device').append(new Option('No receiver detected', '0'));
    devices.forEach(device => $('device').append(new Option(`${device.index}: ${device.name}`, device.index)));
    if (state) $('device').value = state.settings.index;
  } catch (e) {notify(e.message, true);}
}
function resize() {
  const dpr = Math.min(devicePixelRatio || 1, 2);
  for (const canvas of [spec, waterfall]) {
    const box = canvas.getBoundingClientRect();
    canvas.width = Math.round(box.width * dpr); canvas.height = Math.round(box.height * dpr);
  }
  clearHistory(); drawSpectrum();
}
function limits() {
  const floor = Number($('floor').value), ceiling = Number($('ceiling').value);
  return [floor, Math.max(floor + 10, ceiling)];
}
function traceForPixels(bins, width) {
  const result = new Float32Array(width);
  const n = bins.length, first = (view.start + .5) * n, span = (view.stop - view.start) * n;
  for (let x = 0; x < width; x++) {
    const start = Math.min(n - 1, Math.max(0, Math.floor(first + x / width * span)));
    const stop = Math.min(n, Math.max(start + 1, Math.floor(first + (x + 1) / width * span)));
    let value = -160;
    for (let i = start; i < stop; i++) value = Math.max(value, bins[i]);
    result[x] = value;
  }
  return result;
}
function geometry(canvas) {
  const box = canvas.getBoundingClientRect();
  if (box.width <= 0 || box.height <= 0 || canvas.width <= 0 || canvas.height <= 0) return null;
  const scale = canvas.width / box.width;
  if (!Number.isFinite(scale) || canvas.width <= (pad.left + pad.right) * scale ||
      canvas.height <= (pad.top + pad.bottom) * scale) return null;
  return {l: pad.left * scale, r: pad.right * scale, t: pad.top * scale, b: pad.bottom * scale, scale,
          w: canvas.width - (pad.left + pad.right) * scale, h: canvas.height - (pad.top + pad.bottom) * scale};
}
function drawSpectrum() {
  const w = spec.width, h = spec.height, g = geometry(spec), [floor, ceiling] = limits();
  if (!g) return;
  const scaleLabels = document.querySelectorAll('.waterfall-scale span');
  scaleLabels[0].textContent = ceiling; scaleLabels[1].textContent = `${floor} dBFS`;
  sc.fillStyle = '#0c131b'; sc.fillRect(0, 0, w, h);
  sc.font = `${10 * g.scale}px Consolas,monospace`; sc.lineWidth = g.scale;
  const rate = current?.rate || state?.settings.rate || 40e6;
  const center = current?.center || state?.settings.center_hz || 2437e6;
  for (let i = 0; i <= 4; i++) {
    const y = g.t + i / 4 * g.h;
    sc.strokeStyle = '#20303d'; sc.beginPath(); sc.moveTo(g.l, y); sc.lineTo(w - g.r, y); sc.stroke();
    sc.fillStyle = '#6f899b'; sc.textAlign = 'right'; sc.fillText(Math.round(ceiling - i / 4 * (ceiling - floor)), g.l - 10 * g.scale, y + 3 * g.scale);
  }
  sc.fillStyle = '#6f899b'; sc.textAlign = 'left'; sc.fillText('dBFS', 10 * g.scale, 17 * g.scale);
  for (let i = 0; i <= 8; i++) {
    const x = g.l + i / 8 * g.w;
    sc.strokeStyle = '#1a2935'; sc.beginPath(); sc.moveTo(x, g.t); sc.lineTo(x, h - g.b); sc.stroke();
    const f = center + (view.start + i / 8 * (view.stop - view.start)) * rate;
    sc.textAlign = 'center'; sc.fillStyle = '#7892a6'; sc.fillText((f / 1e6).toFixed(view.stop - view.start < .05 ? 3 : 1), x, h - 10 * g.scale);
  }
  if (!current) return;
  const width = Math.max(1, Math.floor(g.w)), line = traceForPixels(current.bins, width);
  const path = values => {
    sc.beginPath();
    for (let i = 0; i < width; i++) {
      const y = g.t + (1 - Math.max(0, Math.min(1, (values[i] - floor) / (ceiling - floor)))) * g.h;
      if (i) sc.lineTo(g.l + i, y); else sc.moveTo(g.l, y);
    }
  };
  path(line); sc.lineTo(g.l + width - 1, g.t + g.h); sc.lineTo(g.l, g.t + g.h); sc.closePath();
  const gradient = sc.createLinearGradient(0, g.t, 0, g.t + g.h);
  gradient.addColorStop(0, '#43d9c42b'); gradient.addColorStop(1, '#2e8dca05'); sc.fillStyle = gradient; sc.fill();
  if ($('peak').checked && peak) {path(traceForPixels(peak, width)); sc.strokeStyle = '#d2a56599'; sc.lineWidth = .8 * g.scale; sc.stroke();}
  path(line); sc.strokeStyle = '#65dfd0'; sc.lineWidth = g.scale; sc.stroke();
  if (cursor !== null) {
    const x = Math.max(g.l, Math.min(g.l + g.w, cursor * spec.width));
    sc.strokeStyle = '#9cb4c277'; sc.setLineDash([3 * g.scale, 4 * g.scale]); sc.beginPath(); sc.moveTo(x, g.t); sc.lineTo(x, h - g.b); sc.stroke(); sc.setLineDash([]);
  }
  $('spanLabel').textContent = `${((view.stop - view.start) * rate / 1e6).toFixed(3)} MHz span`;
}
const stops = [[0, [2, 9, 22]], [.22, [14, 37, 82]], [.44, [21, 74, 145]], [.64, [37, 170, 178]], [.82, [241, 139, 61]], [1, [255, 246, 178]]];
const palette = new Uint8ClampedArray(256 * 4);
for (let i = 0; i < 256; i++) {
  const t = i / 255;
  let at = stops.findIndex(s => s[0] >= t); if (at < 1) at = 1;
  const a = stops[at - 1], b = stops[at], f = (t - a[0]) / (b[0] - a[0]);
  for (let c = 0; c < 3; c++) palette[i * 4 + c] = a[1][c] * (1 - f) + b[1][c] * f;
  palette[i * 4 + 3] = 255;
}
function drawWaterfall() {
  const g = geometry(waterfall);
  if (!g) return;
  const width = Math.max(1, Math.floor(g.w)), [floor, ceiling] = limits();
  const pixels = traceForPixels(current.bins, width), row = wc.createImageData(width, 1), step = Math.max(1, Math.round(2 * g.scale));
  wc.drawImage(waterfall, 0, 0, waterfall.width, waterfall.height - step, 0, step, waterfall.width, waterfall.height - step);
  for (let x = 0; x < width; x++) {
    const code = Math.round(Math.max(0, Math.min(255, (pixels[x] - floor) / (ceiling - floor) * 255)));
    row.data.set(palette.subarray(code * 4, code * 4 + 4), x * 4);
  }
  for (let i = 0; i < step; i++) wc.putImageData(row, Math.round(g.l), i);
}
async function pollFrame() {
  if (frameBusy || !connected) return;
  frameBusy = true;
  try {
    const result = await fetch(`/api/frame?after=${lastSequence}`, {cache: 'no-store'});
    if (result.status !== 200) return;
    const buffer = await result.arrayBuffer(), data = new DataView(buffer);
    if (buffer.byteLength < 40 || data.getUint32(0, true) !== 0x54464642) return;
    const sequence = data.getUint32(4, true), count = data.getUint32(8, true);
    if (sequence === lastSequence || buffer.byteLength !== 40 + count * 4) return;
    const center = Number(data.getBigUint64(16, true)), rate = data.getUint32(12, true);
    if (current && (center !== current.center || rate !== current.rate || count !== current.bins.length)) clearHistory();
    current = {center, rate, bins: new Float32Array(buffer, 40, count)};
    lastSequence = sequence;
    if (!peak || peak.length !== count) peak = current.bins.slice();
    else for (let i = 0; i < count; i++) peak[i] = Math.max(peak[i], current.bins[i]);
    $('empty').hidden = true;
    drawSpectrum(); drawWaterfall();
    frameCount++;
    const now = performance.now();
    if (now - fpsTime > 1000) {$('fps').textContent = (frameCount * 1000 / (now - fpsTime)).toFixed(1); frameCount = 0; fpsTime = now;}
  } catch (e) { /* State polling reports a disconnected server. */ }
  finally {frameBusy = false;}
}
function zoom(factor, anchor = .5) {
  const span = view.stop - view.start, size = Math.min(1, Math.max(.001, span * factor));
  let start = view.start + anchor * span - anchor * size;
  start = Math.max(-.5, Math.min(.5 - size, start));
  view = {start, stop: start + size}; clearHistory(); drawSpectrum();
}
function position(event, canvas) {
  const box = canvas.getBoundingClientRect(), g = geometry(canvas);
  if (!g) return .5;
  return Math.max(0, Math.min(1, ((event.clientX - box.left) * canvas.width / box.width - g.l) / g.w));
}
for (const canvas of [spec, waterfall]) {
  canvas.addEventListener('wheel', event => {event.preventDefault(); zoom(event.deltaY < 0 ? .8 : 1.25, position(event, canvas));}, {passive: false});
  canvas.addEventListener('dblclick', event => {if (!current) return; tune(Math.round(current.center + (view.start + position(event, canvas) * (view.stop - view.start)) * current.rate));});
  canvas.addEventListener('pointerdown', event => {drag = {x: event.clientX, view: {...view}}; canvas.setPointerCapture(event.pointerId);});
  canvas.addEventListener('pointerup', () => drag = null);
  canvas.addEventListener('pointercancel', () => drag = null);
  canvas.addEventListener('pointermove', event => {
    if (drag) {
      const size = drag.view.stop - drag.view.start, dx = (event.clientX - drag.x) / canvas.getBoundingClientRect().width * size;
      const start = Math.max(-.5, Math.min(.5 - size, drag.view.start - dx));
      view = {start, stop: start + size}; clearHistory();
    }
    if (canvas === spec && current) {
      const fraction = position(event, canvas), offset = view.start + fraction * (view.stop - view.start);
      const bin = Math.max(0, Math.min(current.bins.length - 1, Math.round((offset + .5) * current.bins.length)));
      cursor = (event.clientX - spec.getBoundingClientRect().left) / spec.getBoundingClientRect().width;
      $('cursorReadout').textContent = `${((current.center + offset * current.rate) / 1e6).toFixed(6)} MHz   ${current.bins[bin].toFixed(1)} dBFS`;
    }
    drawSpectrum();
  });
  canvas.addEventListener('pointerleave', () => {cursor = null; drawSpectrum();});
}
async function tune(hz) {
  if (!Number.isSafeInteger(hz)) {notify('Enter a frequency in MHz', true); return;}
  await command('/api/configure', {center_hz: hz});
}
$('frequency').addEventListener('keydown', event => {if (event.key === 'Enter') {tune(Math.round(Number($('frequency').value) * 1e6)); $('frequency').blur();}});
$('stepDown').onclick = () => tune(Math.round((Number($('frequency').value) - Number($('step').value)) * 1e6));
$('stepUp').onclick = () => tune(Math.round((Number($('frequency').value) + Number($('step').value)) * 1e6));
document.querySelectorAll('[data-freq]').forEach(button => button.onclick = () => tune(Number(button.dataset.freq) * 1e6));
$('run').onclick = () => command(state?.active ? '/api/stop' : '/api/start');
$('refresh').onclick = refreshDevices;
$('rate').onchange = () => {if ($('rate').value === '40000000') $('bandwidth').value = '40000000';};
$('applyRadio').onclick = () => command('/api/configure', {index: Number($('device').value), center_hz: Math.round(Number($('frequency').value) * 1e6), rate: Number($('rate').value), bandwidth: Number($('bandwidth').value)});
$('manual').onchange = updateGain;
for (const id of ['lna', 'lpf', 'digital']) $(id).oninput = updateGain;
$('applyGain').onclick = () => command('/api/configure', {manual: $('manual').checked, lna: Number($('lna').value), lpf: Number($('lpf').value), digital: Number($('digital').value)});
for (const id of ['fft_size', 'averaging', 'dc']) $(id).onchange = () => command('/api/configure', {fft_size: Number($('fft_size').value), averaging: Number($('averaging').value), dc: $('dc').checked});
for (const id of ['floor', 'ceiling']) $(id).oninput = () => {$(id + 'Value').textContent = `${$(id).value} dBFS`; drawSpectrum();};
$('peak').onchange = () => {peak = current?.bins.slice() || null; drawSpectrum();};
$('zoomIn').onclick = () => zoom(.5); $('zoomOut').onclick = () => zoom(2);
$('resetView').onclick = () => {view = {start: -.5, stop: .5}; clearHistory(); drawSpectrum();};
$('snapshot').onclick = async () => {
  try {const response = await post('/api/snapshot'), blob = await response.blob(), url = URL.createObjectURL(blob), link = document.createElement('a'); link.href = url; link.download = `birria-${state.settings.center_hz}-${Date.now()}.zip`; link.click(); setTimeout(() => URL.revokeObjectURL(url), 30000); notify('Saved raw I/Q and capture metadata');}
  catch (e) {notify(e.message, true);}
};
$('diagnostics').onclick = async () => {
  try {
    const {token: ignored, ...report} = await get('/api/state');
    const blob = new Blob([JSON.stringify(report, null, 2)], {type: 'application/json'});
    const url = URL.createObjectURL(blob), link = document.createElement('a');
    link.href = url; link.download = `birria-diagnostics-${Date.now()}.json`; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    notify('Saved receiver diagnostics');
  } catch (e) {notify(e.message, true);}
};
$('shutdown').onclick = async () => {
  try {await post('/api/shutdown'); connected = false; $('connection').hidden = true; $('empty').hidden = false; $('empty').querySelector('strong').textContent = 'Receiver closed'; $('empty').querySelector('p').textContent = 'You can close this tab.'; document.querySelectorAll('button').forEach(button => button.disabled = true); clearInterval(stateTimer); clearInterval(frameTimer);}
  catch (e) {notify(e.message, true);}
};
new ResizeObserver(resize).observe($('spectrumWrap'));
new ResizeObserver(resize).observe(waterfall.parentElement);
pollState().then(refreshDevices);
const stateTimer = setInterval(pollState, 1000), frameTimer = setInterval(pollFrame, 50);
