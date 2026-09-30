# eqweb-py

Fresh Python rewrite of the local PipeWire EQ editor.

## Run

```bash
cd new
python3 app.py
```

Open `http://127.0.0.1:8932`.

## Notes

- State is stored at `~/.local/state/eqweb/config.json` by default.
- The generated PipeWire fragment is written to `~/.config/pipewire/pipewire.conf.d/99-eqweb.conf` by default.
- Gain-first editing is local and immediate. Save and apply are explicit.
- Shelves and peaking bands with `0 dB` are skipped when rendering the PipeWire graph.
- Gain slider limits and frequency slider scale are browser-local UI settings.
