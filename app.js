'use strict';
const $ = id => document.getElementById(id);
const frequencies = ['31 Hz', '63 Hz', '125 Hz', '250 Hz', '500 Hz', '1 kHz', '2 kHz', '4 kHz', '8 kHz', '16 kHz'];
let settings = {gains: Array(10).fill(0), preamp: 0};
let dirty = false, sending = false, timer, localError = '', acceptedRevision = 0, outputBusy = false, instance;
const pairs = [];
function pair(range, number, change) {
  const update = event => {
    if (!event.target.validity.valid || event.target.value === '') return;
    const value = Number(event.target.value);
    range.value = number.value = value;
    range.setAttribute('aria-valuetext', `${value} dB`);
    change(value);
    dirty = true;
    $('saved').textContent = 'Changes pending…';
    clearTimeout(timer); timer = setTimeout(sendEQ, 70);
  };
  range.addEventListener('input', update);
  number.addEventListener('input', update);
  number.addEventListener('blur', () => { if (!number.validity.valid || number.value === '') number.value = range.value; });
  pairs.push([range, number]);
}
frequencies.forEach((frequency, index) => {
  const row = document.createElement('div'); row.className = 'band';
  const label = document.createElement('label'); label.htmlFor = `band-${index}`; label.textContent = frequency;
  const range = document.createElement('input'); range.type = 'range'; range.id = label.htmlFor;
  const number = document.createElement('input'); number.type = 'number';
  for (const input of [range, number]) { input.min = -12; input.max = 12; input.step = 0.1; input.value = 0; }
  range.setAttribute('aria-label', `${frequency} gain in dB`);
  number.setAttribute('aria-label', `${frequency} gain in dB, numeric`);
  row.append(label, range, number); $('bands').append(row);
  pair(range, number, value => settings.gains[index] = value);
});
pair($('preamp'), $('preamp-value'), value => settings.preamp = value);
function displaySettings() {
  pairs.forEach(([range, number], i) => {
    const value = i === 10 ? settings.preamp : settings.gains[i];
    range.value = value; number.value = Number(value.toFixed(1));
    range.setAttribute('aria-valuetext', `${value} dB`);
  });
}
async function mutation(path, body, method = 'PUT') {
  const response = await fetch(path, {method, headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)});
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
  acceptedRevision = Math.max(acceptedRevision, result.revision);
  return result;
}
async function sendEQ() {
  if (sending || !dirty) return;
  sending = true; dirty = false;
  try { await mutation('/api/eq', {gains: [...settings.gains], preamp: settings.preamp}); localError = ''; }
  catch (error) { localError = error.message; }
  finally { sending = false; if (dirty) sendEQ(); else poll(); }
}
$('reset').addEventListener('click', () => {
  settings = {gains: Array(10).fill(0), preamp: 0}; displaySettings(); dirty = true; sendEQ();
});
$('output').addEventListener('change', async () => {
  outputBusy = true; $('output').disabled = true;
  try { await mutation('/api/output', {name: $('output').value}); localError = ''; }
  catch (error) { localError = error.message; }
  finally { outputBusy = false; poll(); }
});
$('default').addEventListener('click', async () => {
  $('default').disabled = true;
  try { await mutation('/api/default', {}, 'POST'); localError = ''; }
  catch (error) { localError = error.message; }
  poll();
});
let polling = false;
async function poll() {
  if (polling) return;
  polling = true;
  try {
    const response = await fetch('/api/state');
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const state = await response.json();
    if (instance !== state.instance) { instance = state.instance; acceptedRevision = 0; }
    $('status').textContent = state.live.status;
    $('editor').disabled = !state.live.editing;
    $('default').disabled = !state.live.can_set_default || state.live.default;
    $('default').textContent = state.live.default ? 'EQ is the default sink' : 'Use EQ as default';
    $('output').disabled = !state.live.can_select_output || outputBusy;
    if (!outputBusy && state.revision >= acceptedRevision) {
      const outputs = [...state.outputs];
      if (!outputs.some(o => o.name === state.settings.output)) outputs.unshift({name: state.settings.output, description: `${state.settings.output} (unavailable)`});
      const signature = JSON.stringify(outputs);
      if ($('output').dataset.options !== signature) {
        $('output').replaceChildren(...outputs.map(o => new Option(o.description, o.name)));
        $('output').dataset.options = signature;
      }
      $('output').value = state.settings.output;
    }
    if (!dirty && !sending && state.revision >= acceptedRevision && !$('editor').contains(document.activeElement)) {
      settings = {gains: [...state.settings.gains], preamp: state.settings.preamp}; displaySettings();
    }
    $('errors').textContent = [localError, state.persistence_error && `Save failed: ${state.persistence_error}`, state.apply_error && `Apply failed: ${state.apply_error}`, state.output_error, state.default_error].filter(Boolean).join('\n');
    $('saved').textContent = state.setup_required ? 'Stop the editor and run ./eqweb setup to choose your audio output, then restart PipeWire once.' :
      dirty || sending || state.pending || state.revision < acceptedRevision ? 'Applying and saving…' :
      state.persistence_error ? 'Settings have not been fully saved.' : state.apply_error ? 'Saved; live apply needs attention.' : state.live.awaiting_readback ? 'Settings saved; waiting for playback to confirm live values.' : 'Settings saved.';
  } catch (error) {
    $('status').textContent = 'Editor disconnected'; $('errors').textContent = error.message;
    $('editor').disabled = $('output').disabled = $('default').disabled = true;
  } finally { polling = false; }
}
window.addEventListener('pagehide', () => {
  if (dirty) fetch('/api/eq', {method: 'PUT', keepalive: true, headers: {'Content-Type': 'application/json'}, body: JSON.stringify({gains: settings.gains, preamp: settings.preamp})}).catch(() => {});
});
poll(); setInterval(poll, 750);
