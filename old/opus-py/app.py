from __future__ import annotations

import json
import math
import os
import re
import subprocess
import threading
from copy import deepcopy
from dataclasses import asdict, dataclass, field
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


SUPPORTED_TYPES = {"peaking", "lowshelf", "highshelf", "lowpass", "highpass"}


@dataclass
class Band:
    id: str
    type: str
    freq: float
    q: float
    gain_db: float


@dataclass
class TemplatePreset:
    name: str
    description: str
    sink_name: str
    sink_label: str
    channels: int
    bands: list[Band] = field(default_factory=list)


@dataclass
class Config:
    description: str
    sink_name: str
    sink_label: str
    channels: int
    auto_apply: bool
    config_path: str
    state_path: str
    last_apply_note: str = ""
    bands: list[Band] = field(default_factory=list)
    templates: list[TemplatePreset] = field(default_factory=list)


def env_or(name: str, default: str) -> str:
    value = os.environ.get(name, "").strip()
    return value or default


def sanitize_name(value: str) -> str:
    return re.sub(r"[^a-z0-9_-]+", "-", value.strip().lower()).strip("-_")


def default_paths() -> tuple[Path, Path]:
    home = Path.home()
    state_dir = home / ".local" / "state" / "eqweb"
    cfg_path = Path(env_or("EQWEB_CONFIG_PATH", str(home / ".config" / "pipewire" / "pipewire.conf.d" / "99-eqweb.conf")))
    state_path = Path(env_or("EQWEB_STATE_PATH", str(state_dir / "config.json")))
    return cfg_path, state_path


def default_config(config_path: Path, state_path: Path) -> Config:
    return Config(
        description="EQ Web Sink",
        sink_name="eqweb",
        sink_label="EQ Web Sink",
        channels=2,
        auto_apply=False,
        config_path=str(config_path),
        state_path=str(state_path),
        bands=[
            Band(id="low", type="lowshelf", freq=80, q=0.707, gain_db=0),
            Band(id="mid", type="peaking", freq=1000, q=1.0, gain_db=0),
            Band(id="high", type="highshelf", freq=8000, q=0.707, gain_db=0),
        ],
        templates=[],
    )


def band_from_dict(raw: dict[str, Any], fallback_id: str) -> Band:
    return Band(
        id=str(raw.get("id", fallback_id)),
        type=str(raw.get("type", "peaking")),
        freq=float(raw.get("freq", 1000)),
        q=float(raw.get("q", 1)),
        gain_db=float(raw.get("gain_db", 0)),
    )


def template_from_dict(raw: dict[str, Any], index: int) -> TemplatePreset:
    bands = [band_from_dict(item, f"band{i + 1}") for i, item in enumerate(raw.get("bands", []))]
    return TemplatePreset(
        name=str(raw.get("name", f"Template {index + 1}")),
        description=str(raw.get("description", "")),
        sink_name=str(raw.get("sink_name", "")),
        sink_label=str(raw.get("sink_label", "")),
        channels=int(raw.get("channels", 2) or 2),
        bands=bands,
    )


def config_from_dict(raw: dict[str, Any], config_path: Path, state_path: Path) -> Config:
    bands = [band_from_dict(item, f"band{i + 1}") for i, item in enumerate(raw.get("bands", []))]
    templates = [template_from_dict(item, i) for i, item in enumerate(raw.get("templates", []))]
    return Config(
        description=str(raw.get("description", "")),
        sink_name=str(raw.get("sink_name", "")),
        sink_label=str(raw.get("sink_label", "")),
        channels=int(raw.get("channels", 2) or 2),
        auto_apply=bool(raw.get("auto_apply", False)),
        config_path=str(config_path),
        state_path=str(state_path),
        last_apply_note=str(raw.get("last_apply_note", "")),
        bands=bands,
        templates=templates,
    )


