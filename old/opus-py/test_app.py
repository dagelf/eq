import json
import tempfile
import unittest
from pathlib import Path

from app import (
    AppState,
    Band,
    Config,
    TemplatePreset,
    default_config,
    export_equalizer_apo,
    export_graphic_eq,
    normalize_config,
    parse_equalizer_apo,
    parse_graphic_eq,
    render_pipewire_config,
)


class EQWebPyTests(unittest.TestCase):
    def test_render_skips_zero_gain_bands(self) -> None:
        cfg = Config(
            description="Desk",
            sink_name="desk",
            sink_label="Desk",
            channels=2,
            auto_apply=False,
            config_path="/tmp/out.conf",
            state_path="/tmp/state.json",
            bands=[
                Band(id="low", type="lowshelf", freq=80, q=0.707, gain_db=0),
                Band(id="mid", type="peaking", freq=1000, q=1.0, gain_db=2.5),
                Band(id="hp", type="highpass", freq=35, q=0.8, gain_db=0),
            ],
        )
        cfg = normalize_config(cfg)
        render = render_pipewire_config(cfg)
        self.assertNotIn("name  = low", render)
        self.assertIn("name  = mid", render)
        self.assertIn("name  = hp", render)

    def test_normalize_templates(self) -> None:
        cfg = default_config(Path("/tmp/out.conf"), Path("/tmp/state.json"))
        cfg.templates = [
            TemplatePreset(
                name="Desk",
                description="Desk",
                sink_name="desk",
                sink_label="Desk",
                channels=2,
                bands=[Band(id="peak", type="peaking", freq=1000, q=1.0, gain_db=3)],
            ),
            TemplatePreset(
                name="Desk",
                description="Cinema",
                sink_name="cinema",
                sink_label="Cinema",
                channels=2,
                bands=[Band(id="hp", type="highpass", freq=35, q=0.8, gain_db=5)],
            ),
        ]
        cfg = normalize_config(cfg)
        self.assertEqual("Desk 2", cfg.templates[1].name)
        self.assertEqual(0.0, cfg.templates[1].bands[0].gain_db)

    def test_parse_equalizer_apo(self) -> None:
        bands = parse_equalizer_apo(
            """
            Filter 1: ON PK Fc 105 Hz Gain -3.2 dB Q 0.90
            Filter 2: ON HS Fc 8500 Hz Gain 4.5 dB Q 0.70
            Filter 3: ON HP Fc 35 Hz Q 0.80
            """
        )
        self.assertEqual(3, len(bands))
        self.assertEqual("highpass", bands[2].type)
        self.assertEqual(0.0, bands[2].gain_db)

    def test_parse_graphic_eq(self) -> None:
        bands = parse_graphic_eq("GraphicEQ: 20 0; 40 -2.5; 80 -1.0; 160 1.2")
        self.assertEqual(4, len(bands))
        self.assertTrue(all(band.type == "peaking" for band in bands))
        self.assertTrue(all(band.q > 0 for band in bands))

    def test_exports(self) -> None:
        cfg = default_config(Path("/tmp/out.conf"), Path("/tmp/state.json"))
        apo = export_equalizer_apo(cfg)
        graphic = export_graphic_eq(cfg)
        self.assertIn("Filter 1: ON LS", apo)
        self.assertIn("GraphicEQ:", graphic)

    def test_state_persists_templates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "pipewire.conf.d" / "99-eqweb.conf"
            state_path = Path(tmp) / "state" / "config.json"
            app = AppState(cfg_path, state_path)
            app.update_config(
                {
                    "description": "Desk EQ",
                    "sink_name": "desk-eq",
                    "sink_label": "Desk EQ",
                    "channels": 2,
                    "auto_apply": False,
                    "bands": [{"id": "one", "type": "peaking", "freq": 120, "q": 0.8, "gain_db": 2.5}],
                    "templates": [
                        {
                            "name": "Desk",
                            "description": "Desk EQ",
                            "sink_name": "desk-eq",
                            "sink_label": "Desk EQ",
                            "channels": 2,
                            "bands": [{"id": "one", "type": "peaking", "freq": 120, "q": 0.8, "gain_db": 2.5}],
                        }
                    ],
                }
            )
            saved = json.loads(state_path.read_text())
            self.assertEqual(1, len(saved["templates"]))
            self.assertEqual("Desk", saved["templates"][0]["name"])


if __name__ == "__main__":
    unittest.main()
