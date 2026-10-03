# Native EQ implementation specification

This document describes the active C editor as reviewed on 2026-09-30. It records behavior and implementation contracts that refactors must preserve. [README.md](README.md) covers operation, [VALIDATION.md](VALIDATION.md) records checks and measurements, and [TODO.md](TODO.md) tracks known gaps and proposed changes. The Go and Python implementations under `old/` are archived prototypes.

## Components and execution

| Source | Responsibility |
| --- | --- |
| `main.c` | CLI and setup, process lock, PipeWire discovery and control, routing, HTTP validation, application state and persistence scheduling |
| `config.c`, `eq.h` | EQ values and validation, Bluetooth identity, filter graph generation, settings and atomic file writes |
| `library.c`, `library.h` | Presets, quickbar, song recall, general device EQ, saved master settings and song logging |
| `desktop.c`, `desktop.h` | Worker owning libpulse and MPRIS access, command queue, mixer/player snapshots and master restoration |
| `json_util.h` | Borrowed JSON member access and deep-copy helper |
| `app.js`, `web.html` | Browser request state, controls and rendering; embedded at build time by Make and `xxd` |

PipeWire performs DSP independently of the editor. The editor's PipeWire main loop runs a 50 ms control timer; this is not an audio processing callback. Registry events track nodes, links and metadata. The HTTP polling thread accepts commands under `app.mutex`; it updates desired EQ and library state in memory, while the timer sends PipeWire changes and persists files. Library preference actions can resolve song recall immediately before a subsequent edit is accepted.

The desktop worker is the sole caller of PulseAudio and D-Bus operations. A separate mutex protects its command queue and published snapshot. Its queue holds 32 commands. Volume commands for the same token coalesce only back to the most recent queued output change; other commands retain order. Output selections carry an increasing epoch so an earlier snapshot cannot mark a newer output's master as ready. Mixer tokens include the audio connection generation and must be refreshed after reconnects.

Desktop refresh is scheduled 750 ms after the previous update, and the controller samples the desktop snapshot every 500 ms. The browser polls every 750 ms. Blocking desktop calls can extend these intervals; these are scheduling intervals, not response-time guarantees. State/config writes and library/log flushes currently run synchronously inside the controller tick while `app.mutex` is held; extracting that I/O is a refactor candidate.

## EQ values and audio graph

The stereo graph contains a linear preamp followed by ten peaking bands with fixed Q 1.4 and frequencies 31, 63, 125, 250, 500, 1000, 2000, 4000, 8000 and 16000 Hz. Band gains are finite numbers from −12 to +12 dB; preamp is −24 to +12 dB. The graph has no limiter. Master volume is a separate EQ-sink mixer control.

An EQ object has `gains` (exactly ten numbers), `preamp`, optional boolean `maintain`, and optional numeric `loudness_target` from −36 to +24 dB. `parse_eq()` starts from the caller's existing settings, preserves `output`, preserves `maintain` when omitted, and computes an omitted target as mean gain plus preamp. It commits to the caller only after validation succeeds. When `maintain` is true, preamp becomes `clamp(target − mean, −24, 12)`.

The browser's gain total, mean and amplitude multiplier are arithmetic estimates, not measured filter response or perceptual loudness. Loading a preset, autoload item or quickbar snapshot preserves current mean-plus-preamp when `maintain_load` is enabled, subject to preamp limits. This load behavior is implemented in the browser; API clients apply their chosen EQ through `PUT /api/eq`. Song and general-device recall use stored EQ values directly.

`eq_same()` compares only gains and preamp with a 0.05 dB tolerance for detecting changes before auto-preset creation. PipeWire readback uses a separate 0.002 dB tolerance. Neither comparison checks output identity or loudness preferences. Preference-only changes must not be inferred from these audio comparisons.

## Outputs and routing

The graph uses `effect_input.eqweb` as its virtual sink and `effect_output.eqweb` as its playback stream. The installed fragment starts with `# eqweb-native-v1`. Its playback target is explicit, with no fallback and with linger enabled. Ordinary EQ, output and default-sink changes do not restart audio or recreate the graph.

Physical output discovery accepts `Audio/Sink` nodes with `device.id` or `device.api`, excludes `node.virtual=true`, and requires a valid output name. Names must be nonempty, shorter than 512 bytes, contain only alphanumeric characters or `._:-`, and cannot be either EQ node name. Explicit CLI setup validates the name syntactically; it does not require that hardware be present. Interactive setup discovers available outputs, sorts them by description/name and allows cancellation before writing settings or configuration.

Editing requires the native graph, complete parameter discovery, an available physical output and at least two distinct usable playback output ports linked to it. Links must be paused or active, with no wrong-output link or link error. Recording/meter taps are ignored in this route check. Selecting another output remains possible when the old output is absent if the graph and WirePlumber metadata are available.

When EQ is the default and editing is available, the controller checks application playback streams once after they have links. Streams linked only to the selected physical device are redirected through EQ. Other-device streams, virtual streams and internal filter streams are excluded. A transition to EQ as default resets this check; later manual stream moves are respected.