def normalize_band(band: Band, index: int, seen: set[str]) -> Band:
    band.id = sanitize_name(band.id) or f"band{index + 1}"
    if band.id in seen:
        band.id = f"{band.id}{index + 1}"
    seen.add(band.id)
    band.type = band.type.strip().lower()
    if band.type not in SUPPORTED_TYPES:
        raise ValueError(f'band "{band.id}" has unsupported type "{band.type}"')
    if not 10 <= band.freq <= 30000:
        raise ValueError(f'band "{band.id}" frequency out of range')
    if not 0.05 < band.q <= 20:
        raise ValueError(f'band "{band.id}" Q out of range')
    if not -24 <= band.gain_db <= 24:
        raise ValueError(f'band "{band.id}" gain out of range')
    if band.type in {"lowpass", "highpass"}:
        band.gain_db = 0.0
    return band


def normalize_template(template: TemplatePreset, index: int) -> TemplatePreset:
    cfg = Config(
        description=template.description,
        sink_name=template.sink_name,
        sink_label=template.sink_label,
        channels=template.channels,
        auto_apply=False,
        config_path="",
        state_path="",
        bands=deepcopy(template.bands),
        templates=[],
    )
    cfg = normalize_config(cfg)
    template.name = template.name.strip() or f"Template {index + 1}"
    template.description = cfg.description
    template.sink_name = cfg.sink_name
    template.sink_label = cfg.sink_label
    template.channels = cfg.channels
    template.bands = cfg.bands
    return template


def normalize_config(cfg: Config) -> Config:
    cfg.description = cfg.description.strip() or "EQ Web Sink"
    cfg.sink_name = sanitize_name(cfg.sink_name) or "eqweb"
    cfg.sink_label = cfg.sink_label.strip() or cfg.description
    cfg.channels = cfg.channels or 2
    if cfg.channels != 2:
        raise ValueError("only stereo output is supported right now")
    if not cfg.bands:
        raise ValueError("at least one band is required")

    seen: set[str] = set()
    cfg.bands = [normalize_band(deepcopy(band), index, seen) for index, band in enumerate(cfg.bands)]
    cfg.bands.sort(key=lambda band: (band.freq, band.id))

    seen_templates: set[str] = set()
    normalized_templates: list[TemplatePreset] = []
    for index, template in enumerate(cfg.templates):
        normalized = normalize_template(deepcopy(template), index)
        if normalized.name in seen_templates:
            normalized.name = f"{normalized.name} {index + 1}"
        seen_templates.add(normalized.name)
        normalized_templates.append(normalized)
    cfg.templates = normalized_templates
    return cfg


def pipewire_label(kind: str) -> str:
    return {
        "lowshelf": "bq_lowshelf",
        "highshelf": "bq_highshelf",
        "lowpass": "bq_lowpass",
        "highpass": "bq_highpass",
    }.get(kind, "bq_peaking")


def active_bands(cfg: Config) -> list[Band]:
    items: list[Band] = []
    for band in cfg.bands:
        if band.type in {"peaking", "lowshelf", "highshelf"} and band.gain_db == 0:
            continue
        items.append(band)
    return items


def render_pipewire_config(cfg: Config) -> str:
    bands = active_bands(cfg)
    node_lines: list[str] = []
    link_lines: list[str] = []
    for index, band in enumerate(bands):
        node_lines.extend(
            [
                "                    {",
                "                        type  = builtin",
                f"                        name  = {band.id}",
                f"                        label = {pipewire_label(band.type)}",
                (
                    f'                        control = {{ "Freq" = {band.freq:.3f} "Q" = {band.q:.3f} }}'
                    if band.type in {"lowpass", "highpass"}
                    else f'                        control = {{ "Freq" = {band.freq:.3f} "Q" = {band.q:.3f} "Gain" = {band.gain_db:.3f} }}'
                ),
                "                    }",
            ]
        )
        if index < len(bands) - 1:
            link_lines.append(f'                    {{ output = "{band.id}:Out" input = "{bands[index + 1].id}:In" }}')

    return "\n".join(
        [
            "# generated by eqweb-py",
            "context.modules = [",
            "    { name = libpipewire-module-filter-chain",
            "        args = {",
            f'            node.description = "{cfg.description}"',
            f'            media.name       = "{cfg.sink_label}"',
            "            filter.graph = {",
            "                nodes = [",
            *node_lines,
            "                ]",
            "                links = [",
            *link_lines,
            "                ]",
            "            }",
            f"            audio.channels = {cfg.channels}",
            "            audio.position = [ FL FR ]",
            "            capture.props = {",
            f'                node.name   = "effect_input.{cfg.sink_name}"',
            "                media.class = Audio/Sink",
            "            }",
            "            playback.props = {",
            f'                node.name    = "effect_output.{cfg.sink_name}"',
            "                node.passive = true",
            "            }",
            "        }",
            "    }",
            "]",
            "",
        ]
    )


