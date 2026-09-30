# Validation results

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

The editor executable is about 329 KiB with debug information. It dynamically links the installed system libraries; that file size is not its total memory footprint.

| Sample | Editor CPU, one core | Editor RSS |
| --- | ---: | ---: |
| Idle, no browser, 5.011 seconds | 0.5986% | 15,140 KiB |
| Playback active, no browser, 5.011 seconds | 0.7983% | 15,380 KiB |

CPU samples use `/proc/PID/stat` deltas at the host's scheduler tick resolution. These short samples include polling the mixer and test media player. The control timer runs at 20 Hz, so idle cost is small but nonzero. Browser resource usage is excluded.

Two noninitial `pw-top` snapshots during playback showed EQ input-node busy times of 10.0–11.5 µs and output-node busy times of 71.4–74.0 µs per 5.333 ms quantum; reported error counters were zero. These include PipeWire node work and are observations, not an isolated DSP benchmark or a guarantee against underruns under other loads. There is no claim of zero CPU, zero latency or zero allocation cost.

## Limits

Physical ALSA/Bluetooth playback and real hardware hotplug were not exercised. Output removal used destruction of a synthetic sink. Tests demonstrate routing policy, signal processing and software timing in isolation; actual device buffering and session-manager policy may differ. The editor does not alter system quantum/rate. Application-stream routing changes only when requested through the mixer. Media integration was tested with a synthetic MPRIS player; metadata quality and supported actions vary by player. Gain compensation uses the arithmetic mean of band gains and does not measure perceptual loudness.

PipeWire 1.6.8's idle filter-graph controls may retain stale parameter readback until activation. That case is handled and tested as “saved; waiting for playback to confirm,” with live readback verified once processing begins.
