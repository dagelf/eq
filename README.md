# Minimal PipeWire web EQ

A native C editor at **http://127.0.0.1:8932**. PipeWire owns the stereo filter chain, so audio processing and the last applied controls continue after the editor exits. HTML and JavaScript are embedded in the executable. No Node, Python, frontend framework, or separate application runtime is used.

Developer references: [implementation specification](SPEC.md), [review and TODOs](TODO.md), and [validation results](VALIDATION.md). The Go/Python projects under `old/` are archived prototypes; their features and setup instructions do not describe this implementation.

## Build and run

Requires Linux, PipeWire with its builtin filter-chain plugin, WirePlumber 0.5, a C compiler, Make, pkg-config, xxd, and development packages for `libpipewire-0.3`, `libmicrohttpd`, `json-c`, `libpulse`, and `gio-2.0`. Mixer controls use `pipewire-pulse` (or PulseAudio); player controls use the desktop session bus.

```sh
make
./eqweb
```

Open **http://127.0.0.1:8932** using that exact address. The physical-output list is available before setup. Stop the editor before running setup.

After rebuilding an existing installation, restart only `eqweb` and refresh the page. No additional audio restart or setup is needed for the new editor controls. `EQWEB_PORT=18932 ./eqweb` selects another loopback port if needed.

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

Use the output selector, then **Use EQ as default**. While the editor is running and EQ is the default, it routes existing and new application streams from the selected device through **EQ Web Sink** automatically. Each stream is checked once when it connects or EQ becomes the default; later manual moves in the app's Playback mixer or pavucontrol are respected. Streams using another device and internal filter streams are left alone. Applications routed directly to a device bypass both the EQ and its master volume.

Every output you select moves to the top of a saved priority list (most recently selected first). If it disappears, the running editor switches to the next available output you previously selected, restoring that device’s EQ and master settings. Reconnecting a higher-priority device switches back to it automatically, including Bluetooth. Automatic switches preserve the order; selecting an older device again promotes it. **Prefer this output** promotes the currently active fallback without switching devices. The order survives editor and audio-server restarts and is available as `library.output_priority` in `/api/state`.

Bluetooth endpoint name changes are matched using the Bluetooth address, with exactly one matching available output required when the saved endpoint name is absent. Ambiguous matches are skipped in favor of the next available preferred device. If no preferred outputs are available, EQ waits until one reconnects or you choose another output. Output switching requires no audio restart. This automatic routing runs while the editor is open; closing the browser does not stop it. The master control reports when no application streams are routed through the EQ.

For a preview without writes:

```sh
./eqweb print-config --output alsa_output.YOUR_DEVICE_NAME
```

## Controls and status

- Ten peaking bands: 31, 63, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz; fixed Q 1.4; −12 to +12 dB.
- Taller vertical bands run bass to treble from left to right; narrow screens scroll the band bank horizontally.
- Preamp: −24 to +12 dB, before the bands. Reset sets all gains and preamp to zero. There is no limiter; use preamp attenuation to leave headroom when boosting.
- The gain display shows the sum of all ten band gains, their arithmetic mean in dB, the mean's amplitude multiplier, and the mean plus preamp. This is an equal-weight estimate, not a measurement of the filter response or perceptual loudness.
- **Maintain loudness** captures the current mean-plus-preamp as its target. Band edits automatically set preamp to `target − mean`, within −24 to +12 dB. Adjusting preamp changes the target; reaching a limit is shown in the page. The checkbox and target persist with EQ settings.
- **Maintain loudness when loading** (on by default) preserves the **With preamp** value when loading presets, quickbar slots, autoload entries, saved song EQ or the current output’s general-EQ fallback. Preamp becomes `previous target − loaded mean gain`: a 0 dB target stays at 0 dB across loads. Loads retain your current **Maintain loudness** checkbox; when enabled, its target also survives preamp limits. Preamp remains limited to −24 to +12 dB, so some targets cannot be reached. Disable load compensation to restore saved preamp and loudness settings exactly. Selecting a different physical output still restores that device’s own settings.
- **Master** changes the EQ sink's volume and mute independently of the preamp. Each physical output remembers its band gains, preamp, loudness settings, master volume and mute. Master volume and mute are restored on output changes, editor startup and audio-server reconnect; song, preset and general-EQ loads never adjust them. A new output inherits the previous device’s general EQ and current master settings. Bluetooth endpoint renames retain the same device settings. Presets and song EQ contain EQ values, while master volume and mute remain per output. Changes made to the EQ master in another mixer are also remembered while the editor is running.
- Native sliders support Tab, arrow keys, Home and End. Every gain also has a labelled numeric input: bands use 0.1 dB steps and preamp uses 0.01 dB steps.
- Editing requires the native sink, complete parameter discovery, an available selected physical output, and two usable output links. Output selection remains available when the old output disappears.
- PipeWire reconnection is automatic. The page distinguishes disconnected audio, absent outputs, persistence errors and parameter-apply errors.
- On PipeWire 1.6.8, idle filter controls can be accepted before parameter readback reflects them. The page reports “waiting for playback to confirm live values” until playback activates the graph. It does not claim an idle change has been verified live.