If a selected Bluetooth endpoint disappears, recovery selects a replacement only when exactly one available output matches its Bluetooth address. It does not match descriptions. Persistent device keys normalize recognized Bluetooth names to `bluez_output.aa:bb:cc:dd:ee:ff`, ignoring endpoint suffixes and normalizing separators/case. The actual selected output name remains in settings and routing metadata.

## Recall and master settings

The library stores general EQ separately from song overrides. A new output inherits the previous output's general EQ; an existing output restores its own general EQ before song recall. Startup seeds missing general EQ from current settings and preserves existing entries.

Song identity is the exact artist/title pair on the selected output. Automatic player selection prefers a playing player; explicit selection does not fall back to another player. Paused tracks can retain song identity. A stopped or missing player, or a missing title, clears it and restores general EQ when recall is enabled. Recall and song-transition logging advance only when EQ editing is available.

With recall enabled, lookup order is:

1. This output's song override, including compatible older Bluetooth keys.
2. A legacy artist/title-only override, if present.
3. With cross-device fallback enabled, the preferred saved device's override.
4. With fallback enabled, the first other saved device with that song, sorted by canonical device name.
5. This output's general EQ.

Borrowed cross-device EQ does not create a local override until an edit. Legacy song entries are copied to the current output key on recall. New explicit song edits are limited to 1,000 entries; the legacy-copy path currently bypasses this limit, tracked in [TODO.md](TODO.md). Existing library loads also do not enforce preset/song entry counts.

Edits with recall enabled and a current song update only that output/song entry. Other edits update general EQ. `device_eq` explicitly saves general EQ regardless of song selection. Turning recall off preserves song entries and restores general EQ. Recall preference changes clear song identity so the new selection is resolved before later edits.

If a recalled song's gains or preamp differ from its baseline by more than 0.05 dB when another song starts, an `(Auto)` preset is saved. Recall must be enabled for that baseline to exist. A new auto preset is skipped when the preset limit is reached; existing names can be replaced. Song logging appends the EQ at each observed song transition, including transitions caused by output changes or recall-preference resets. It does not log every subsequent gain edit, and repeated songs can produce repeated entries.

Master volume and mute belong to canonical output keys and are independent of song/preset/general-EQ loads. A new device inherits the current EQ master. The desktop worker restores saved values on output selection, startup and audio reconnect, applying attenuation before unmuting. It observes external mixer changes while running. `master_ready` requires readback and a matching selected output and epoch. Mixer commands accept 0–150 percent; stored external master values can exceed the UI range up to PulseAudio's supported maximum.

## Persistent data and recovery

Paths follow absolute `XDG_STATE_HOME` and `XDG_CONFIG_HOME`, with the home-directory defaults in the README. `HOME` must also be absolute. Setup and the server share a nonblocking process lock alongside `settings-v1.json`.

| File | Contents and ownership |
| --- | --- |
| `settings-v1.json` | Version 1, current EQ fields and actual selected `output`; authoritative for regenerating the installed native fragment |
| `99-eqweb.conf` | Generated native graph; ordinary persistence requires its native marker; explicit setup backs up and replaces an existing fragment |
| `library-v1.json` | Version 1, `presets`, `songs`, `quickbar`, `output_eq`, `output_volumes`, and global preferences |
| `outputlog.txt` | Append-only JSON lines containing EQ fields, artist, title, actual output name and UTC time |

Library EQ entries omit settings' `version` and `output`. `presets` maps names to EQ objects; user-created names are 1–128 bytes with up to 128 entries. `quickbar` is an array of at most 26 EQ snapshots. `songs` maps JSON-encoded `[canonical_output, artist, title]` strings to EQ objects, with legacy `[artist, title]` support. `output_eq` maps canonical output keys to general EQ. `output_volumes` maps those keys to `{volume, mute}`.

Global preferences default to `remember=true`, `maintain_load=true`, `output_log=true`, `fallback=false`, `fallback_device=""` and `player=""` (automatic). These preferences are not per output. Missing supported optional library fields are initialized on load and saved later. Invalid settings or library values cause startup failure; archived prototype state is not imported.

Settings and config saves debounce for 350 ms, write settings first, then the marked fragment, and retry failures after about two seconds. Each replacement writes and syncs a temporary file, renames it, then syncs the parent directory. These files and the library are separate writes, not one transaction. An error after rename may occur after the replacement is visible. Startup regenerates an already-installed native fragment from saved settings.

Library changes use their own 350 ms debounce and two-second failure retry. Song-log failures are reported independently; up to 1,000 failed/pending entries remain in memory for retry. Partial appends are truncated to their starting offset before retry; a rollback failure is reported. Entries that cannot enter a full backlog are dropped with an error. The log queue is not persisted across a crash.

SIGINT/SIGTERM stop accepting mutations, flush accepted EQ/persistence work and drain accepted desktop commands before the final master/library save. The PipeWire shutdown sync has a 500 ms deadline; live confirmation cannot be guaranteed after audio disconnects. Unsaved settings, library or log failures produce a nonzero exit. Abrupt termination can lose debounced edits and queued log entries.

