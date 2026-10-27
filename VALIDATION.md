# Validation results

## Load loudness preservation on 2026-10-21

`make test` and full `make integration` passed. Artifacts: `/tmp/eqweb-integration-DLo5BI`. Library regressions verify 0 dB and nonzero targets across song recall and general-EQ fallback, preservation of the continuous-maintain checkbox and requested target through preamp limits, exact saved-EQ restoration when disabled, and unchanged saved song values. Chromium verifies a 0 dB With preamp value through preset loading and two automatic song transitions. Existing exact-preamp recall tests now explicitly disable load compensation. Stereo flat/boosted measurements, editor shutdown/restart, device priority, routing and Bluetooth simulations passed. The earlier capture failures recorded below did not recur in this run.

## Song-end saving and ignore toggle on 2026-10-21

`make test` passed. New library regressions cover stopped playback, missing player/title, next-song transitions, no writes on song start, final-EQ logging, pause retention, small preamp edits, repeated ended snapshots, restart persistence, and ignore/remember interaction. HTTP checks reject invalid ignore preferences. Chromium passed the new checkbox interaction checks in `/tmp/eqweb-integration-Tp1bcW` and `/tmp/eqweb-integration-ksccJd`; an existing device-EQ display assertion now waits for the page refresh.

Full integration did not pass: one run measured the flat capture at −3.187 dB; the repeat measured flat correctly and latency at 256 frames, then measured the boosted capture at −0.192 dB instead of +3 dB. The amplitude tolerances were retained. Functional audio, device, song recall and browser checks passed before these failures; later integration checks were not reached. These capture failures remain unresolved.

## Output priority update on 2026-10-03

`make test` and full `make integration` passed with the final rebuilt editor, including Chromium browser checks. Artifacts: `/tmp/eqweb-integration-qq3wrn`.

New library checks cover selection-order promotion, Bluetooth address deduplication and priority persistence. Isolated PipeWire tests cover cascading fallback through previous selections, waiting when all preferred outputs are absent despite an unrelated available output, return of a preferred Bluetooth device under a changed endpoint name, skipping ambiguous Bluetooth matches, explicit promotion of an older choice, and editor restart while preferred devices are missing or connected. Existing device EQ/master restoration, audio-server restart, routing and stereo audio measurements also passed. Bluetooth/hotplug behavior was simulated with synthetic sinks; real hardware was not disconnected.

## Documentation review on 2026-09-30

The active C/JavaScript implementation, headers, build rules and test coverage were reviewed against the documentation. `make test` passed during this review: config, library and isolated HTTP/setup checks. Runtime sources were unchanged, and integration, sanitizer, static-analysis and audio measurements were not rerun for this documentation pass; results below are historical runs.

A temporary C harness linked against the current `library.c` and `config.c` confirmed a limit bypass: a library with 1,000 legacy artist/title-only song entries had 1,001 entries after `library_follow()` recalled one and copied it to an output-specific key. This is a known gap, not a passing limit regression. Source review also found that the integration script skips browser checks without `chromium` on `PATH`, even when `CHROMIUM_BINARY` names an executable. Both are tracked in [TODO.md](TODO.md), alongside refactor and missing-code-documentation work. [SPEC.md](SPEC.md) records the current contracts.

## Recorded implementation validation

Update verified on 2026-09-30: `make test` and the full `make integration` suite passed. Final artifacts and the reviewed browser screenshot are in `/tmp/eqweb-integration-wh8fgb`. AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer passed the library regressions; `cppcheck` reported no warnings and the JavaScript syntax check passed.

New coverage includes independent per-output master volume/mute, external mixer changes, rapid output switches, editor restart, per-output general EQ, per-output/song overrides, preferred-device song fallback, a missing preferred device, and falling back to the saved general EQ. Tests verify that song recall does not alter master volume or overwrite general EQ, borrowed song EQ is not saved as an override until edited, and startup does not replace an existing general EQ. Chromium exercises the fallback checkbox/dropdown, explicit device-EQ saving and display, transient request retries, retention of newer unsent edits, permanent rejection without retry loops, and incomplete comma-separated values. Library tests cover debounced persistence and retained/retried song-log failures; HTTP tests cover the new action validation. A browser regression exposed a preference-change/edit race; recall preferences are now resolved before accepting subsequent edits.

The final audio measurements matched the values below, including 256 frames (5.333 ms) monitor-to-monitor latency. An earlier run encountered a discontinuity in the captured tone; the original Fourier thresholds were retained, and subsequent measurements passed. These synthetic audio tests run without guaranteed realtime scheduling.

