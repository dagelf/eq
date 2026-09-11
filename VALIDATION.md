# Validation results

Verified on 2026-09-11 with PipeWire 1.6.8, WirePlumber 0.5.15, libmicrohttpd 1.0.6, json-c 0.19 and Chromium 150.0.7871.186. Host CPU: Intel Xeon W-2123, 3.60 GHz. Optimized build with `-O2`, warnings treated as errors.

The tests used a separate PipeWire socket, D-Bus session and XDG directories, with hardware monitors disabled and two stereo null sinks. The desktop EQ fragment and archived prototypes were left intact. Final artifacts: `/tmp/eqweb-integration-Y22RNO`.

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
- Chromium: 22 labelled slider/numeric controls, ArrowRight gain adjustment, Enter activation of reset and default-sink buttons, output selection, no displayed errors and no horizontal overflow at a 390 px viewport. The 1000 × 1250 screenshot was visually reviewed.

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

The editor executable is about 185 KiB with debug information. It dynamically links the installed system libraries; that file size is not its total memory footprint.

| Sample | Editor CPU, one core | Editor RSS |
| --- | ---: | ---: |
| Idle, no browser, 5.008 seconds | 0.3993% | 8,632 KiB |
| Playback active, no browser, 5.008 seconds | 0.1997% | 8,748 KiB |

CPU samples use `/proc/PID/stat` deltas at the host's scheduler tick resolution. Other runs observed roughly 0.2–0.4%; these short samples cannot establish that playback lowers CPU usage. The control timer runs at 20 Hz, so idle cost is small but nonzero. Browser resource usage is excluded.

Two noninitial `pw-top` snapshots during playback showed EQ input-node busy times of 6.1–8.7 µs and output-node busy times of 27.4–29.5 µs per 5.333 ms quantum; reported error counters were zero. These include PipeWire node work and are observations, not an isolated DSP benchmark or a guarantee against underruns under other loads. There is no claim of zero CPU, zero latency or zero allocation cost.

## Limits

Physical ALSA/Bluetooth playback and real hardware hotplug were not exercised. Output removal used destruction of a synthetic sink. Tests demonstrate routing policy, signal processing and software timing in isolation; actual device buffering and session-manager policy may differ. The editor intentionally does not alter system quantum/rate or application-stream routing.

PipeWire 1.6.8's idle filter-graph controls may retain stale parameter readback until activation. That case is handled and tested as “saved; waiting for playback to confirm,” with live readback verified once processing begins.
