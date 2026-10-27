'use strict';
const $ = id => document.getElementById(id);
const frequencies = ['31 Hz','63 Hz','125 Hz','250 Hz','500 Hz','1 kHz','2 kHz','4 kHz','8 kHz','16 kHz'];
let settings = {gains: Array(10).fill(0), preamp: 0, maintain: false, loudness_target: 0};
let dirty = false, sending = false, timer, localError = '', acceptedRevision = 0, outputBusy = false, instance;
let editSequence = 0, editOutput = '', retryDelay = 500, eqError = '', masterEpoch = 0;
let latest = null, editing = false, textInvalid = false, presetSignature = '', playerSignature = '', quickbarSignature = '';
let fallbackSignature = '';
const pairs = [], mixerRows = new Map(), mixerTimers = new Map(), mixerHold = new Map();
const average = () => settings.gains.reduce((sum, gain) => sum + gain, 0) / 10;
const signed = value => `${value >= 0 ? '+' : ''}${value.toFixed(2)}`;
const eqCopy = () => ({gains: [...settings.gains], preamp: settings.preamp, maintain: settings.maintain, loudness_target: settings.loudness_target});
function normalize(value) {
  if (!value || !Array.isArray(value.gains) || value.gains.length !== 10 || !value.gains.every(v => typeof v === 'number' && Number.isFinite(v) && v >= -12 && v <= 12)) throw Error('Enter ten band gains between −12 and +12 dB.');
  if (typeof value.preamp !== 'number' || !Number.isFinite(value.preamp) || value.preamp < -24 || value.preamp > 12) throw Error('Preamp must be between −24 and +12 dB.');
  if (value.maintain !== undefined && typeof value.maintain !== 'boolean') throw Error('maintain must be true or false.');
  const target = value.loudness_target ?? (value.gains.reduce((a,b) => a+b, 0)/10 + value.preamp);
  if (typeof target !== 'number' || !Number.isFinite(target) || target < -36 || target > 24) throw Error('Invalid loudness target.');
  return {gains: [...value.gains], preamp: value.preamp, maintain: value.maintain ?? settings.maintain, loudness_target: target};
}
function compensate() {
  if (settings.maintain) settings.preamp = Math.max(-24, Math.min(12, settings.loudness_target - average()));
}
function displaySettings(forceText = false) {
  pairs.forEach(([range, number], i) => {
    const value = i === 10 ? settings.preamp : settings.gains[i];
    range.value = value;
    if (document.activeElement !== number) number.value = Number(value.toFixed(2));
    range.setAttribute('aria-valuetext', `${value.toFixed(2)} dB`);
  });
  $('maintain').checked = settings.maintain;
  const avg = average(), total = avg * 10;
  $('gain-summary').textContent = `Band total ${signed(total)} dB · Average ${signed(avg)} dB (${(10 ** (avg/20)).toFixed(2)}×) · With preamp ${signed(avg + settings.preamp)} dB`;
  const limited = settings.maintain && Math.abs(avg + settings.preamp - settings.loudness_target) > .01;
  $('compensation').textContent = 'Average is the sum of band gains ÷ 10; an estimate of gain, not measured loudness.' + (settings.maintain ? ` Holding ${signed(settings.loudness_target)} dB.` : '') + (limited ? ' Preamp limit reached.' : '');
  if (forceText || (document.activeElement !== $('eq-text') && !textInvalid)) $('eq-text').value = JSON.stringify(eqCopy());
}
function changed() {
  compensate(); displaySettings(); dirty = true;
  editSequence++; editOutput = latest?.settings.output || ''; retryDelay = 500;
  $('saved').textContent = 'Changes pending…';
  clearTimeout(timer); timer = setTimeout(sendEQ, 70);
}
function pair(range, number, change) {
  const update = event => {
    if (!event.target.validity.valid || event.target.value === '') return;
    const value = Number(event.target.value); range.value = number.value = value;
    change(value); changed();
  };
  range.addEventListener('input', update); number.addEventListener('input', update);
  number.addEventListener('blur', () => { number.value = range.value; });
  pairs.push([range, number]);
}
frequencies.forEach((frequency, index) => {
  const row = document.createElement('div'); row.className = 'band';
  const label = document.createElement('label'); label.htmlFor = `band-${index}`; label.textContent = frequency;
  const range = document.createElement('input'); range.type = 'range'; range.id = label.htmlFor;
  const number = document.createElement('input'); number.type = 'number';
  for (const input of [range, number]) { input.min = -12; input.max = 12; input.step = .1; input.value = 0; }
  range.setAttribute('aria-label', `${frequency} gain in dB`); number.setAttribute('aria-label', `${frequency} gain in dB, numeric`);
  row.append(label, range, number); $('bands').append(row);
  pair(range, number, value => settings.gains[index] = value);
});
pair($('preamp'), $('preamp-value'), value => { settings.preamp = value; settings.loudness_target = average() + value; });
$('maintain').addEventListener('change', () => {
  settings.maintain = $('maintain').checked; settings.loudness_target = average() + settings.preamp; changed();
});
$('reset').addEventListener('click', () => {
  settings = {gains: Array(10).fill(0), preamp: 0, maintain: settings.maintain, loudness_target: 0};
  textInvalid = false; $('text-error').textContent = ''; $('eq-text').removeAttribute('aria-invalid'); changed(); displaySettings(true);
});
async function mutation(path, body, method = 'PUT') {
  const response = await fetch(path, {method, headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)});
  const result = await response.json();
  if (!response.ok) {
    const error = Error(result.error || `HTTP ${response.status}`);
    error.retryable = response.status >= 500 || (response.status === 409 && result.error?.includes('disconnected'));
    throw error;
  }
  acceptedRevision = Math.max(acceptedRevision, result.revision || 0); return result;
}
async function action(path, body) {
  try { await mutation(path, body, 'POST'); localError = ''; }
  catch (error) { localError = error.message; }
  poll();
}
async function sendEQ() {
  if (sending || !dirty) return;
  clearTimeout(timer);
  const sequence = editSequence;
  sending = true; dirty = false;
  let retry = true, delay = 70;
  try {
    await mutation('/api/eq', {...eqCopy(), output: editOutput});
    eqError = ''; retryDelay = 500;
  } catch (error) {
    dirty = true; eqError = `EQ changes have not been sent: ${error.message}`;
    retry = error.retryable !== false || editSequence !== sequence;
    delay = retryDelay; retryDelay = Math.min(8000, retryDelay * 2);
  } finally {
    sending = false;
    if (dirty && retry) timer = setTimeout(sendEQ, delay);
    poll();
  }
}
async function selectOutput() {
  outputBusy = true; $('output').disabled = $('prefer-output').disabled = true;
  try { await mutation('/api/output', {name: $('output').value}); localError = ''; }
  catch (error) { localError = error.message; }
  finally { outputBusy = false; poll(); }
}
$('output').addEventListener('change', selectOutput);
$('prefer-output').addEventListener('click', selectOutput);
$('default').addEventListener('click', () => { $('default').disabled = true; action('/api/default', {}); });
$('eq-text').addEventListener('input', () => {
  try {
    const text = $('eq-text').value.trim();
    let value;
    if (text.startsWith('{') || text.startsWith('[')) value = JSON.parse(text);
    else {
      if (!text) throw Error('Enter EQ values to apply.');
      if (/^,|,$|,\s*,/.test(text)) throw Error('Enter a number between every comma.');
      value = text.split(/[\s,]+/).map(v => Number(v));
    }
    if (Array.isArray(value)) {
      if (value.length !== 10 && value.length !== 11) throw Error('Enter 10 band gains, optionally followed by preamp.');
      value = {gains: value.slice(0,10), preamp: value[10] ?? settings.preamp};
    }
    const next = normalize(value);
    if (!editing) throw Error('Connect the EQ before applying values.');
    settings = next; textInvalid = false; $('eq-text').removeAttribute('aria-invalid'); $('text-error').textContent = ''; changed();
  } catch (error) { textInvalid = true; $('eq-text').setAttribute('aria-invalid','true'); $('text-error').textContent = error.message; }
});
$('eq-text').addEventListener('blur', () => { if (!textInvalid) displaySettings(true); });
$('copy-values').addEventListener('click', async () => {
  textInvalid = false; $('text-error').textContent = ''; $('eq-text').removeAttribute('aria-invalid'); displaySettings(true);
  $('eq-text').focus(); $('eq-text').select();
  try { await navigator.clipboard.writeText($('eq-text').value); $('preset-status').textContent = 'Values copied.'; }
  catch { $('preset-status').textContent = 'Values selected. Press Ctrl+C to copy.'; }
});
$('preset-save').addEventListener('click', async () => {
  const name = $('preset-name').value.trim();
  if (!name) { $('preset-status').textContent = 'Enter a preset name.'; $('preset-name').focus(); return; }
  try {
    await mutation('/api/library', {action:'save', name, eq:eqCopy()}, 'POST');
    $('preset-status').textContent = `Saving “${name}”…`; localError = ''; poll();
  } catch (error) { $('preset-status').textContent = error.message; }
});
function loadEQ(eq, status) {
  if (!eq || !editing) return;
  let next = normalize(eq);
  if ($('maintain-load').checked) {
    const target = settings.maintain ? settings.loudness_target : average() + settings.preamp, mean = next.gains.reduce((sum, gain) => sum + gain, 0) / 10;
    next.maintain = settings.maintain;
    next.preamp = Math.max(-24, Math.min(12, target - mean));
    next.loudness_target = target;
  }
  settings = next; textInvalid = false; $('text-error').textContent = ''; $('eq-text').removeAttribute('aria-invalid');
  changed(); displaySettings(true); if (status) $('preset-status').textContent = status;
}
$('preset-load').addEventListener('click', () => {
  const name = $('preset-select').value, preset = latest?.library.presets[name];
  if (!preset) return;
  $('preset-name').value = name; loadEQ(preset, `Loaded “${name}”.`);
});
$('preset-delete').addEventListener('click', () => {
  const name = $('preset-select').value;
  if (name) { action('/api/library', {action:'delete', name}); $('preset-status').textContent = `Deleted “${name}”.`; }
});
$('preset-quickbar').addEventListener('click', async () => {
  const name = $('preset-select').value, preset = name && latest?.library.presets[name];
  const eq = preset || eqCopy();
  if (!preset && !editing) { $('preset-status').textContent = 'Choose a preset or connect the EQ first.'; return; }
  try {
    await mutation('/api/library', {action:'quickbar', eq}, 'POST');
    $('preset-status').textContent = preset ? `Added “${name}” to the quickbar.` : 'Added current EQ to the quickbar.';
    localError = ''; poll();
  } catch (error) { $('preset-status').textContent = error.message; }
});
$('preset-quickbar-clear').addEventListener('click', () => {
  action('/api/library', {action:'quickbar_clear'}); $('preset-status').textContent = 'Quickbar cleared.';
});
$('preset-select').addEventListener('change', () => { $('preset-name').value = $('preset-select').value; });
$('autoload').addEventListener('change', () => {
  const name = $('autoload').value; if (!name) return;
  $('preset-select').value = $('preset-name').value = name;
  loadEQ(latest?.library.presets[name], `Loaded “${name}”.`);
});
$('autoload').addEventListener('wheel', event => {
  const el = $('autoload'); if (!el.options.length) return;
  event.preventDefault();
  let i = el.selectedIndex;
  if (i < 0) i = event.deltaY > 0 ? 0 : el.options.length - 1;
  else i = Math.max(0, Math.min(el.options.length - 1, i + (event.deltaY > 0 ? 1 : -1)));
  if (el.selectedIndex === i) return;
  el.selectedIndex = i;
  const name = el.value; if (!name) return;
  $('preset-select').value = $('preset-name').value = name;
  loadEQ(latest?.library.presets[name], `Loaded “${name}”.`);
}, {passive:false});
$('remember-song').addEventListener('change', () => action('/api/library', {action:'remember', enabled:$('remember-song').checked}));
$('ignore-custom').addEventListener('change', () => action('/api/library', {action:'ignore_custom', enabled:$('ignore-custom').checked}));
$('fallback-eq').addEventListener('change', () => action('/api/library', {action:'fallback', enabled:$('fallback-eq').checked}));
$('fallback-device').addEventListener('change', () => action('/api/library', {action:'fallback_device', device:$('fallback-device').value}));
$('save-device-eq').addEventListener('click', async () => {
  try {
    await mutation('/api/library', {action:'device_eq', output:latest.settings.output, eq:eqCopy()}, 'POST');
    $('device-eq-status').textContent = 'Saving device EQ…'; poll();
  } catch (error) { $('device-eq-status').textContent = error.message; }
});
$('maintain-load').addEventListener('change', () => action('/api/library', {action:'maintain_load', enabled:$('maintain-load').checked}));
$('output-log').addEventListener('change', () => action('/api/library', {action:'output_log', enabled:$('output-log').checked}));
$('player').addEventListener('change', () => action('/api/library', {action:'player', player:$('player').value}));
for (const name of ['previous','playpause','next']) $(name).addEventListener('click', () => {
  const player = latest?.desktop.selected_player?.id;
  if (player) action('/api/player', {player, action:name});
});
$('seek-back').addEventListener('click', () => {
  const player = latest?.desktop.selected_player?.id;
  if (player) action('/api/player', {player, action:'seek_relative', offset:-20});
});
$('seek-forward').addEventListener('click', () => {
  const player = latest?.desktop.selected_player?.id;
  if (player) action('/api/player', {player, action:'seek_relative', offset:20});
});
$('player-progress').addEventListener('click', event => {
  const player = latest?.desktop.selected_player || {};
  if (!player.id || !player.CanControl || !player.CanSeek || !player.length) return;
  const rect = $('player-progress').getBoundingClientRect();
  const fraction = Math.min(1, Math.max(0, (event.clientX - rect.left) / (rect.width || 1)));
  action('/api/player', {player: player.id, action: 'seek', position: fraction * player.length});
});
const duration = seconds => `${Math.floor(Math.max(0,seconds || 0)/60)}:${String(Math.floor(Math.max(0,seconds || 0)%60)).padStart(2,'0')}`;
// The player owns position/status. Only the visible playback clock advances
// between observations; no requests or business-state mutations occur here.
function updateProgress() {
  clearTimeout(playerClock);
  const player = latest?.desktop.selected_player || {};
  const elapsed = player.status === 'Playing' && player.observed_at ? Math.max(0,Date.now()/1000-player.observed_at) : 0;
  const position = Math.min((player.position || 0) + elapsed, player.length || Infinity);
  $('player-progress').max = player.length || 1;
  $('player-progress').value = Math.min(position,player.length || 1);
  $('player-time').textContent = player.id ? `${duration(position)} / ${duration(player.length)}` : '';
  if (!document.hidden && player.status === 'Playing' && position < (player.length || Infinity))
    playerClock = setTimeout(updateProgress,1000);
}
function updatePlayer(desktop, library) {
  const signature = JSON.stringify([(desktop.players || []).map(p => [p.id,p.name]), library.player]);
  if (signature !== playerSignature) {
    const options = [new Option('Automatic · prefer playing','')];
    for (const player of desktop.players || []) options.push(new Option(player.name,player.id));
    if (library.player && !options.some(o => o.value === library.player)) options.push(new Option(`${library.player.replace('org.mpris.MediaPlayer2.','')} (unavailable)`,library.player));
    $('player').replaceChildren(...options); $('player').value = library.player; playerSignature = signature;
  }
  const player = desktop.selected_player || {}, playing = player.status === 'Playing';
  $('now-title').textContent = player.title || (player.id ? 'No track title' : 'No player available');
  $('now-details').textContent = [player.artist,player.album,player.status].filter(Boolean).join(' · ');
  $('previous').disabled = !(player.CanControl && player.CanGoPrevious);
  $('next').disabled = !(player.CanControl && player.CanGoNext);
  $('playpause').disabled = !(player.CanControl && (playing ? player.CanPause : player.CanPlay));
  $('playpause').textContent = playing ? 'Pause' : 'Play';
  const seekable = !!(player.id && player.CanControl && player.CanSeek && player.length);
  const canSeek = !!(player.id && player.CanControl && player.CanSeek);
  $('player-progress').disabled = !seekable;
  $('player-progress').setAttribute('aria-label', seekable ? 'Track progress; click to seek' : 'Track progress');
  updateProgress();
  $('seek-back').disabled = $('seek-forward').disabled = !canSeek;
  $('remember-song').checked = library.remember;
  $('ignore-custom').checked = library.ignore_custom;
  $('fallback-eq').checked = library.fallback;
  $('fallback-device').disabled = !library.fallback || !library.remember;
  const fallback = JSON.stringify([library.devices, library.fallback_device]);
  if (fallback !== fallbackSignature) {
    const options = [new Option('First saved device with this song', '')];
    for (const name of library.devices || []) options.push(new Option(name, name));
    if (library.fallback_device && !(library.devices || []).includes(library.fallback_device)) options.push(new Option(`${library.fallback_device} (no longer saved)`, library.fallback_device));
    $('fallback-device').replaceChildren(...options); $('fallback-device').value = library.fallback_device;
    fallbackSignature = fallback;
  }
  $('save-device-eq').disabled = !editing || dirty || sending;
  const general = library.device_eq;
  $('device-eq-values').textContent = general ? general.gains.map((v, i) => `${frequencies[i]}: ${signed(v)} dB`).join(' · ') + ` · Preamp: ${signed(general.preamp)} dB` : 'No general EQ saved yet.';
  if ($('device-eq-status').textContent.startsWith('Saving') && !library.pending && !library.error) $('device-eq-status').textContent = 'Device EQ saved.';
  $('output-log').checked = library.output_log !== false;
  $('song-status').textContent = library.remember ? library.song_status : 'Song recall off';
  $('player-error').textContent = desktop.player_error || '';
}
function sendMixer(token, actionName, value) {
  mixerHold.set(token,Date.now()+1800);
  clearTimeout(holdTimer);
  holdTimer = setTimeout(() => { if (latest && !document.hidden) updateMixer(latest.desktop); },1850);
  const body = {token,action:actionName,...(actionName === 'move' ? {target:value} : {value})};
  if (latest?.desktop.mixer.some(item => item.token === token && item.name === 'effect_input.eqweb')) body.output = latest.settings.output;
  const key = `${token}:${actionName}`;
  clearTimeout(mixerTimers.get(key));
  mixerTimers.set(key,setTimeout(() => { mixerTimers.delete(key); action('/api/mixer',body); },actionName === 'volume' ? 70 : 0));
}
const groups = {};
for (const [kind,title] of [['playback','Playback'],['recording','Recording'],['output','Output devices'],['input','Input devices']]) {
  const section = document.createElement('section'); section.className = 'mixer-group';
  const heading = document.createElement('h3'); heading.textContent = title;
  const rows = document.createElement('div'), empty = document.createElement('p'); empty.className = 'muted'; empty.textContent = 'No active controls';
  section.append(heading,rows,empty); $('mixer').append(section); groups[kind] = {rows,empty};
}
function makeMixerRow(item) {
  const row = document.createElement('div'); row.className = 'mixer-row'; row.dataset.token = item.token;
  const label = document.createElement('label'); label.className = 'mixer-label';
  const range = document.createElement('input'); range.type = 'range'; range.min = 0; range.max = 150; range.step = 1; range.id = `mixer-${item.kind}-${item.id}`; label.htmlFor = range.id;
  const number = document.createElement('input'); number.type = 'number'; number.min = 0; number.max = 150; number.step = 1;
  const mute = document.createElement('button'); mute.textContent = 'Mute';
  const route = document.createElement('select'); route.className = 'mixer-route';
  const update = event => {
    if (event.target.value === '' || !event.target.validity.valid) return;
    range.value = number.value = event.target.value; sendMixer(item.token,'volume',Number(event.target.value));
  };
  range.addEventListener('input',update); number.addEventListener('input',update);
  number.addEventListener('blur',() => number.value = range.value);
  mute.addEventListener('click',() => {
    const value = mute.getAttribute('aria-pressed') !== 'true'; mute.setAttribute('aria-pressed',String(value)); sendMixer(item.token,'mute',value);
  });
  route.addEventListener('change',() => sendMixer(item.token,'move',route.value));
  row.append(label,range,number,mute);
  if (item.kind === 'playback' || item.kind === 'recording') row.append(route);
  groups[item.kind].rows.append(row); return {row,label,range,number,mute,route,routeSignature:''};
}
function updateMixer(desktop) {
  const items = desktop.mixer || [], tokens = new Set(items.map(item => item.token));
  const masterChanged = desktop.master_ready && masterEpoch !== desktop.master_epoch;
  if (masterChanged) masterEpoch = desktop.master_epoch;
  for (const [token,row] of mixerRows) if (!tokens.has(token)) { row.row.remove(); mixerRows.delete(token); mixerHold.delete(token); }
  for (const group of Object.values(groups)) group.empty.hidden = false;
  for (const item of items) {
    if (!groups[item.kind]) continue;
    groups[item.kind].empty.hidden = true;
    if (!mixerRows.has(item.token)) mixerRows.set(item.token,makeMixerRow(item));
    const row = mixerRows.get(item.token), label = [item.application,item.description || item.name].filter(Boolean).join(' · ');
    row.label.textContent = label; row.range.setAttribute('aria-label',`${label} volume percent`); row.number.setAttribute('aria-label',`${label} volume percent, numeric`);
    const masterPending = item.name === 'effect_input.eqweb' && !desktop.master_ready;
    const restoreMaster = item.name === 'effect_input.eqweb' && masterChanged;
    if (restoreMaster) mixerHold.delete(item.token);
    row.range.disabled = row.number.disabled = !item.writable || masterPending;
    row.mute.disabled = masterPending; row.route.disabled = false;
    row.mute.setAttribute('aria-label',`Mute ${label}`); row.route.setAttribute('aria-label',`Route ${label}`);
    if (!(mixerHold.get(item.token) > Date.now())) {
      if (restoreMaster || (document.activeElement !== row.range && document.activeElement !== row.number)) row.range.value = row.number.value = Math.round(item.volume);
      row.mute.setAttribute('aria-pressed',String(item.mute));
    }
    const destinations = items.filter(other => other.kind === (item.kind === 'playback' ? 'output' : 'input'));
    const signature = JSON.stringify(destinations.map(o => [o.token,o.description]));
    if (signature !== row.routeSignature) { row.route.replaceChildren(...destinations.map(o => new Option(o.description || o.name,o.token))); row.routeSignature = signature; }
    if (document.activeElement !== row.route && !(mixerHold.get(item.token) > Date.now())) row.route.value = destinations.find(o => o.id === item.route)?.token || '';
  }
  const master = items.find(item => item.kind === 'output' && item.name === 'effect_input.eqweb');
  $('master-control').disabled = !master?.writable || !desktop.master_ready;
  const routed = master && items.some(item => item.kind === 'playback' && item.route === master.id);
  $('master-help').textContent = !master ? 'EQ volume is unavailable because EQ Web Sink is missing.' : !desktop.master_ready ? 'Restoring volume and mute for this output…' : routed ?
    'EQ volume only affects applications routed to EQ Web Sink. Other application and device volume controls still apply.' :
    'No applications are routed through the EQ. This volume control will not change direct playback. In Playback below, choose EQ Web Sink as the application’s output.';
  if (master && !(mixerHold.get(master.token) > Date.now())) {
    if (masterChanged || (document.activeElement !== $('master') && document.activeElement !== $('master-value'))) $('master').value = $('master-value').value = Math.round(master.volume);
    $('master-mute').setAttribute('aria-pressed',String(master.mute));
  }
  $('mixer-error').textContent = desktop.mixer_error || '';
}
for (const id of ['master','master-value']) $(id).addEventListener('input', event => {
  if (event.target.value === '' || !event.target.validity.valid) return;
  const master = latest?.desktop.mixer.find(item => item.kind === 'output' && item.name === 'effect_input.eqweb');
  if (!master) return;
  $('master').value = $('master-value').value = event.target.value; sendMixer(master.token,'volume',Number(event.target.value));
});
$('master-value').addEventListener('blur', () => $('master-value').value = $('master').value);
$('master-mute').addEventListener('click', () => {
  const master = latest?.desktop.mixer.find(item => item.kind === 'output' && item.name === 'effect_input.eqweb');
  if (!master) return;
  const value = $('master-mute').getAttribute('aria-pressed') !== 'true'; $('master-mute').setAttribute('aria-pressed',String(value)); sendMixer(master.token,'mute',value);
});
let polling = false, stateTag = '', stateRequest, stateRetry, playerClock, holdTimer;
function renderState(state) {
  latest = state;
    if (instance !== state.instance) { instance = state.instance; acceptedRevision = 0; masterEpoch = 0; mixerHold.clear(); }
    $('status').textContent = state.live.status;
    editing = state.live.editing; $('editor').disabled = !editing; $('preset-load').disabled = $('autoload').disabled = !editing;
    $('default').disabled = !state.live.can_set_default || state.live.default;
    $('default').textContent = state.live.default ? 'EQ is the default sink' : 'Use EQ as default';
    $('output').disabled = !state.live.can_select_output || outputBusy || dirty || sending;
    $('prefer-output').disabled = $('output').disabled || !state.live.output_present ||
      state.library.output_priority?.[0] === state.settings.output;
    if (!outputBusy && state.revision >= acceptedRevision) {
      const outputs = [...state.outputs];
      if (!outputs.some(o => o.name === state.settings.output)) outputs.unshift({name:state.settings.output,description:`${state.settings.output} (unavailable)`});
      const signature = JSON.stringify(outputs);
      if ($('output').dataset.options !== signature) { $('output').replaceChildren(...outputs.map(o => new Option(o.description,o.name))); $('output').dataset.options = signature; }
      $('output').value = state.settings.output;
    }
    if (!dirty && !sending && state.revision >= acceptedRevision) { settings = normalize(state.settings); displaySettings(); }
    updatePlayer(state.desktop,state.library); updateMixer(state.desktop);
    $('maintain-load').checked = state.library.maintain_load !== false;
    const signature = JSON.stringify(state.library.presets);
    if (signature !== presetSignature) {
      const selected = $('preset-select').value, autoSelected = $('autoload').value;
      const names = Object.keys(state.library.presets).sort();
      $('preset-select').replaceChildren(new Option('Choose a preset…',''),...names.map(name => new Option(name,name)));
      $('preset-select').value = selected;
      $('autoload').replaceChildren(...names.map(name => new Option(name,name)));
      if (names.includes(autoSelected)) $('autoload').value = autoSelected;
      presetSignature = signature;
    }
    const bars = state.library.quickbar || [];
    const barSignature = JSON.stringify(bars);
    if (barSignature !== quickbarSignature) {
      for (const button of [...$('quickbar').querySelectorAll('[data-quickbar]')]) button.remove();
      bars.forEach((eq, i) => {
        const button = document.createElement('button');
        const letter = String.fromCharCode(65 + i);
        button.type = 'button'; button.dataset.quickbar = String(i); button.textContent = letter;
        button.setAttribute('aria-label', `Load quickbar ${letter}`);
        button.addEventListener('click', () => loadEQ(eq, `Loaded quickbar ${letter}.`));
        $('quickbar').append(button);
      });
      quickbarSignature = barSignature;
    }
    for (const button of $('quickbar').querySelectorAll('[data-quickbar]')) button.disabled = !editing;
    if ($('preset-status').textContent.startsWith('Saving') && !state.library.pending && !state.library.error) $('preset-status').textContent = 'Preset saved.';
    $('errors').textContent = [eqError,localError,state.persistence_error && `Save failed: ${state.persistence_error}`,state.apply_error && `Apply failed: ${state.apply_error}`,state.output_error,state.default_error,state.library.error,state.library.log_error,state.desktop.action_error,state.desktop.master_error].filter(Boolean).join('\n');
    $('saved').textContent = state.setup_required ? 'Stop the editor and run ./eqweb setup to choose your audio output, then restart PipeWire once.' :
      dirty || sending || state.pending || state.library.pending || state.revision < acceptedRevision ? 'Applying and saving…' :
      state.persistence_error ? 'Settings have not been fully saved.' : state.apply_error ? 'Saved; live apply needs attention.' : state.live.awaiting_readback ? 'Settings saved; waiting for playback to confirm live values.' : 'Settings saved.';
}
async function poll(wait = false) {
  if (polling) { if (latest) renderState(latest); return; }
  if (document.hidden) return;
  clearTimeout(stateRetry);
  polling = true;
  const request = new AbortController(); stateRequest = request;
  let retry = 0;
  try {
    const response = await fetch(wait && stateTag ? '/api/state?wait=1' : '/api/state', {
      signal: request.signal, headers: wait && stateTag ? {'If-None-Match': stateTag} : {}
    });
    if (response.status === 304) return;
    if (!response.ok) throw Error(`HTTP ${response.status}`);
    const state = await response.json();
    if (request.signal.aborted) return;
    stateTag = response.headers.get('ETag') || '';
    renderState(state);
  } catch (error) {
    if (request.signal.aborted) return;
    retry = 2000;
    $('status').textContent = 'Editor disconnected'; $('errors').textContent = error.message; editing = false;
    $('editor').disabled = $('output').disabled = $('prefer-output').disabled = $('default').disabled = $('master-control').disabled = true;
    for (const id of ['previous','next','playpause','preset-load','seek-back','seek-forward','autoload','save-device-eq']) $(id).disabled = true;
    $('player-progress').disabled = true;
    for (const button of $('quickbar').querySelectorAll('[data-quickbar]')) button.disabled = true;
    for (const row of mixerRows.values()) row.range.disabled = row.number.disabled = row.mute.disabled = row.route.disabled = true;
  } finally {
    polling = false; stateRequest = null;
    if (!document.hidden) stateRetry = setTimeout(() => poll(true),retry);
  }
}
window.addEventListener('pagehide', () => {
  stateRequest?.abort(); clearTimeout(stateRetry); clearTimeout(playerClock);
  if (dirty) fetch('/api/eq', {method:'PUT',keepalive:true,headers:{'Content-Type':'application/json'},body:JSON.stringify({...eqCopy(),output:editOutput})}).catch(() => {});
});
document.addEventListener('visibilitychange', () => {
  clearTimeout(stateRetry); clearTimeout(playerClock);
  if (document.hidden) stateRequest?.abort();
  else { stateTag = ''; poll(); updateProgress(); }
});
window.addEventListener('pageshow', () => { stateTag = ''; poll(); });
displaySettings(); poll();
