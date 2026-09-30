#!/usr/bin/env bash
set -euo pipefail
export EQWEB_PORT=${EQWEB_PORT:-18932}
cd "$(dirname "$0")/.."
temp=$(mktemp -d /tmp/eqweb-http-XXXXXX)
pid=
cleanup() { local result=$?; if ((result)); then cat "$temp/log" "$temp/state.json" 2>/dev/null || true; fi; if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; rm -rf "$temp"; }
trap cleanup EXIT
export XDG_STATE_HOME="$temp/state" XDG_CONFIG_HOME="$temp/config" PIPEWIRE_RUNTIME_DIR="$temp"
export PULSE_SERVER="unix:$temp/pulse/native" DBUS_SESSION_BUS_ADDRESS="unix:path=$temp/no-bus"
./eqweb > "$temp/log" 2>&1 & pid=$!
for ((i=0;i<100;i++)); do if curl -fsS http://127.0.0.1:$EQWEB_PORT/api/state > "$temp/state.json" 2>/dev/null; then break; fi; sleep .05; done
jq -e --arg prefix "$pid-" '(.instance | startswith($prefix)) and .setup_required and (.live.connected | not)' "$temp/state.json" >/dev/null
check() {
  local expected=$1; shift
  local actual
  actual=$(curl -sS -o "$temp/body" -w '%{http_code}' "$@")
  if [[ "$actual" != "$expected" ]]; then cat "$temp/body"; echo "Expected $expected, got $actual"; exit 1; fi
}
check 200 http://127.0.0.1:$EQWEB_PORT/
check 200 http://127.0.0.1:$EQWEB_PORT/app.js
check 403 -H 'Host: attacker.example' http://127.0.0.1:$EQWEB_PORT/api/state
check 403 -X PUT -H 'Content-Type: application/json' -H 'Origin: https://evil.example' -d '{}' http://127.0.0.1:$EQWEB_PORT/api/eq
check 403 -X PUT -H 'Content-Type: application/json' -H 'Sec-Fetch-Site: cross-site' -d '{}' http://127.0.0.1:$EQWEB_PORT/api/eq
check 415 -X PUT -d '{}' http://127.0.0.1:$EQWEB_PORT/api/eq
check 400 -X PUT -H 'Content-Type: application/json' -d '{"gains":[0],"preamp":0}' http://127.0.0.1:$EQWEB_PORT/api/eq
check 400 -X PUT -H 'Content-Type: application/json' -d '{} {}' http://127.0.0.1:$EQWEB_PORT/api/eq
check 409 -X PUT -H 'Content-Type: application/json' -d '{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}' http://127.0.0.1:$EQWEB_PORT/api/eq
check 409 -X PUT -H 'Content-Type: application/json' -d '{"name":"alsa_output.missing"}' http://127.0.0.1:$EQWEB_PORT/api/output
check 400 -X PUT -H 'Content-Type: application/json' -d '{"name":"effect_input.eqweb"}' http://127.0.0.1:$EQWEB_PORT/api/output
check 409 -X POST -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:$EQWEB_PORT/api/default
check 403 -X POST -H 'Content-Type: application/json' -H 'Origin: https://evil.example' -d '{}' http://127.0.0.1:$EQWEB_PORT/api/mixer
check 400 -X POST -H 'Content-Type: application/json' -d '{"token":"missing","action":"volume","value":151}' http://127.0.0.1:$EQWEB_PORT/api/mixer
check 400 -X POST -H 'Content-Type: application/json' -d '{"player":"missing","action":"exec"}' http://127.0.0.1:$EQWEB_PORT/api/player
check 400 -X POST -H 'Content-Type: application/json' -d '{"player":"missing","action":"seek","position":-1}' http://127.0.0.1:$EQWEB_PORT/api/player
check 400 -X POST -H 'Content-Type: application/json' -d '{"player":"missing","action":"seek","position":"90"}' http://127.0.0.1:$EQWEB_PORT/api/player
check 400 -X POST -H 'Content-Type: application/json' -d '{"player":"missing","action":"seek"}' http://127.0.0.1:$EQWEB_PORT/api/player
check 400 -X POST -H 'Content-Type: application/json' -d '{"player":"missing","action":"seek_relative"}' http://127.0.0.1:$EQWEB_PORT/api/player
check 400 -X POST -H 'Content-Type: application/json' -d '{"player":"missing","action":"seek_relative","offset":"20"}' http://127.0.0.1:$EQWEB_PORT/api/player
check 400 -X POST -H 'Content-Type: application/json' -d '{"action":"save","name":"Bad","eq":{"gains":[1],"preamp":0}}' http://127.0.0.1:$EQWEB_PORT/api/library
check 400 -X POST -H 'Content-Type: application/json' -d '{"action":"quickbar","eq":{"gains":[1],"preamp":0}}' http://127.0.0.1:$EQWEB_PORT/api/library
check 400 -X POST -H 'Content-Type: application/json' -d '{"action":"maintain_load"}' http://127.0.0.1:$EQWEB_PORT/api/library
check 400 -X POST -H 'Content-Type: application/json' -d '{"action":"fallback","enabled":"yes"}' http://127.0.0.1:$EQWEB_PORT/api/library
check 400 -X POST -H 'Content-Type: application/json' -d '{"action":"fallback_device","device":null}' http://127.0.0.1:$EQWEB_PORT/api/library
check 400 -X POST -H 'Content-Type: application/json' -d '{"action":"device_eq","output":"wrong.output","eq":{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}}' http://127.0.0.1:$EQWEB_PORT/api/library
head -c 4097 /dev/zero > "$temp/large"
check 413 -X PUT -H 'Content-Type: application/json' --data-binary "@$temp/large" http://127.0.0.1:$EQWEB_PORT/api/eq
if ./eqweb setup --output test.output > "$temp/lock.log" 2>&1; then echo 'Setup bypassed process lock'; exit 1; fi
kill "$pid"; wait "$pid"; pid=
if ./eqweb setup < /dev/null > "$temp/discovery.log" 2>&1; then echo 'Setup succeeded without PipeWire'; exit 1; fi
rg -q 'Cannot connect to PipeWire' "$temp/discovery.log"
test ! -e "$XDG_STATE_HOME/eqweb/settings-v1.json"
if ./eqweb print-config > "$temp/usage.log" 2>&1; then echo 'Missing output accepted for print-config'; exit 1; fi
rg -q './eqweb setup to choose' "$temp/usage.log"
mkdir -p "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d"
printf 'old prototype\n' > "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/99-eqweb.conf"
./eqweb setup --output test.output > "$temp/setup.log"
rg -q 'old prototype' "$XDG_CONFIG_HOME"/pipewire/pipewire.conf.d/*.backup.*
rg -q 'eqweb-native-v1' "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/99-eqweb.conf"
echo 'HTTP security, invalid input, missing output, process lock and setup backup tests passed.'