## Presets, text and song memory

Enter a name and select **Save preset** to save the current EQ; using the same name replaces it. Select a saved preset and **Load** to apply it, or **Delete** to remove it. Up to 128 named presets are retained. **Quickbar** pins the selected preset (or the current EQ if none is selected) to the next free letter A–Z beside **Reset to flat**; the letter loads that snapshot. **Clear** removes the letter buttons.

**Autoload** is a visible preset list. Rolling the mouse wheel over it, or clicking a name, loads that EQ immediately for quick A/B comparison.

The values textarea accepts the displayed JSONL object, a JSON array, or comma/space-separated values: ten band gains and an optional eleventh preamp value. Valid input applies live; incomplete or invalid input is retained with an error and does not affect audio. **Copy values** copies the current valid EQ as one JSON line. JSON can include `maintain` and `loudness_target`; without a target, pasted preamp and gains establish the target.

The player selector lists the same MPRIS players exposed by `playerctl`, with title, artist, album, playback state, progress, previous/next, play/pause and ±20 s seek controls. Clicking the progress bar seeks to that point; the bar and seek buttons are inactive when the selected player does not support seeking. Automatic selection prefers a playing player; selecting one explicitly keeps other players from controlling song recall. Unsupported controls are disabled.

**Remember EQ for each song** saves adjustments by the selected physical output plus the exact artist and song title, and restores them when that song returns on that output, including after an editor restart. A saved song EQ takes precedence over the output’s general EQ. An unfamiliar song restores the output’s general EQ instead of inheriting the previous song’s adjustments. New song-specific overrides are created by edits; older artist/title-only entries are also copied to the current output on recall. New edits are limited to 1,000 song entries across outputs, and the page reports when memory is full. Legacy recall currently can exceed this limit; the gap is tracked in [TODO.md](TODO.md). Turning recall off preserves saved song settings and restores general device EQ.

With recall enabled, any gain or preamp difference from the song’s recalled baseline saves a preset named with **(Auto)** appended when that song ends (next track, stopped player, missing title or player). Pausing does not end a song. New auto presets are skipped when the 128-preset limit is full; an existing name can still be replaced. **Log songs and EQ to outputlog.txt** (on by default) appends one JSON line when each song ends to `~/.local/state/eqweb/outputlog.txt`, using that song’s final EQ before loading the next song or restoring device EQ. Starting a song does not write a log entry. Replaying a song can produce another entry when it ends. Recall and logging require `eqweb` to be running and the EQ to be connected for editing; closing only the browser does not stop them.

**Ignore custom presets** automatically turns off **Remember EQ for each song**, uses device EQ, and disables song EQ saving, recall, automatic song presets and song logging. Existing saved entries are retained. Turning song memory back on clears Ignore custom presets; turning Ignore custom presets off leaves song memory off until enabled.

**Use best device** adds cross-device song recall when this output has no override. The **Most similar device** dropdown lists all saved devices, including disconnected ones. Recall tries that device’s EQ for the exact artist/title, then the first saved device (sorted by name) that has that song. If neither has it, or the checkbox is off, the output’s general EQ is used. Borrowed EQ is not copied into an output-specific song entry unless you edit it. Existing output-specific song EQ always wins.