def restart_user_audio() -> None:
    for cmd in (
        ["systemctl", "--user", "restart", "pipewire.service"],
        ["systemctl", "--user", "restart", "pipewire-pulse.service"],
        ["systemctl", "--user", "restart", "wireplumber.service"],
    ):
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).strip()
            raise RuntimeError(f'{" ".join(cmd)}: {detail}')


APO_RE = re.compile(
    r"(?i)^filter\s+\d+:\s*(?:on\s+)?([A-Z]+)\s+fc\s+([0-9.]+)\s*hz(?:\s+gain\s+([-0-9.]+)\s*dB)?(?:\s+q\s+([0-9.]+))?"
)


def apo_type_to_band_type(token: str) -> str:
    mapping = {"PK": "peaking", "LS": "lowshelf", "LSC": "lowshelf", "HS": "highshelf", "HSC": "highshelf", "HP": "highpass", "LP": "lowpass"}
    if token not in mapping:
        raise ValueError(f'unsupported Equalizer APO filter type "{token}"')
    return mapping[token]


def parse_equalizer_apo(content: str) -> list[Band]:
    bands: list[Band] = []
    for raw in content.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        match = APO_RE.match(line)
        if not match:
            continue
        kind = apo_type_to_band_type(match.group(1).upper())
        freq = float(match.group(2))
        gain = float(match.group(3)) if match.group(3) else 0.0
        q = float(match.group(4)) if match.group(4) else (1.0 if kind == "peaking" else 0.707)
        bands.append(Band(id=f"f{len(bands) + 1}", type=kind, freq=freq, q=q, gain_db=gain))
    if not bands:
        raise ValueError("no Equalizer APO filters were found")
    return bands


def guess_graphic_q(index: int, freqs: list[float], freq: float) -> float:
    if len(freqs) < 2:
        return 1.414
    left = freqs[index - 1] if index > 0 else freq
    right = freqs[index + 1] if index + 1 < len(freqs) else freq
    if left != freq and right != freq:
        ratio = math.sqrt(right / left)
    elif right != freq:
        ratio = right / freq
    elif left != freq:
        ratio = freq / left
    else:
        ratio = 2.0
    if ratio <= 1:
        return 1.414
    bw = math.log(ratio, 2)
    if bw <= 0:
        return 1.414
    q = math.sqrt(2**bw) / ((2**bw) - 1)
    return min(8.0, max(0.2, q))


def parse_graphic_eq(content: str) -> list[Band]:
    text = content.strip()
    index = text.lower().find("graphiceq:")
    if index >= 0:
        text = text[index + len("graphiceq:") :]
    chunks = [chunk.strip() for chunk in re.split(r"[;\n\r]+", text) if chunk.strip()]
    freqs: list[float] = []
    points: list[tuple[float, float]] = []
    for chunk in chunks:
        fields = chunk.split()
        if len(fields) < 2:
            continue
        try:
            freq = float(fields[0])
            gain = float(fields[1])
        except ValueError:
            continue
        freqs.append(freq)
        points.append((freq, gain))
    if not points:
        raise ValueError("no GraphicEQ points were found")
    return [
        Band(id=f"g{i + 1}", type="peaking", freq=freq, q=guess_graphic_q(i, freqs, freq), gain_db=gain)
        for i, (freq, gain) in enumerate(points)
    ]


