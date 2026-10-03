# eqweb

Archived Go prototype. These historical features and run instructions do not describe the current native C editor. Use the [root README](../../README.md) for the maintained implementation. This prototype shares its configuration path and default port with that editor.

`eqweb` is a small local web service that manages a PipeWire equalizer sink.

The audio processing stays inside PipeWire using `libpipewire-module-filter-chain` and builtin biquad filters, which is the low-CPU path. The web app only edits config and writes a generated fragment to:

- `~/.config/pipewire/pipewire.conf.d/99-eqweb.conf`

State is stored at:

- `~/.local/state/eqweb/config.json`

## Features

- Arbitrary number of EQ bands
- Per-band type: `lowshelf`, `peaking`, `highshelf`, `lowpass`, `highpass`
- Slider-based browser UI served by the same binary
- Import/export for `Equalizer APO`, `GraphicEQ` / `AutoEq`, and `eqweb JSON`
- Best-effort live apply by restarting user audio services

## Run

```bash
go run .
```

Open `http://127.0.0.1:8932`.

## Build

```bash
go build .
```

## Notes

- Current implementation targets stereo sinks.
- `GraphicEQ` import/export is intentionally lossy because that format only stores frequency and gain.
- If live apply fails, the config file is still written; restart `pipewire`, `pipewire-pulse`, and `wireplumber` in the user session manually.
- Route applications to the created virtual sink with `pavucontrol`, `wpctl`, or your normal PipeWire mixer tooling.