Automation update verified on 2026-09-12: `make test` and `make integration` passed; artifacts are in `/tmp/eqweb-integration-Y7htqx`. New isolated tests cover existing and new playback moving through EQ when it becomes the default, preservation of another default/output and later manual stream moves, and exclusion of internal filter streams. Synthetic Bluetooth outputs reproduce underscore/colon and endpoint-name changes, recover the same device without recreating EQ, persist the updated name, and recover legacy saved names on editor startup. Unrelated devices and ambiguous matches are excluded. Bluetooth reconnection was simulated; these tests do not disconnect the user's headphones.

Verified on 2026-09-11 with PipeWire 1.6.8, WirePlumber 0.5.15, libmicrohttpd 1.0.6, json-c 0.19 and Chromium 150.0.7871.186. Host CPU: Intel Xeon W-2123, 3.60 GHz. Optimized build with `-O2`, warnings treated as errors.

The tests used separate PipeWire/PulseAudio sockets, a D-Bus session, XDG directories and HTTP ports, with hardware monitors disabled, two stereo null outputs, a virtual-output exclusion fixture, and a synthetic MPRIS player. The running desktop editor was left intact. Final artifacts: `/tmp/eqweb-integration-3xofX2`.

## Functional checks

`make test` and `make integration` passed. Coverage includes:

- Configuration structure, all ten bands, fixed frequencies/Q, stereo channels, explicit target and no-fallback properties.
- Finite numeric ranges, array length and type validation, malformed/trailing JSON, self-target rejection, missing outputs, oversized bodies, Host/Origin/Fetch Metadata checks, content type and methods.
- Atomic state/config writes, setup backup, corrupt state rejection and concurrent setup/editor exclusion.
- Actual stereo links and parameter readback; 24 rapid updates retained the final 11 dB value without changing the sink ID.
- Both kinds of persistence failure were injected by removing directory write permission. State-file and PipeWire-fragment errors remained distinct from successful live apply, and saving recovered automatically after permissions were restored.
- Output switching and default-sink metadata, loss of the selected output, disabled editing while disconnected, and recovery by selecting the other output.
- PipeWire shutdown/restart with the editor open: reconnection, rediscovery and reapplication.
- Immediate SIGTERM after accepting a new preamp setting: pending live controls and persistence flushed before exit; the measured response survived editor closure and a subsequent PipeWire restart.
- Loudness compensation for positive and negative mean gain, preamp limits, and persisted compensation preferences.
- Named presets, library save/load, song recall across library reload, artist/title disambiguation, the recall toggle, per-output song keys, quickbar slots, auto-named `(Auto)` presets and JSONL song logging.
- Master volume/mute through libpulse, playback/recording stream volume and mute, and playback stream routing to the other synthetic output.
- MPRIS title/artist/album/progress, next/previous, play/pause, absolute-position seek, ±20 s relative seek, and automatic EQ restoration when returning to an earlier song.
- Chromium: 22 labelled EQ slider/numeric controls, ArrowUp gain adjustment, keyboard reset/default-sink buttons, valid JSON and plain-text application, rejected malformed text, compensated preamp, preset save/load, maintain-loudness-when-loading, JSONL values, quickbar letter buttons, autoload wheel, master volume, player skip controls, ±20 s seek buttons, and a progress-bar click that seeks the synthetic player to the clicked offset. Bands measured at least 350 px tall with less than 65 px between adjacent slider centers. No horizontal page overflow at a 390 px viewport. The 1000 × 1250 screenshot was visually reviewed.

## Audio measurements

48 kHz stereo float PCM; a 1 kHz tone on the left and a 4 kHz tone on the right, each at amplitude 0.02. Measurements use independent Fourier amplitude extraction from a one-second settled window of the selected output's monitor. Tolerance: 0.15 dB; opposite-channel leakage threshold: −65 dB.

| Configuration | Left, 1 kHz | Right, 4 kHz |
| --- | ---: | ---: |
| All gains flat, preamp 0 dB | +0.000 dB | −0.000 dB |
| 1 kHz band +6 dB, preamp −3 dB | +3.000 dB | −2.785 dB |
| 1 kHz band +6 dB, preamp −6 dB, editor closed | +0.000 dB | −5.785 dB |
| Same saved settings after PipeWire restart | +0.000 dB | −5.785 dB |