def export_equalizer_apo(cfg: Config) -> str:
    lines = ["# exported by eqweb-py"]
    for index, band in enumerate(cfg.bands, 1):
        token = {"peaking": "PK", "lowshelf": "LS", "highshelf": "HS", "highpass": "HP", "lowpass": "LP"}[band.type]
        line = f"Filter {index}: ON {token} Fc {band.freq:.3f} Hz"
        if band.type not in {"highpass", "lowpass"}:
            line += f" Gain {band.gain_db:.3f} dB"
        line += f" Q {band.q:.3f}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def export_graphic_eq(cfg: Config) -> str:
    return "GraphicEQ: " + "; ".join(f"{band.freq:.3f} {band.gain_db:.3f}" for band in cfg.bands) + "\n"


def import_config(fmt: str, content: str, base: Config, config_path: Path, state_path: Path) -> tuple[Config, str]:
    fmt = fmt.strip().lower()
    raw_base = config_to_dict(base)
    if fmt == "eqweb-json":
        imported = config_from_dict(json.loads(content), config_path, state_path)
    elif fmt == "equalizer-apo":
        imported = config_from_dict(raw_base, config_path, state_path)
        imported.bands = parse_equalizer_apo(content)
    elif fmt == "graphic-eq":
        imported = config_from_dict(raw_base, config_path, state_path)
        imported.bands = parse_graphic_eq(content)
    else:
        raise ValueError(f'unsupported import format "{fmt}"')
    imported = normalize_config(imported)
    return imported, f"imported {len(imported.bands)} bands from {fmt}"


def export_config(fmt: str, cfg: Config) -> str:
    fmt = fmt.strip().lower()
    if fmt == "eqweb-json":
        return json.dumps(config_to_dict(cfg), indent=2) + "\n"
    if fmt == "equalizer-apo":
        return export_equalizer_apo(cfg)
    if fmt == "graphic-eq":
        return export_graphic_eq(cfg)
    raise ValueError(f'unsupported export format "{fmt}"')


def band_to_dict(band: Band) -> dict[str, Any]:
    return asdict(band)


def template_to_dict(template: TemplatePreset) -> dict[str, Any]:
    return {
        "name": template.name,
        "description": template.description,
        "sink_name": template.sink_name,
        "sink_label": template.sink_label,
        "channels": template.channels,
        "bands": [band_to_dict(band) for band in template.bands],
    }


def config_to_dict(cfg: Config) -> dict[str, Any]:
    return {
        "description": cfg.description,
        "sink_name": cfg.sink_name,
        "sink_label": cfg.sink_label,
        "channels": cfg.channels,
        "auto_apply": cfg.auto_apply,
        "config_path": cfg.config_path,
        "state_path": cfg.state_path,
        "last_apply_note": cfg.last_apply_note,
        "bands": [band_to_dict(band) for band in cfg.bands],
        "templates": [template_to_dict(template) for template in cfg.templates],
    }


class AppState:
    def __init__(self, config_path: Path, state_path: Path) -> None:
        self.config_path = config_path
        self.state_path = state_path
        self.lock = threading.RLock()
        self.cfg = default_config(config_path, state_path)
        self.last_render = render_pipewire_config(self.cfg)
        self.load()

    def load(self) -> None:
        if not self.state_path.exists():
            return
        raw = json.loads(self.state_path.read_text())
        cfg = normalize_config(config_from_dict(raw, self.config_path, self.state_path))
        with self.lock:
            self.cfg = cfg
            self.last_render = render_pipewire_config(cfg)

    def save_locked(self) -> None:
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(config_to_dict(self.cfg), indent=2) + "\n")

    def snapshot(self) -> tuple[Config, str]:
        with self.lock:
            return deepcopy(self.cfg), self.last_render

    def update_config(self, raw: dict[str, Any]) -> tuple[Config, str]:
        cfg = normalize_config(config_from_dict(raw, self.config_path, self.state_path))
        render = render_pipewire_config(cfg)
        with self.lock:
            self.cfg = cfg
            self.last_render = render
            self.save_locked()
        return deepcopy(cfg), render

    def apply_current(self) -> tuple[Config, str, str]:
        with self.lock:
            render = render_pipewire_config(self.cfg)
            self.config_path.parent.mkdir(parents=True, exist_ok=True)
            self.config_path.write_text(render)
            self.last_render = render
            message = "PipeWire config written. Restart PipeWire to apply."
            try:
                restart_user_audio()
                message = "PipeWire config written and user audio services restarted."
            except Exception as exc:  # noqa: BLE001
                message += f" Live restart failed: {exc}"
            self.cfg.last_apply_note = message
            self.save_locked()
            return deepcopy(self.cfg), render, message

    def import_preset(self, fmt: str, content: str) -> tuple[Config, str]:
        with self.lock:
            cfg, message = import_config(fmt, content, self.cfg, self.config_path, self.state_path)
            self.cfg = cfg
            self.last_render = render_pipewire_config(cfg)
            self.save_locked()
            return deepcopy(cfg), message


