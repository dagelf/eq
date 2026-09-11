#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${EQWEB_TEST_BUS:-} != 1 ]]; then exec dbus-run-session -- env EQWEB_TEST_BUS=1 bash "$0"; fi
temp=$(mktemp -d /tmp/eqweb-integration-XXXXXX)
echo "Test artifacts: $temp"
export XDG_RUNTIME_DIR="$temp/run" PIPEWIRE_RUNTIME_DIR="$temp/run" XDG_CONFIG_HOME="$temp/config" XDG_STATE_HOME="$temp/state"
export GIO_USE_VFS=local NO_AT_BRIDGE=1
unset PIPEWIRE_REMOTE
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d" "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d"
chmod 700 "$XDG_RUNTIME_DIR"
pw= wp= editor= recorder= silence= latency=
cleanup() { for pid in "$latency" "$recorder" "$silence" "$editor" "$wp" "$pw"; do if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; done; }
trap cleanup EXIT
cat > "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d/90-test.conf" <<'EOF'
wireplumber.profiles = { main = {
 monitor.alsa = disabled
 monitor.alsa-midi = disabled
 monitor.bluez = disabled
 monitor.bluez-midi = disabled
 monitor.v4l2 = disabled
 monitor.libcamera = disabled
 support.reserve-device = disabled
} }
EOF
cat > "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/10-test.conf" <<'EOF'
context.properties = { default.clock.rate = 48000 default.clock.quantum = 256 default.clock.min-quantum = 256 default.clock.max-quantum = 256 }
context.objects = [
 { factory = adapter args = { factory.name = support.null-audio-sink node.name = test.output node.description = "Test output A" media.class = Audio/Sink device.api = eqweb-test node.virtual = false audio.position = [ FL FR ] } }
 { factory = adapter args = { factory.name = support.null-audio-sink node.name = test.other node.description = "Test output B" media.class = Audio/Sink device.api = eqweb-test node.virtual = false audio.position = [ FL FR ] } }
 { factory = adapter args = { factory.name = support.null-audio-sink node.name = test.virtual node.description = "Virtual output" media.class = Audio/Sink device.api = eqweb-test node.virtual = true audio.position = [ FL FR ] } }
]
EOF
./eqweb setup --output test.output > "$temp/setup.log"
start_audio() {
  pipewire > "$temp/pipewire.log" 2>&1 & pw=$!
  for ((i=0;i<100;i++)); do [[ -S "$XDG_RUNTIME_DIR/pipewire-0" ]] && break; sleep .05; done
  wireplumber > "$temp/wireplumber.log" 2>&1 & wp=$!
}
start_editor() { ./eqweb > "$temp/editor.log" 2>&1 & editor=$!; }
state() { curl -fsS http://127.0.0.1:8932/api/state; }
await_state() {
  for ((i=0;i<120;i++)); do
    if state > "$temp/latest.json" 2>/dev/null && jq -e "$1" "$temp/latest.json" >/dev/null; then return; fi
    sleep .05
  done
  pw-dump > "$temp/dump.json" || true
  cat "$temp/latest.json" "$temp/editor.log" "$temp/pipewire.log" "$temp/wireplumber.log"; echo "Timed out: $1"; exit 1
}
put() { curl -fsS -X PUT -H 'Content-Type: application/json' -d "$2" "http://127.0.0.1:8932/api/$1" >/dev/null; }
start_audio
./eqweb outputs > "$temp/outputs.log"
rg -q '1\. Test output A' "$temp/outputs.log"
rg -q '2\. Test output B' "$temp/outputs.log"
rg -q './eqweb setup --output test.output' "$temp/outputs.log"
if rg -q 'test.virtual|effect_input.eqweb' "$temp/outputs.log"; then echo 'Listed a virtual output'; exit 1; fi
cp "$XDG_STATE_HOME/eqweb/settings-v1.json" "$temp/before-setup.json"
cp "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/99-eqweb.conf" "$temp/before-setup.conf"
if ./eqweb setup < /dev/null > "$temp/cancel.log"; then echo 'EOF did not cancel setup'; exit 1; fi
printf '\n' > "$temp/blank-choice"
if ./eqweb setup < "$temp/blank-choice" >> "$temp/cancel.log"; then echo 'Enter did not cancel setup'; exit 1; fi
cmp "$temp/before-setup.json" "$XDG_STATE_HOME/eqweb/settings-v1.json"
cmp "$temp/before-setup.conf" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/99-eqweb.conf"
printf '99\ninvalid\n2\n' > "$temp/choice"
./eqweb setup < "$temp/choice" > "$temp/chooser.log"
rg -q 'Enter a number from the list' "$temp/chooser.log"
jq -e '.output == "test.other"' "$XDG_STATE_HOME/eqweb/settings-v1.json" >/dev/null
printf '1\n' > "$temp/choice"
./eqweb setup --output < "$temp/choice" > "$temp/missing-output.log"
jq -e '.output == "test.output"' "$XDG_STATE_HOME/eqweb/settings-v1.json" >/dev/null
./eqweb setup --output '?' < "$temp/choice" > "$temp/question-output.log"
echo 'Setup: device discovery, virtual-output filtering, numbered selection, invalid input and cancellation passed.'
start_editor
await_state '.live.connected and (.pending | not) and .apply_error == ""'
usage() {
  local label=$1 before after stamp elapsed
  before=$(awk '{print $14+$15}' "/proc/$editor/stat"); stamp=$(date +%s%N)
  sleep 5
  after=$(awk '{print $14+$15}' "/proc/$editor/stat"); elapsed=$(( $(date +%s%N)-stamp ))
  awk -v label="$label" -v ticks="$((after-before))" -v hz="$(getconf CLK_TCK)" -v elapsed="$elapsed" 'BEGIN {printf "%s editor CPU: %.4f%% of one core over %.3f seconds\n",label,100*ticks/hz/(elapsed/1e9),elapsed/1e9}' | tee -a "$temp/usage.txt"
  awk -v label="$label" '/VmRSS/ {print label " editor " $0}' "/proc/$editor/status" | tee -a "$temp/usage.txt"
}
usage idle
sink_id=$(state | jq .live.sink_id)
put eq '{"gains":[0,0,0,0,0,6,0,0,0,0],"preamp":-3}'
await_state '(.pending | not) and .apply_error == "" and .saved.gains[5] == 6'
pw-cat --raw --playback --target effect_input.eqweb --latency 256 --rate 48000 --channels 2 --format f32 - < /dev/zero > "$temp/silence.log" 2>&1 & silence=$!
await_state '.live.eq.gains[5] == 6 and (.live.eq.preamp < -2.99) and (.pending | not) and .apply_error == ""'
for ((i=0;i<24;i++)); do put eq "{\"gains\":[0,0,0,0,0,$((i%12)),0,0,0,0],\"preamp\":-3}"; done
await_state ".live.eq.gains[5] == 11 and (.pending | not) and .live.sink_id == $sink_id and .applied_revision == .revision"
jq -e '.gains[5] == 11' "$XDG_STATE_HOME/eqweb/settings-v1.json" >/dev/null
chmod 500 "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d"
put eq '{"gains":[0,0,0,0,0,5,0,0,0,0],"preamp":-3}'
await_state '.live.eq.gains[5] == 5 and .saved.gains[5] == 5 and (.persistence_error | startswith("State saved; PipeWire configuration:")) and .apply_error == ""'
chmod 700 "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d"
await_state '(.pending | not) and .persistence_error == ""'
chmod 500 "$XDG_STATE_HOME/eqweb"
put eq '{"gains":[0,0,0,0,0,4,0,0,0,0],"preamp":-3}'
await_state '.live.eq.gains[5] == 4 and (.persistence_error | startswith("State file:")) and .apply_error == ""'
chmod 700 "$XDG_STATE_HOME/eqweb"
await_state '(.pending | not) and .persistence_error == ""'
put output '{"name":"test.other"}'
await_state '.settings.output == "test.other" and .live.connected and (.pending | not)'
# Return to flat before browser keyboard checks.
put eq '{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}'
await_state '(.pending | not) and .live.eq.gains[5] == 0'
if command -v chromium >/dev/null && [[ -x build/browser ]]; then
  build/browser "$temp" 2> "$temp/browser.log"
  base64 -d "$temp/browser.png.base64" > "$temp/browser.png"
fi
curl -fsS -X POST -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:8932/api/default >/dev/null
await_state '.live.default and .live.connected'
put output '{"name":"test.output"}'
await_state '.settings.output == "test.output" and .live.connected and (.pending | not)'
# Audio measurements are added below; all routes are confined to null devices.
if [[ -x build/probe ]]; then
  build/probe generate "$temp/tone.raw"
  measure() {
    local label=$1 left=$2 right=$3
    pw-cat --raw --record --target test.output --rate 48000 --channels 2 --format f32 -P '{ stream.capture.sink = true }' - > "$temp/$label.raw" 2> "$temp/record.log" & recorder=$!
    sleep .2
    pw-cat --raw --playback --target effect_input.eqweb --rate 48000 --channels 2 --format f32 - < "$temp/tone.raw" 2> "$temp/play.log"
    sleep .2
    kill "$recorder"; wait "$recorder" || true; recorder=
    build/probe analyze "$temp/$label.raw" "$left" "$right"
  }
  put eq '{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}'
  await_state '(.pending | not) and .live.eq.preamp > -0.001'
  measure flat 0 0
  build/probe noise "$temp/noise.raw"
  build/latency > "$temp/latency.txt" 2> "$temp/latency.log" & latency=$!
  for ((i=0;i<60;i++)); do if pw-link -i | rg -q 'eqweb-latency:processed'; then break; fi; sleep .05; done
  pw-link effect_input.eqweb:monitor_FL eqweb-latency:reference
  pw-link test.output:monitor_FL eqweb-latency:processed
  pw-cat --raw --playback --target effect_input.eqweb --latency 256 --rate 48000 --channels 2 --format f32 - < "$temp/noise.raw" 2> "$temp/noise.log"
  wait "$latency"; latency=
  cat "$temp/latency.txt"
  usage playback
  pw-top -b -n 3 > "$temp/pw-top.txt"
  put eq '{"gains":[0,0,0,0,0,6,0,0,0,0],"preamp":-3}'
  await_state '(.pending | not) and .live.eq.gains[5] == 6'
  measure boosted 3 -2.75
  # Terminate immediately after acceptance, before the 350 ms save debounce.
  put eq '{"gains":[0,0,0,0,0,6,0,0,0,0],"preamp":-6}'
  kill "$editor"; wait "$editor"; editor=
  jq -e '.preamp == -6' "$XDG_STATE_HOME/eqweb/settings-v1.json" >/dev/null
  measure editor-closed 0 -5.75
  start_editor
  await_state '.live.connected'
fi
kill "$wp"; wait "$wp" || true; wp=
kill "$pw"; wait "$pw" || true; pw=
await_state '(.live.pipewire | not) and (.live.editing | not)'
start_audio
await_state '.live.connected and (.pending | not) and .apply_error == ""'
state > "$temp/restarted.json"
if [[ -x build/probe ]]; then measure restarted 0 -5.75; fi
output_id=$(pw-dump | jq '.[] | select(.type == "PipeWire:Interface:Node" and .info.props["node.name"] == "test.output") | .id')
pw-cli destroy "$output_id"
await_state '(.live.output_present | not) and (.live.connected | not) and (.live.editing | not)'
put output '{"name":"test.other"}'
await_state '.live.connected and (.pending | not)'
echo 'Integration: readback, coalescing, stable sink, output switching, default sink, restart and output removal passed.'