Legacy artist/title-only song entries take precedence over cross-device fallback. Recall, fallback-device, player-selection, load-loudness and logging preferences apply globally; EQ and master settings remain per output.

The **general device EQ** is stored separately in `library-v1.json` under `output_eq`. It is seeded from the current EQ when a device is first recorded. Edits with song recall off, or with no song selected, update it automatically. While a song is selected and recall is on, edits affect only that output/song pair. **Save as device EQ** explicitly makes the current bands, preamp and loudness settings the general fallback; expand **Saved general EQ for this output** to inspect it. Automatic song recall never overwrites the general EQ. Existing libraries without general settings are initialized from the current EQ; older historical general settings cannot be reconstructed.

## Audio mixer

Playback streams, recording streams, output devices and input devices each have volume and mute controls. Streams can be routed to another output or input. The mixer includes virtual devices and monitor sources, and disables volume sliders for streams without writable volume. Volume ranges from 0 to 150 percent and preserves the existing channel balance. Device profiles, ports, per-channel balance editing and peak meters remain in pavucontrol.

Mixer and player availability are independent of EQ connectivity, and failures appear beside their controls. Native libpulse and D-Bus calls run in a separate worker; media-player commands do not invoke a shell or require the `playerctl` executable.

The playback stream uses a stable `target.object`, `node.dont-fallback=true` and `node.linger=true`. Missing hardware leaves it waiting instead of falling back to the EQ. Output changes use WirePlumber's `default` metadata, and actual PipeWire links determine connection status. Outputs must be hardware-backed `Audio/Sink` nodes with a device ID/API and without `node.virtual=true`; virtual sinks are intentionally excluded. Stereo output is required.

## Storage and API

Files follow `XDG_STATE_HOME` and `XDG_CONFIG_HOME`, defaulting to:

```text
~/.local/state/eqweb/settings-v1.json
~/.local/state/eqweb/library-v1.json
~/.local/state/eqweb/outputlog.txt
~/.config/pipewire/pipewire.conf.d/99-eqweb.conf
```

The control loop coalesces live updates at 50 ms intervals. Settled changes save after 350 ms. Each file is written to a temporary file, synced, renamed, and its directory synced. The state file is authoritative and is written first; the two files are not a single filesystem transaction. A failed second write is reported separately and retried. Startup regenerates an already-installed native fragment from saved state, recovering from an interrupted save. An unmarked prototype or externally replaced fragment is never silently overwritten during ordinary use. Invalid state prevents startup instead of silently resetting it.

SIGINT/SIGTERM flush accepted EQ changes and pending saves. Abrupt termination can lose changes still within the debounce window; wait for the saved indicator before stopping the process. A lock prevents simultaneous editor/setup writers. EQ control runs in the PipeWire main loop; a separate desktop worker handles mixer/player requests. The HTTP thread validates and queues requests. Presets, song memory and per-output EQ/volume use a separate atomically written library file. Writes wait until edits have settled for 350 ms; failures are reported and retried. Song-log errors are reported independently. Failed log entries are retained in memory and retried while the editor runs (up to 1,000 pending entries); an unsuccessful shutdown reports any remaining failure. Older settings files are supported without migration.

| Endpoint | Body / result |
| --- | --- |
| `GET /api/state` | `settings` (requested), `saved` (state file), `live`, physical `outputs`, revisions, pending and error fields |
| `PUT /api/eq` | `{"gains":[0,0,0,0,0,0,0,0,0,0],"preamp":0}` |
| `PUT /api/output` | `{"name":"alsa_output.YOUR_DEVICE_NAME"}` |
| `POST /api/default` | `{}` |
| `POST /api/library` | `{"action":"save","name":"Bass","eq":{"gains":[6,0,0,0,0,0,0,0,0,0],"preamp":-3}}`; actions also include `delete`, `remember`/`maintain_load`/`output_log` with `enabled`, `player` with a player ID or `""` for automatic, `quickbar` with `eq`, and `quickbar_clear`; `fallback` takes `enabled`, `fallback_device` takes `device`, and `device_eq` takes `output` and `eq` |
| `POST /api/mixer` | `{"token":"…","action":"volume","value":75}`; `mute` takes a boolean, and `move` takes a destination `target` token |
| `POST /api/player` | `{"player":"org.mpris.MediaPlayer2.…","action":"next"}`; also `previous`, `playpause`, `seek` with `position` in seconds, and `seek_relative` with `offset` in seconds |