The right-channel result includes the expected tail of the 1 kHz peaking filter; it is not exactly the preamp value. Opposite-channel tone measurements passed the −65 dB threshold. Extremely small Fourier residuals in a synthetic test are numerical observations, not a hardware crosstalk specification.

A shared-clock two-input native probe compared broadband noise at the EQ input monitor and physical-output surrogate monitor with the EQ flat. Cross-correlation measured **256 samples, or 5.333 ms**, with correlation 1.00000000. The graph quantum was 256 samples at 48 kHz. This is the measured software monitor-to-monitor path in this graph, including its scheduling. It does not measure speaker/DAC/Bluetooth latency or isolate biquad group delay under nonflat settings.

## Observed overhead

The 2026-09-11 editor executable was about 329 KiB with debug information. That is a historical build measurement, not the current executable size. It dynamically links the installed system libraries; file size is not its total memory footprint. The CPU/RSS and `pw-top` samples below are also from that run.

| Sample | Editor CPU, one core | Editor RSS |
| --- | ---: | ---: |
| Idle, no browser, 5.011 seconds | 0.5986% | 15,140 KiB |
| Playback active, no browser, 5.011 seconds | 0.7983% | 15,380 KiB |

CPU samples use `/proc/PID/stat` deltas at the host's scheduler tick resolution. Those historical samples include the former mixer/player polling and 20 Hz control timer. The event-driven implementation below replaces those loops. Browser resource usage is excluded.

Two noninitial `pw-top` snapshots during playback showed EQ input-node busy times of 10.0–11.5 µs and output-node busy times of 71.4–74.0 µs per 5.333 ms quantum; reported error counters were zero. These include PipeWire node work and are observations, not an isolated DSP benchmark or a guarantee against underruns under other loads. There is no claim of zero CPU, zero latency or zero allocation cost.

## Limits

Physical ALSA/Bluetooth playback and real hardware hotplug were not exercised. Output removal used destruction of a synthetic sink. Tests demonstrate routing policy, signal processing and software timing in isolation; actual device buffering and session-manager policy may differ. The editor does not alter system quantum/rate. While EQ is default and connected for editing, it automatically routes eligible application streams from the selected output through EQ, checking once per stream/default selection and respecting later manual moves. Mixer controls also allow explicit routing. Media integration was tested with a synthetic MPRIS player; metadata quality and supported actions vary by player. Gain compensation uses the arithmetic mean of band gains and does not measure perceptual loudness.

PipeWire 1.6.8's idle filter-graph controls may retain stale parameter readback until activation. That case is handled and tested as “saved; waiting for playback to confirm,” with live readback verified once processing begins.


## Event-driven idle operation — 2026-10-27

The controller now uses notifications and one-shot deadlines. The desktop worker uses PulseAudio subscriptions and MPRIS signals. HTTP state responses reuse cached JSON, and visible browsers wait for changes using conditional requests rather than polling. Hidden pages cancel state requests and the playback display clock. Connection failures stop after four attempts and socket changes restart recovery. Pending-save retries and the 30-second lifetime of waiting HTTP requests remain explicit deadlines.

Isolated run: `/tmp/eqweb-integration-HyWRMS`.

| Sample | Editor CPU, one core | Editor RSS |
| --- | ---: | ---: |
| Idle, no browser, 5.010 seconds | 0.0000% | 16,048 KiB |
| Playback active, no browser, 5.011 seconds | 0.0000% | 16,216 KiB |

Both observations recorded zero CPU ticks at the host's `/proc/PID/stat` resolution. They do not imply that commands, state changes or PipeWire DSP take no CPU. Audio gain, stereo isolation and editor-closed processing retain the expected results above.

Regression checks cover idle wakeups after connection attempts stop, conditional state notifications without blocking ordinary requests, external player changes via D-Bus, PulseAudio reconnect token replacement, PipeWire recovery after retry exhaustion, and absence of repeated browser requests while settled or hidden. Existing persistence, routing, output recall, browser controls and signal-processing checks remain in the suite.

The deployed desktop process (PID 2462938) recorded **zero CPU ticks over 10.002 seconds** after settling with the host audio services still failed. Its controller, desktop worker, D-Bus and HTTP threads recorded zero voluntary wakeups during that interval; GLib's filesystem-monitor thread recorded 15. The serving process and embedded JavaScript were checked through port 8932. A focused private-session-bus test additionally verified player appearance, external playback-state changes and disappearance with the namespace-filtered subscription.
