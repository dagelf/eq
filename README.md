# Minimal PipeWire web EQ

A native C editor at **http://127.0.0.1:8932**. PipeWire owns the stereo filter chain, so audio processing and the last applied controls continue after the editor exits. HTML and JavaScript are embedded in the executable. No Node, Python, frontend framework, or separate application runtime is used.

## Build and run

Requires Linux, PipeWire with its builtin filter-chain plugin, WirePlumber 0.5, a C compiler, Make, pkg-config, xxd, and development packages for `libpipewire-0.3`, `libmicrohttpd`, and `json-c`.

```sh
make
./eqweb
```

Open **http://127.0.0.1:8932** using that exact address. The physical-output list is available before setup. Stop the editor before running setup.

## One-time setup

Stop the editor, then run:

```sh
./eqweb setup
```

Setup lists your audio devices by name. Enter the number of the speakers, headphones or audio interface you want the EQ to use. Press Enter to cancel.

For scripts, `./eqweb outputs` lists the devices with a copyable `./eqweb setup --output NAME` command for each one. The output name is PipeWire's device identifier; you do not need to find or type it when using the numbered chooser.

Setup makes a uniquely named backup of the existing `99-eqweb.conf`, prints its path, and replaces the fragment with the native graph. It starts flat when no native state exists; running setup again retains native gains and preamp. Archived Go/Python state and prototypes are preserved and are not imported.

Load the new graph with one audio restart:

```sh
command systemctl --user restart pipewire pipewire-pulse wireplumber
./eqweb
```

Run this as your normal user. The `command` prefix bypasses shell aliases that might add `sudo`; user audio services belong to your desktop session. The restart interrupts playback. On systems without those user services, restart audio using the distribution's service manager or log out and back in. **Ordinary gain, preamp, output and default-sink changes need no restart.** Setup itself neither restarts audio nor changes the default sink.

Use the output selector, then **Use EQ as default**. Move any existing application streams with your system mixer if needed; the editor does not move them itself. WirePlumber's own stream-migration policy still applies.

For a preview without writes:

```sh
./eqweb print-config --output alsa_output.YOUR_DEVICE_NAME
```

## Controls and status

- Ten peaking bands: 31, 63, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz; fixed Q 1.4; −12 to +12 dB.
- Preamp: −24 to 0 dB, before the bands. Reset sets all gains and preamp to zero. There is no limiter; use preamp attenuation to leave headroom when boosting.
- Native sliders support Tab, arrow keys, Home and End. Every gain also has a labelled numeric input, in 0.1 dB steps.
- Editing requires the native sink, complete parameter discovery, an available selected physical output, and two usable output links. Output selection remains available when the old output disappears.
- PipeWire reconnection is automatic. The page distinguishes disconnected audio, absent outputs, persistence errors and parameter-apply errors.
- On PipeWire 1.6.8, idle filter controls can be accepted before parameter readback reflects them. The page reports “waiting for playback to confirm live values” until playback activates the graph. It does not claim an idle change has been verified live.

The playback stream uses a stable `target.object`, `node.dont-fallback=true` and `node.linger=true`. Missing hardware leaves it waiting instead of falling back to the EQ. Output changes use WirePlumber's `default` metadata, and actual PipeWire links determine connection status. Outputs must be hardware-backed `Audio/Sink` nodes with a device ID/API and without `node.virtual=true`; virtual sinks are intentionally excluded. Stereo output is required.

## Storage and API

Files follow `XDG_STATE_HOME` and `XDG_CONFIG_HOME`, defaulting to:

```text
~/.local/state/eqweb/settings-v1.json
~/.config/pipewire/pipewire.conf.d/99-eqweb.conf
```

The control loop coalesces live updates at 50 ms intervals. Settled changes save after 350 ms. Each file is written to a temporary file, synced, renamed, and its directory synced. The state file is authoritative and is written first; the two files are not a single filesystem transaction. A failed second write is reported separately and retried. Startup regenerates an already-installed native fragment from saved state, recovering from an interrupted save. An unmarked prototype or externally replaced fragment is never silently overwritten during ordinary use. Invalid state prevents startup instead of silently resetting it.

SIGINT/SIGTERM flush accepted changes and pending saves. Abrupt termination can lose changes still within the debounce window; wait for the saved indicator before stopping the process. A lock prevents simultaneous editor/setup writers. Runtime control is entirely in the PipeWire main loop; the HTTP thread only validates requests, queues desired settings and reads a published status snapshot.

| Endpoint | Body / result |
| --- | --- |
| `GET /api/state` | `settings` (requested), `saved` (state file), `live`, physical `outputs`, revisions, pending and error fields |
| `PUT /api/eq` | `{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}` |
| `PUT /api/output` | `{"name":"alsa_output.YOUR_DEVICE_NAME"}` |
| `POST /api/default` | `{}` |

Mutations require `Content-Type: application/json`. A `202` means queued, **not** saved or confirmed live: poll `/api/state` for completion. `saved_revision` confirms both files; `applied_revision` is the last EQ revision confirmed by readback. Output/default success is determined from `live.connected`, selected output and `live.default`. `live.awaiting_readback` exposes an unconfirmed control update.

The listener binds IPv4 loopback only. Host validation, Origin and Fetch Metadata checks reject cross-origin mutations and DNS rebinding. There is no CORS grant. Bodies are limited to 4 KiB, with connection/memory/time limits and a restrictive content security policy. Command-line clients may omit Origin. This is a local desktop tool, with no authentication against other local processes; do not proxy it onto a network.

## Verification

```sh
make test
make integration
```

`make test` covers validation, generation, atomic saves, setup backups, missing outputs, HTTP request limits and origin/host checks. `make integration` creates a separate PipeWire socket, D-Bus session, WirePlumber configuration and synthetic outputs under `/tmp`; hardware monitors are disabled. Port 8932 must be free. It requires PipeWire CLI tools, curl, jq and rg. Chromium enables native DevTools-driven browser checks; `CHROMIUM_BINARY` can override its executable. No browser automation language runtime is needed.

Integration tests cover stereo Fourier measurements, live parameter readback, rapid updates without sink recreation, editor closure, daemon restart, output loss, output/default selection, persistence failure recovery, keyboard interaction, a sample-delay measurement and CPU/RSS observations. Test artifacts remain in the printed `/tmp/eqweb-integration-*` directory for inspection. See [VALIDATION.md](VALIDATION.md) for measured results and limits.

This implementation uses the documented [PipeWire filter-chain graph and controls](https://docs.pipewire.org/page_module_filter_chain.html) and [WirePlumber linking policy](https://pipewire.pages.freedesktop.org/wireplumber/policies/linking.html). Drawing, variable band counts, imports, presets and application stream migration are deferred.