## HTTP and browser contracts

The server binds IPv4 loopback, default port 8932; `EQWEB_PORT` accepts 1024–65535. Host must exactly match `127.0.0.1:PORT`. Mutation Origin, if supplied, must match `http://127.0.0.1:PORT`; Fetch Metadata accepts `same-origin` or `none`. There is no CORS grant or authentication against local processes. Responses use `no-store`, `nosniff` and a restrictive content security policy.

Mutations accept a strict UTF-8 JSON object up to 4,096 bytes and require `application/json` or `application/json; charset=utf-8`. The listener limits connections to 16, per-connection memory to 16 KiB and connection timeout to five seconds. The endpoint/action table is maintained in the README.

Accepted mutations return HTTP 202 with `{accepted:true, revision:N}`. This confirms acceptance, not completed application or durability. Validation errors return `{error:"…"}` with 400; unavailable/conflicting EQ/output/default operations use 409; origin/host rejection uses 403, unknown routes 404, wrong methods 405, oversized bodies 413 and unsupported content types 415. Shutdown and a busy output-selection queue use 503. A busy mixer/player queue currently returns 400 through the action-validation path.

`PUT /api/eq` rejects unknown fields and optionally checks the expected `output`; `PUT /api/output` requires only `name`; `POST /api/default` requires `{}`. Mixer commands can also check expected `output`. A mismatch on EQ or mixer requests returns 409. The library `device_eq` action requires `output` and `eq`, but its mismatch currently returns 400. Preset loading is a browser EQ mutation, not a library `load` action.

| State field | Meaning |
| --- | --- |
| `instance` | Process identity; revisions restart with each editor process |
| `settings` | Requested EQ and output |
| `saved` | Last successful settings-file contents, even if the config write failed |
| `revision` | Current controller revision; EQ/output changes and recall can advance it |
| `saved_revision` | Revision saved to both settings and config; excludes library/log persistence |
| `applied_revision` | Last sent EQ revision confirmed by parameter values; excludes routing/default/master confirmation |
| `pending` | Controller work/deadlines; does not cover all library work or idle readback |
| `live.eq` | Last discovered audio gains/preamp; its other serialized settings fields are not recalled preferences or output identity |
| `live.awaiting_readback` | Requested controls still need parameter confirmation; can remain true while idle and otherwise saved |
| `library.pending`, `library.error`, `library.log_error` | Independent library/log work and failures |
| `desktop.action_error`, `desktop.master_error`, `desktop.master_ready` | Asynchronous desktop command/restoration state |

The browser debounces EQ sends by 70 ms and allows only one in flight. It retains newer edits during a request, retries network failures, 5xx responses and disconnected-EQ conflicts with backoff from 500 ms to eight seconds, and displays permanent rejections without repeated automatic retries. Polls must not overwrite unsent/in-flight EQ or invalid text. Output selection is disabled while EQ edits are pending. `pagehide` makes a best-effort keepalive send; it is not a durability guarantee.

## C interface ownership

`json_get()` and `json_string()` return borrowed values; `json_copy()` returns a caller-owned deep copy. `settings_json()` returns a caller-owned JSON object; `render_config()` returns a buffer to free. `parse_eq()` leaves its settings argument unchanged on invalid input. `load_settings()` returns 1 when loaded, 0 when absent and −1 on failure; `backup_config()` uses the same three-way success/absence/error convention. Save functions return 0 on success and −1 on failure.

`library_snapshot()` returns a caller-owned wrapper with retained references to nested library objects; it is not a deep independent snapshot. Keep access serialized with library mutations and treat its children as read-only. `library_follow()` can mutate both settings and the supplied desktop JSON (`selected_player`). A true return asks the controller to apply/save the recalled EQ; it is not a general JSON-change detector.

`desktop_start()` copies its initial volume map. `desktop_action()` validates and copies/queues an action; NULL means accepted and a non-NULL string describes rejection. `desktop_select_output()` returns an epoch only on acceptance. `desktop_snapshot()` returns a caller-owned deep copy. `desktop_stop()` drains the worker and transfers ownership of the final volume map. The module currently assumes a single start/stop lifecycle per process. Header-level documentation for these contracts remains a TODO.

## Verification boundaries

`make test` runs config, library and isolated HTTP/setup tests. `make integration` adds isolated PipeWire/PulseAudio, WirePlumber, MPRIS, browser and synthetic audio checks. Browser execution currently requires a `chromium` command on `PATH` even when `CHROMIUM_BINARY` is set; see TODO. Existing assertions and audio tolerances should be preserved during refactors, with focused regression cases for changed state transitions.

Physical hardware latency, real Bluetooth/ALSA hotplug and arbitrary third-party player behavior remain outside the recorded validation. Drawing, variable band counts, other filter types, profiles, ports, per-channel editing and meters are outside the native editor's current feature set.
