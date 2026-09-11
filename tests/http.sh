#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
temp=$(mktemp -d /tmp/eqweb-http-XXXXXX)
pid=
cleanup() { if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; rm -rf "$temp"; }
trap cleanup EXIT
export XDG_STATE_HOME="$temp/state" XDG_CONFIG_HOME="$temp/config" PIPEWIRE_RUNTIME_DIR="$temp"
./eqweb > "$temp/log" 2>&1 & pid=$!
for ((i=0;i<100;i++)); do if curl -fsS http://127.0.0.1:8932/api/state > "$temp/state.json" 2>/dev/null; then break; fi; sleep .05; done
jq -e '.setup_required and (.live.connected | not)' "$temp/state.json" >/dev/null
check() {
  local expected=$1; shift
  local actual
  actual=$(curl -sS -o "$temp/body" -w '%{http_code}' "$@")
  if [[ "$actual" != "$expected" ]]; then cat "$temp/body"; echo "Expected $expected, got $actual"; exit 1; fi
}
check 200 http://127.0.0.1:8932/
check 200 http://127.0.0.1:8932/app.js
check 403 -H 'Host: attacker.example' http://127.0.0.1:8932/api/state
check 403 -X PUT -H 'Content-Type: application/json' -H 'Origin: https://evil.example' -d '{}' http://127.0.0.1:8932/api/eq
check 403 -X PUT -H 'Content-Type: application/json' -H 'Sec-Fetch-Site: cross-site' -d '{}' http://127.0.0.1:8932/api/eq
check 415 -X PUT -d '{}' http://127.0.0.1:8932/api/eq
check 400 -X PUT -H 'Content-Type: application/json' -d '{"gains":[0],"preamp":0}' http://127.0.0.1:8932/api/eq
check 400 -X PUT -H 'Content-Type: application/json' -d '{} {}' http://127.0.0.1:8932/api/eq
check 409 -X PUT -H 'Content-Type: application/json' -d '{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}' http://127.0.0.1:8932/api/eq
check 409 -X PUT -H 'Content-Type: application/json' -d '{"name":"alsa_output.missing"}' http://127.0.0.1:8932/api/output
check 400 -X PUT -H 'Content-Type: application/json' -d '{"name":"effect_input.eqweb"}' http://127.0.0.1:8932/api/output
check 409 -X POST -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:8932/api/default
head -c 4097 /dev/zero > "$temp/large"
check 413 -X PUT -H 'Content-Type: application/json' --data-binary "@$temp/large" http://127.0.0.1:8932/api/eq
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