`GET /api/state` includes `library` and `desktop` snapshots with mixer tokens, player IDs, quickbar slots and load/log preferences. `PUT /api/eq` optionally accepts `maintain` (boolean), `loudness_target` (−36 to +24 dB), and `output` (the expected physical output name). A mismatched output returns 409, preventing delayed edits from affecting a newly selected device. Mixer commands may also include `output` for this check. The browser retains unsent EQ edits and retries temporary failures with backoff; invalid requests remain visible without automatic retries. `desktop.master_ready` indicates that the selected output’s volume/mute has been restored; `desktop.master_error` reports restoration failures. Mixer/player commands are queued and failures appear in `desktop.action_error`; poll for actual values. Mixer tokens are valid only for the current audio-server connection.

Mutations require `Content-Type: application/json`. A `202` means accepted, **not** saved or confirmed live: poll `/api/state` for completion. Revisions are scoped to the process identified by `instance`. `saved_revision` confirms the settings file and PipeWire fragment, while library/log saves have independent `library.pending`, `library.error` and `library.log_error` fields. `applied_revision` is the last EQ revision confirmed by readback. Output/default success is determined from `live.connected`, selected output and `live.default`. `live.awaiting_readback` exposes an unconfirmed control update and can remain true while the graph is idle even when `pending` is false. Presets are loaded through `PUT /api/eq`; there is no library `load` action. See [SPEC.md](SPEC.md) for state and ownership contracts.

The listener binds IPv4 loopback only. Host validation, Origin and Fetch Metadata checks reject cross-origin mutations and DNS rebinding. There is no CORS grant. Bodies are limited to 4 KiB, with connection/memory/time limits and a restrictive content security policy. Command-line clients may omit Origin. This is a local desktop tool, with no authentication against other local processes; do not proxy it onto a network.

## Verification

```sh
make test
make integration
```

`make test` covers validation, compensation, preset/song persistence, per-output song memory, general EQ persistence, debounced saves, song-log failure recovery, quickbar slots, auto-named presets, song logging, setup backups, missing outputs, HTTP request limits and origin/host checks. It requires Bash, curl, jq and rg in addition to the build dependencies. `make integration` creates separate PipeWire/PulseAudio sockets, a D-Bus session, a test MPRIS player, WirePlumber configuration and synthetic outputs under `/tmp`; hardware monitors are disabled. Tests default to ports 18932 and 18933 respectively; `EQWEB_PORT` overrides either, so choose an unused port. Integration additionally requires PipeWire CLI tools (including `pw-cat`, `pw-cli`, `pw-dump`, `pw-link` and `pw-top`), `pipewire-pulse`, WirePlumber, `pacat`, `parec`, `pactl` and `dbus-run-session`.

Chromium enables native DevTools-driven browser checks. The integration script currently checks for `chromium` on `PATH` before invoking the driver, whose executable can be overridden with `CHROMIUM_BINARY`. Setting only the override does not enable the checks if `chromium` is absent; this launcher gap is tracked in [TODO.md](TODO.md). No browser automation language runtime is needed.

Integration tests cover per-output EQ and master volume/mute recall, rapid output switching, external mixer changes, editor restart, browser request retries and incomplete text input, stereo Fourier measurements, live parameter readback, rapid updates without sink recreation, editor closure, daemon restart, output loss, output/default selection, persistence failure recovery, keyboard interaction, a sample-delay measurement and CPU/RSS observations. Test artifacts remain in the printed `/tmp/eqweb-integration-*` directory for inspection. See [VALIDATION.md](VALIDATION.md) for measured results and limits.

This implementation uses the documented [PipeWire filter-chain graph and controls](https://docs.pipewire.org/page_module_filter_chain.html), [WirePlumber linking policy](https://pipewire.pages.freedesktop.org/wireplumber/policies/linking.html), [PulseAudio volume API](https://freedesktop.org/software/pulseaudio/doxygen/volume_8h.html), and [MPRIS player interface](https://specifications.freedesktop.org/mpris/latest/Player_Interface.html). Drawing and variable band counts are deferred.
