# Review and TODOs

Reviewed the active native C/JavaScript implementation on 2026-09-30. The archived prototypes under `old/` are outside the refactor scope. Current behavior and ownership contracts are in [SPEC.md](SPEC.md); validation evidence is in [VALIDATION.md](VALIDATION.md). Items below are proposed work, not completed refactors. P1 is the first refactor work; P2 follows it.

## Correctness and verification gaps

- [ ] **P2 — Apply the song limit consistently.** `library_remember()` rejects a new song at 1,000 entries, but `library_follow()` copies legacy artist/title-only entries to per-output keys without that guard. A temporary native reproducer with 1,000 legacy entries grew to 1,001 after recalling one track. Introduce a shared insertion policy that preserves legacy recall when full; add a regression for a full library and repeated legacy recalls on different outputs. Also decide and document how already-oversized libraries should load; `library_open()` currently validates EQ values but not preset/song counts.
- [ ] **P2 — Honor the browser executable override in the integration launcher.** `tests/integration.sh` gates browser execution on `command -v chromium`, while `tests/browser.c` supports `CHROMIUM_BINARY`. An override alone therefore silently skips browser checks. Resolve/check the chosen executable consistently and print an explicit skip reason. Verify an override-only environment and a missing-browser environment.

## Refactor opportunities

- [ ] **P1 — Separate controller state from HTTP, PipeWire and CLI code.** `main.c` puts request validation (`handle()`), revision/pending flags (`struct app`, `tick()`), graph tracking, routing, setup and shutdown in one global module. First extract HTTP validation/response helpers around explicit controller commands and snapshots; then isolate discovery/routing and setup. Preserve preference-before-edit ordering, output guards, revision meanings and missing-output recovery. Existing HTTP and integration cases should pass at each extraction.
- [ ] **P1 — Remove filesystem waits from the locked control tick.** `tick()` holds `app.mutex` while `persist()` and `library_flush()` serialize, write and sync files; `flush_log()` can drain up to 1,000 entries with a sync per entry. Slow storage can delay HTTP state/mutations and control updates. Measure with a delayed/failing writer, then use versioned immutable save jobs or another bounded I/O design. Preserve settings-before-config ordering, independent library/log errors, retries and shutdown draining; never let an older completed write mark newer edits saved.
- [ ] **P1 — Separate recall decisions from library side effects.** `library_follow()` chooses a player, resolves song/general/fallback EQ, migrates legacy keys, creates auto presets, logs transitions and changes the supplied desktop JSON. Extract a recall selector with an explicit result/source, followed by a transition handler for persistence/logging. Preserve own-song → legacy → preferred-device → sorted-other-device → general precedence, and the rule that borrowed EQ is not stored until edited.
- [ ] **P2 — Separate browser request state from rendering.** `app.js` mixes listeners, EQ retry state, polling, library controls and mixer/player DOM updates. Extract the EQ request coordinator and focused render functions first, keeping embedded static assets and the runtime-free build. Preserve `editSequence`, `editOutput`, process-instance resets, permanent-error behavior and invalid text. Use existing browser retry, newer-edit and incomplete-input regressions as the acceptance checks.
- [ ] **P2 — Isolate mixer and player adapters inside the desktop worker.** `desktop.c` combines command validation, libpulse operations, MPRIS polling and master restoration. Synchronous `GetAll` calls for up to 16 players share a worker with mixer/output commands, so slow players can delay volume restoration. Extract adapter boundaries and measure a slow-player fixture before choosing asynchronous calls or another worker. Preserve command order, volume coalescing barriers, token generations and master epochs.
- [ ] **P2 — Make EQ policy duplication and comparisons explicit.** `config.c`, `app.js` and `web.html` repeat band counts/ranges, compensation and control constants. Keep validation on both sides of the HTTP boundary, but give parity cases one fixture source. Distinguish `eq_same()`'s exact audio-only comparison from full settings equality and 0.002 dB readback verification in names and comments. Avoid generic helpers that hide these different meanings.
- [ ] **P2 — Break large test scenarios into named cases.** `tests/library.c`, `tests/browser.c` and `tests/integration.sh` contain long ordered flows. Extract reusable fixtures and named scenario functions so failures identify the transition under test; retain explicit sequencing where the test verifies persistence, routing or restart history. Keep existing assertions and Fourier/latency thresholds.

## Missing code documentation

The user guide is detailed, but most public declarations and browser state transitions have no contract comments. The specification now records these contracts; the source still needs the following documentation.

- [ ] **`eq.h` / `config.c`:** document units, ranges, defaults, `parse_eq()`'s copy-then-commit behavior and preservation of output/omitted maintain, audio-only equality tolerance, Bluetooth key normalization, allocated return values, and 1/0/−1 load/backup results. Explain why an atomic-write error after rename can leave the replacement visible.
- [ ] **`library.h`:** document required zero initialization, caller serialization, ownership of `data`/cached device/log objects, shallow nested references in `library_snapshot()`, and the side effects/return value of `library_follow()`. Distinguish seeding, explicit general-EQ saving, song edits, debounced flush and shutdown save.
- [ ] **`desktop.h` / `desktop.c`:** document copied/borrowed/transferred JSON arguments and results, single-start lifecycle, queue capacity, NULL/error-string convention, output epoch assignment, stale-token handling, and command draining on stop. Explain the output-change barrier in volume coalescing and attenuation-before-unmute restoration. Replace the misleading “realtime-control loop” comment with “PipeWire control loop”; the editor does not run the DSP callback.
- [ ] **`main.c`:** add a module overview and state-transition comments for desired/sent/readback/saved values, each revision, pending flags and deadlines, mutex boundaries, idle readback, one-time routing checks and shutdown sync. Make clear that HTTP acceptance is neither live confirmation nor persistence.
- [ ] **`app.js`:** document which values belong to local unsent edits versus the last server snapshot; explain retry classification, `acceptedRevision`, `editSequence`, output identity, instance resets, invalid-text retention and mixer holds. Explain the separation between maintain-on-edit and maintain-on-load behavior.
- [ ] **`json_util.h`:** document borrowed string/object lifetimes and deep-copy ownership/failure behavior. Keep public API comments beside declarations; use implementation comments to explain invariants rather than narrating obvious statements.
- [ ] **Tests:** document fixture isolation and environmental dependencies, and add a short purpose comment to named regressions where timing, queue order or synthetic audio assumptions are essential. Keep measured results dated in `VALIDATION.md` rather than in source comments.

## Deferred features

- [ ] Drawing EQ curves and variable band counts (already deferred in the README).

## Completed features

- [x] Quickbar / Clear controls and A–Z snapshot buttons beside Reset to flat
- [x] Seek +20s and −20s
- [x] Maintain loudness when loading (default on)
- [x] Copy values as JSONL
- [x] Autoload preset listbox with mouse-wheel load
- [x] Auto-save an `(Auto)` preset when an edited song ends with recall enabled
- [x] Log each ended song’s final EQ to outputlog.txt when enabled (default on)
- [x] Per-output song EQ memory and separate general device EQ
- [x] Cross-device song fallback with a preferred saved device
- [x] Per-output master volume/mute recall and external mixer observation
- [x] Automatic playback routing when EQ is default and unambiguous Bluetooth recovery
- [x] Saved output selection priority, cascading fallback and automatic return on reconnect