def json_response(handler: BaseHTTPRequestHandler, status: int, payload: dict[str, Any]) -> None:
    body = json.dumps(payload).encode()
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


class EQHandler(BaseHTTPRequestHandler):
    server_version = "eqweb-py/0.1"

    @property
    def app_state(self) -> AppState:
        return self.server.app_state  # type: ignore[attr-defined]

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/":
            index_path = Path(__file__).with_name("static") / "index.html"
            body = index_path.read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/api/config":
            cfg, render = self.app_state.snapshot()
            json_response(self, HTTPStatus.OK, {"ok": True, "config": config_to_dict(cfg), "render": render})
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def do_PUT(self) -> None:  # noqa: N802
        if self.path != "/api/config":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        try:
            payload = self.read_json()
            cfg, render = self.app_state.update_config(payload)
            if cfg.auto_apply:
                cfg, render, message = self.app_state.apply_current()
            else:
                message = "configuration saved"
            json_response(self, HTTPStatus.OK, {"ok": True, "message": message, "config": config_to_dict(cfg), "render": render})
        except ValueError as exc:
            json_response(self, HTTPStatus.BAD_REQUEST, {"ok": False, "message": str(exc)})

    def do_POST(self) -> None:  # noqa: N802
        try:
            if self.path == "/api/apply":
                cfg, render, message = self.app_state.apply_current()
                json_response(self, HTTPStatus.OK, {"ok": True, "message": message, "config": config_to_dict(cfg), "render": render})
                return
            if self.path == "/api/import":
                payload = self.read_json()
                cfg, message = self.app_state.import_preset(str(payload.get("format", "")), str(payload.get("content", "")))
                _, render = self.app_state.snapshot()
                json_response(self, HTTPStatus.OK, {"ok": True, "message": message, "config": config_to_dict(cfg), "render": render})
                return
            if self.path == "/api/export":
                payload = self.read_json()
                cfg, _ = self.app_state.snapshot()
                content = export_config(str(payload.get("format", "")), cfg)
                json_response(self, HTTPStatus.OK, {"ok": True, "message": "configuration exported", "format": payload.get("format", ""), "content": content})
                return
            self.send_error(HTTPStatus.NOT_FOUND)
        except ValueError as exc:
            json_response(self, HTTPStatus.BAD_REQUEST, {"ok": False, "message": str(exc)})

    def read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        try:
            return json.loads(self.rfile.read(length) if length else b"{}")
        except json.JSONDecodeError as exc:
            raise ValueError("invalid json") from exc

    def log_message(self, fmt: str, *args: Any) -> None:
        return


def run() -> None:
    config_path, state_path = default_paths()
    config_path.parent.mkdir(parents=True, exist_ok=True)
    state_path.parent.mkdir(parents=True, exist_ok=True)
    host, port_text = env_or("EQWEB_ADDR", "127.0.0.1:8932").split(":", 1)
    port = int(port_text)
    server = ThreadingHTTPServer((host, port), EQHandler)
    server.app_state = AppState(config_path, state_path)  # type: ignore[attr-defined]
    print(f"serving eqweb-py on http://{host}:{port}")
    server.serve_forever()


if __name__ == "__main__":
    run()
