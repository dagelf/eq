#!/usr/bin/env bash
set -euo pipefail
export EQWEB_PORT=${EQWEB_PORT:-18933}
cd "$(dirname "$0")/.."
if [[ ${EQWEB_TEST_BUS:-} != 1 ]]; then exec dbus-run-session -- env EQWEB_TEST_BUS=1 bash "$0"; fi
temp=$(mktemp -d /tmp/eqweb-integration-XXXXXX)
echo "Test artifacts: $temp"
export XDG_RUNTIME_DIR="$temp/run" PIPEWIRE_RUNTIME_DIR="$temp/run" XDG_CONFIG_HOME="$temp/config" XDG_STATE_HOME="$temp/state"
export PULSE_SERVER="unix:$temp/run/pulse/native"
export GIO_USE_VFS=local NO_AT_BRIDGE=1
unset PIPEWIRE_REMOTE
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d" "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d"
chmod 700 "$XDG_RUNTIME_DIR"
pw= wp= pulse= player= stream= capture= editor= recorder= silence= latency=
cleanup() { for pid in "$latency" "$recorder" "$silence" "$stream" "$capture" "$player" "$editor" "$pulse" "$wp" "$pw"; do if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; done; }
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
  if [[ -z "$pulse" ]]; then pipewire-pulse > "$temp/pulse.log" 2>&1 & pulse=$!; fi
}
start_editor() { ./eqweb > "$temp/editor.log" 2>&1 & editor=$!; }
state() { curl -fsS http://127.0.0.1:$EQWEB_PORT/api/state | jq -e --arg prefix "$editor-" 'select(.instance | startswith($prefix))'; }
await_state() {
  for ((i=0;i<120;i++)); do
    if state > "$temp/latest.json" 2>/dev/null && jq -e "$1" "$temp/latest.json" >/dev/null; then return; fi
    sleep .05
  done
  pw-dump > "$temp/dump.json" || true
  cat "$temp/latest.json" "$temp/editor.log" "$temp/pipewire.log" "$temp/wireplumber.log"; echo "Timed out: $1"; exit 1
}
put() { curl -fsS -X PUT -H 'Content-Type: application/json' -d "$2" "http://127.0.0.1:$EQWEB_PORT/api/$1" >/dev/null; }
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
# A meter/recording tap on EQ playback must not be mistaken for another output.
pw-cat --raw --record --target 0 --rate 48000 --channels 2 --format f32 -P '{ stream.capture.sink = true stream.monitor = true node.name = eqweb-test-meter }' - > /dev/null 2> "$temp/meter.log" & capture=$!
meter_linked=false
for ((i=0;i<100;i++)); do
  pw-link effect_output.eqweb:output_FL eqweb-test-meter:input_FL 2>/dev/null || true
  pw-link effect_output.eqweb:output_FR eqweb-test-meter:input_FR 2>/dev/null || true
  if pw-dump | jq -e '
    (map(select(.type == "PipeWire:Interface:Node" and .info.props["node.name"] == "effect_output.eqweb"))[0].id) as $play |
    (map(select(.type == "PipeWire:Interface:Node" and .info.props["node.name"] == "eqweb-test-meter"))[0].id) as $meter |
    any(.[]; .type == "PipeWire:Interface:Link" and .info["output-node-id"] == $play and .info["input-node-id"] == $meter and (.info.state == "active" or .info.state == "paused"))' >/dev/null; then meter_linked=true; break; fi
  sleep .05
done
[[ $meter_linked == true ]]
sleep .2
await_state '.live.connected and .live.editing and .output_error == ""'
put output '{"name":"test.output"}'
await_state '.live.connected and (.pending | not) and .output_error == ""'
kill "$capture"; wait "$capture" || true; capture=
echo 'EQ remains editable with a playback meter attached.'
await_state '.desktop.master_ready and any(.desktop.mixer[]; .name == "effect_input.eqweb")'
post() { curl -fsS -X POST -H 'Content-Type: application/json' -d "$2" "http://127.0.0.1:$EQWEB_PORT/api/$1" >/dev/null; }
master_token=$(state | jq -r '.desktop.mixer[] | select(.kind == "output" and .name == "effect_input.eqweb") | .token')
post mixer "$(jq -nc --arg token "$master_token" '{token:$token,action:"volume",value:75}')"
await_state 'any(.desktop.mixer[]; .name == "effect_input.eqweb" and (.volume > 74.9 and .volume < 75.1))'
post mixer "$(jq -nc --arg token "$master_token" '{token:$token,action:"mute",value:true}')"
await_state 'any(.desktop.mixer[]; .name == "effect_input.eqweb" and .mute)'
post mixer "$(jq -nc --arg token "$master_token" '{token:$token,action:"mute",value:false}')"
post mixer "$(jq -nc --arg token "$master_token" '{token:$token,action:"volume",value:100}')"
await_state 'any(.desktop.mixer[]; .name == "effect_input.eqweb" and (.mute | not) and .volume == 100)'
# General EQ and master controls belong to each selected physical output.
master() {
  local token
  token=$(state | jq -r '.desktop.mixer[] | select(.name == "effect_input.eqweb" and .kind == "output") | .token')
  post mixer "$(jq -nc --arg token "$token" --arg action "$1" --argjson value "$2" '{token:$token,action:$action,value:$value}')"
}
put eq '{"gains":[4,0,0,0,0,0,0,0,0,0],"preamp":-2}'
master volume 75
master mute true
await_state '.desktop.master_ready and any(.desktop.mixer[]; .name == "effect_input.eqweb" and .mute and (.volume-75|fabs)<.01)'
put output '{"name":"test.other"}'
await_state '.settings.output == "test.other" and .live.connected and .desktop.master_ready'
put eq '{"gains":[-5,0,0,0,0,0,0,0,0,0],"preamp":-3}'
master volume 35
master mute false
await_state 'any(.desktop.mixer[]; .name == "effect_input.eqweb" and (.mute|not) and (.volume-35|fabs)<.01)'
put output '{"name":"test.output"}'
await_state '.settings.gains[0] == 4 and .settings.preamp == -2 and .desktop.master_ready and any(.desktop.mixer[]; .name == "effect_input.eqweb" and .mute and (.volume-75|fabs)<.01)'
status=$(curl -sS -o "$temp/stale-eq.json" -w '%{http_code}' -X PUT -H 'Content-Type: application/json' -d '{"gains":[9,0,0,0,0,0,0,0,0,0],"preamp":0,"output":"test.other"}' "http://127.0.0.1:$EQWEB_PORT/api/eq")
[[ $status == 409 ]]
# External mixer changes are also remembered. Queued volume precedes a switch.
pactl set-sink-volume effect_input.eqweb 63%
pactl set-sink-mute effect_input.eqweb 0
await_state '(.desktop.output_volumes["test.output"].volume-63|fabs)<.01 and (.desktop.output_volumes["test.output"].mute|not)'
master volume 66
put output '{"name":"test.other"}'
await_state '.desktop.master_ready and .settings.gains[0] == -5 and .settings.preamp == -3 and any(.desktop.mixer[]; .name == "effect_input.eqweb" and (.mute|not) and (.volume-35|fabs)<.01)'
put output '{"name":"test.output"}'
put output '{"name":"test.other"}'
put output '{"name":"test.output"}'
await_state '.desktop.master_ready and .settings.gains[0] == 4 and any(.desktop.mixer[]; .name == "effect_input.eqweb" and (.mute|not) and (.volume-66|fabs)<.01)'
kill "$editor"; wait "$editor"; editor=
pactl set-sink-volume effect_input.eqweb 11%
pactl set-sink-mute effect_input.eqweb 1
start_editor
await_state '.desktop.master_ready and .live.connected and .settings.gains[0] == 4 and .settings.preamp == -2 and any(.desktop.mixer[]; .name == "effect_input.eqweb" and (.mute|not) and (.volume-66|fabs)<.01)'
put output '{"name":"test.other"}'
await_state '.desktop.master_ready and .settings.gains[0] == -5 and any(.desktop.mixer[]; .name == "effect_input.eqweb" and (.volume-35|fabs)<.01)'
# Leave both test outputs flat and at unity for subsequent audio measurements.
for output in test.other test.output; do
  put output "$(jq -nc --arg name "$output" '{name:$name}')"
  await_state ".settings.output == \"$output\" and .live.connected and .desktop.master_ready"
  put eq '{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}'
  master volume 100
  master mute false
  await_state '(.pending|not) and (.library.pending|not) and any(.desktop.mixer[]; .name == "effect_input.eqweb" and .volume == 100 and (.mute|not))'
done
echo 'Per-output EQ, master volume/mute, external mixer edits, queued edits, rapid switching and restart recall passed.'
pacat --playback --raw --device=effect_input.eqweb --client-name='EQ test playback' < /dev/zero > "$temp/pacat.log" 2>&1 & stream=$!
parec --raw --device=test.output.monitor --client-name='EQ test recording' > /dev/null 2> "$temp/parec.log" & capture=$!
await_state 'any(.desktop.mixer[]; .kind == "playback" and .application == "EQ test playback") and any(.desktop.mixer[]; .kind == "recording" and .application == "EQ test recording")'
for kind in playback recording; do
  token=$(state | jq -r --arg kind "$kind" '.desktop.mixer[] | select(.kind == $kind and .application == ("EQ test " + $kind)) | .token')
  post mixer "$(jq -nc --arg token "$token" '{token:$token,action:"volume",value:65}')"
  await_state "any(.desktop.mixer[]; .token == \"$token\" and (.volume > 64.9 and .volume < 65.1))"
  post mixer "$(jq -nc --arg token "$token" '{token:$token,action:"mute",value:true}')"
  await_state "any(.desktop.mixer[]; .token == \"$token\" and .mute)"
done
playback_token=$(state | jq -r '.desktop.mixer[] | select(.kind == "playback" and .application == "EQ test playback") | .token')
other_token=$(state | jq -r '.desktop.mixer[] | select(.kind == "output" and .name == "test.other") | .token')
other_id=$(state | jq '.desktop.mixer[] | select(.kind == "output" and .name == "test.other") | .id')
post mixer "$(jq -nc --arg token "$playback_token" --arg target "$other_token" '{token:$token,action:"move",target:$target}')"
await_state "any(.desktop.mixer[]; .token == \"$playback_token\" and .route == $other_id)"
kill "$stream" "$capture"; wait "$stream" || true; wait "$capture" || true; stream= capture=
# Exercise exact saved-preamp recall with load compensation explicitly off.
post library '{"action":"maintain_load","enabled":false}'
build/test-player > "$temp/player.log" 2>&1 & player=$!
await_state '.desktop.selected_player.title == "Song A" and .library.song_count == 0'
put eq '{"gains":[4,0,0,0,0,0,0,0,0,0],"preamp":0}'
await_state '.settings.gains[0] == 4 and (.pending | not)'
post player '{"player":"org.mpris.MediaPlayer2.eqweb_test","action":"next"}'
await_state '.desktop.selected_player.title == "Song B" and .library.song_count == 1'
put eq '{"gains":[-3,0,0,0,0,0,0,0,0,0],"preamp":0}'
await_state '.settings.gains[0] == -3 and (.pending | not)'
post player '{"player":"org.mpris.MediaPlayer2.eqweb_test","action":"previous"}'
await_state '.desktop.selected_player.title == "Song A" and .settings.gains[0] == 4 and (.pending | not)'
put output '{"name":"test.other"}'
await_state '.live.connected and .settings.gains[0] == 0'
put eq '{"gains":[2,0,0,0,0,0,0,0,0,0],"preamp":-1,"output":"test.other"}'
put output '{"name":"test.output"}'
await_state '.settings.gains[0] == 4 and .live.connected'
put output '{"name":"test.other"}'
await_state '.settings.gains[0] == 2 and .settings.preamp == -1 and .live.connected'
put output '{"name":"test.output"}'
await_state '.settings.gains[0] == 4 and .live.connected'
post player '{"player":"org.mpris.MediaPlayer2.eqweb_test","action":"playpause"}'
await_state '.desktop.selected_player.status == "Paused"'
post player '{"player":"org.mpris.MediaPlayer2.eqweb_test","action":"seek","position":90}'
await_state '.desktop.selected_player.position == 90 and .desktop.action_error == ""'
post player '{"player":"org.mpris.MediaPlayer2.eqweb_test","action":"seek_relative","offset":20}'
await_state '.desktop.selected_player.position == 110 and .desktop.action_error == ""'
post player '{"player":"org.mpris.MediaPlayer2.eqweb_test","action":"seek_relative","offset":-20}'
await_state '.desktop.selected_player.position == 90 and .desktop.action_error == ""'
await_state '.library.presets["Test Artist - Song A (Auto)"].gains[0] == 4'
rg -q 'Song A' "$XDG_STATE_HOME/eqweb/outputlog.txt"
put output '{"name":"test.other"}'
await_state '.desktop.master_ready and .live.connected and .settings.gains[0] == 2'
master volume 71
master mute true
await_state 'any(.desktop.mixer[]; .name == "effect_input.eqweb" and .mute and (.volume-71|fabs)<.01)'
post player '{"player":"org.mpris.MediaPlayer2.eqweb_test","action":"next"}'
await_state '.desktop.selected_player.title == "Song B" and .settings.gains[0] == 0 and .library.device_eq.gains[0] == 0'
post library '{"action":"fallback_device","device":"test.output"}'
post library '{"action":"fallback","enabled":true}'
await_state '.settings.gains[0] == -3 and (.library.song_status|contains("test.output")) and .library.device_eq.gains[0] == 0 and any(.desktop.mixer[]; .name == "effect_input.eqweb" and .mute and (.volume-71|fabs)<.01)'
post library '{"action":"fallback_device","device":"saved.but.removed"}'
await_state '.settings.gains[0] == -3 and .library.fallback_device == "saved.but.removed"'
post library '{"action":"fallback","enabled":false}'
await_state '.settings.gains[0] == 0 and any(.desktop.mixer[]; .name == "effect_input.eqweb" and .mute and (.volume-71|fabs)<.01)'
post library '{"action":"fallback_device","device":"test.output"}'
master volume 100
master mute false
await_state 'any(.desktop.mixer[]; .name == "effect_input.eqweb" and (.mute|not) and .volume == 100)'
put output '{"name":"test.output"}'
await_state '.desktop.master_ready and .live.connected and .settings.gains[0] == -3'
post player '{"player":"org.mpris.MediaPlayer2.eqweb_test","action":"previous"}'
await_state '.desktop.selected_player.title == "Song A" and .settings.gains[0] == 4'
echo 'Song fallback: own device, preferred saved device, missing preferred device, general EQ, and independent master volume passed.'
post library '{"action":"remember","enabled":false}'
put eq '{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}'
await_state '.settings.gains[0] == 0 and (.pending | not)'
echo 'Desktop: master volume/mute, playback/recording volumes, stream routing, player metadata/controls, progress seek, relative seek, auto presets, song log and per-song EQ recall passed.'
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
post library '{"action":"maintain_load","enabled":true}'
put eq '{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}'
await_state '(.pending | not) and .live.eq.gains[5] == 0'
if command -v chromium >/dev/null && [[ -x build/browser ]]; then
  build/browser "$temp" 2> "$temp/browser.log"
  base64 -d "$temp/browser.png.base64" > "$temp/browser.png"
fi
curl -fsS -X POST -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:$EQWEB_PORT/api/default >/dev/null
await_state '.live.default and .live.connected'
post default '{}'
sleep 2.1
await_state '.live.default and .default_error == ""'
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
await_state '.live.connected and .settings.output == "test.other" and (.pending | not)'
put output '{"name":"test.other"}'
await_state '.live.connected and (.pending | not)'
echo 'Integration: readback, coalescing, stable sink, output switching, default sink, restart and output removal passed.'

# Automate playback only while EQ is default, and do not fight later manual moves.
pactl set-default-sink test.other
await_state '(.live.default | not)'
pacat --playback --raw --device=test.other --client-name='EQ auto-route test' < /dev/zero > "$temp/auto-playback.log" 2>&1 & stream=$!
await_state 'any(.desktop.mixer[]; .application == "EQ auto-route test")'
auto_id=$(state | jq '.desktop.mixer[] | select(.application == "EQ auto-route test") | .id')
other_id=$(state | jq '.desktop.mixer[] | select(.kind == "output" and .name == "test.other") | .id')
eq_id=$(state | jq '.desktop.mixer[] | select(.kind == "output" and .name == "effect_input.eqweb") | .id')
sleep 1
await_state "any(.desktop.mixer[]; .id == $auto_id and .kind == \"playback\" and .route == $other_id)"
post default '{}'
await_state ".live.default and any(.desktop.mixer[]; .id == $auto_id and .kind == \"playback\" and .route == $eq_id)"
pactl move-sink-input "$auto_id" test.other
await_state "any(.desktop.mixer[]; .id == $auto_id and .kind == \"playback\" and .route == $other_id)"
sleep 1
await_state "any(.desktop.mixer[]; .id == $auto_id and .kind == \"playback\" and .route == $other_id)"
kill "$stream"; wait "$stream" || true; stream=
pacat --playback --raw --device=test.other --client-name='EQ new-stream test' < /dev/zero > "$temp/auto-new.log" 2>&1 & stream=$!
await_state "any(.desktop.mixer[]; .application == \"EQ new-stream test\" and .route == $eq_id)"
kill "$stream"; wait "$stream" || true; stream=
pacat --playback --raw --device=test.virtual --client-name='EQ other-device test' < /dev/zero > "$temp/auto-other.log" 2>&1 & stream=$!
virtual_id=$(state | jq '.desktop.mixer[] | select(.kind == "output" and .name == "test.virtual") | .id')
await_state "any(.desktop.mixer[]; .application == \"EQ other-device test\" and .route == $virtual_id)"
sleep 1
await_state "any(.desktop.mixer[]; .application == \"EQ other-device test\" and .route == $virtual_id)"
kill "$stream"; wait "$stream" || true; stream=
echo 'Automation: existing and new playback, other defaults/devices, manual overrides and filter loop exclusion passed.'

# Reproduce Bluetooth endpoint renaming without Bluetooth hardware or a server restart.
mock_output() {
  pw-cli create-node adapter "{ factory.name = support.null-audio-sink node.name = \"$1\" node.description = \"Mock headphones\" media.class = Audio/Sink device.api = eqweb-test node.virtual = false object.linger = true audio.position = [ FL FR ] }"
}
mock_output bluez_output.AA_BB_CC_DD_EE_01.1
await_state 'any(.outputs[]; .name == "bluez_output.AA_BB_CC_DD_EE_01.1")'
put output '{"name":"bluez_output.AA_BB_CC_DD_EE_01.1"}'
await_state '.live.connected and (.pending | not)'
eq_node=$(state | jq '.live.sink_id')
old_node=$(pw-dump | jq '.[] | select(.type == "PipeWire:Interface:Node" and .info.props["node.name"] == "bluez_output.AA_BB_CC_DD_EE_01.1") | .id')
pw-cli destroy "$old_node"
mock_output bluez_output.AA:BB:CC:DD:EE:02
await_state '.live.connected and .settings.output == "test.other" and (.pending | not)'
mock_output bluez_output.AA:BB:CC:DD:EE:01
await_state ".live.connected and .settings.output == \"bluez_output.AA:BB:CC:DD:EE:01\" and (.pending | not) and .live.sink_id == $eq_node"
jq -e '.output == "bluez_output.AA:BB:CC:DD:EE:01"' "$XDG_STATE_HOME/eqweb/settings-v1.json" >/dev/null
# Legacy saved names are also repaired when the editor starts, without recreating EQ.
kill "$editor"; wait "$editor"; editor=
./eqweb setup --output bluez_output.AA_BB_CC_DD_EE_01.9 > "$temp/legacy-setup.log"
mock_output bluez_output.AA_BB_CC_DD_EE_01.2
start_editor
await_state 'any(.outputs[]; .name == "bluez_output.AA_BB_CC_DD_EE_01.2") and any(.outputs[]; .name == "bluez_output.AA:BB:CC:DD:EE:01")'
sleep 1
await_state '.live.connected and .settings.output == "test.other" and (.pending | not)'
ambiguous_node=$(pw-dump | jq '.[] | select(.type == "PipeWire:Interface:Node" and .info.props["node.name"] == "bluez_output.AA_BB_CC_DD_EE_01.2") | .id')
pw-cli destroy "$ambiguous_node"
await_state ".live.connected and .settings.output == \"bluez_output.AA:BB:CC:DD:EE:01\" and (.pending | not) and .live.sink_id == $eq_node"
echo 'Automation: Bluetooth name changes, unrelated/ambiguous device exclusion, persistence and legacy startup recovery passed.'

# Selection history survives fallback and editor restart. Reselecting promotes a device.
mock_output test.third
await_state 'any(.outputs[]; .name == "test.third")'
put output '{"name":"test.third"}'
await_state '.live.connected and .settings.output == "test.third"'
put output '{"name":"bluez_output.AA:BB:CC:DD:EE:01"}'
await_state '.live.connected and .settings.output == "bluez_output.AA:BB:CC:DD:EE:01" and (.library.pending | not)'
remove_output() {
  local node
  node=$(pw-dump | jq --arg name "$1" '.[] | select(.type == "PipeWire:Interface:Node" and .info.props["node.name"] == $name) | .id')
  pw-cli destroy "$node"
}
remove_output bluez_output.AA:BB:CC:DD:EE:01
await_state '.live.connected and .settings.output == "test.third" and (.pending | not)'
remove_output test.third
await_state '.live.connected and .settings.output == "test.other" and (.pending | not)'
remove_output test.other
await_state '(.live.output_present | not) and (.live.editing | not) and .settings.output == "test.other"'
# Unselected Bluetooth 02 remains available but must not be adopted.
kill "$editor"; wait "$editor"; editor=
start_editor
await_state '(.live.output_present | not) and .settings.output == "test.other"'
mock_output test.third
await_state '.live.connected and .settings.output == "test.third"'
mock_output bluez_output.AA_BB_CC_DD_EE_01.3
await_state '.live.connected and .settings.output == "bluez_output.AA_BB_CC_DD_EE_01.3"'
put output '{"name":"test.third"}'
await_state '.live.connected and .settings.output == "test.third" and .library.output_priority[0] == "test.third" and (.library.pending | not)'
kill "$editor"; wait "$editor"; editor=
start_editor
await_state '.live.connected and .settings.output == "test.third"'
remove_output test.third
await_state '.live.connected and .settings.output == "bluez_output.AA_BB_CC_DD_EE_01.3"'
echo 'Output priority: cascading fallback, no selected outputs, reconnect, reselection and restart passed.'
