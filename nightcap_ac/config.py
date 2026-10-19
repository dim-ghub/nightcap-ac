"""Persistent settings in ~/.config/nightcap-ac/config.json."""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, fields
from pathlib import Path

from .animalese import Settings as VoiceSettings

OUTPUT_MODES = {
    "vc": "Voice chat only",
    "both": "Voice chat and my speakers",
    "local": "My speakers only",
}


def config_path() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "nightcap-ac" / "config.json"


@dataclass
class Config:
    enabled: bool = True
    output: str = "vc"              # one of OUTPUT_MODES
    voice: str = "normal"
    style: str = "classic"
    pitch: float = 0.0
    speed: float = 1.0
    vc_volume: float = 0.9
    local_volume: float = 0.7
    mix_real_mic: bool = True
    real_mic: str = ""              # PulseAudio source name; "" = default
    skip_commands: bool = True      # don't speak "/e dance", "/w name" ...
    sample_pack: str = ""
    max_queue: int = 3

    def voice_settings(self) -> VoiceSettings:
        return VoiceSettings(voice=self.voice, style=self.style, pitch=self.pitch,
                             speed=self.speed, sample_pack=self.sample_pack)

    @property
    def uses_vc(self) -> bool:
        return self.output in ("vc", "both")

    @property
    def uses_local(self) -> bool:
        return self.output in ("local", "both")

    @classmethod
    def load(cls, path: Path | None = None) -> "Config":
        path = path or config_path()
        try:
            data = json.loads(path.read_text())
        except (OSError, ValueError):
            return cls()
        known = {f.name for f in fields(cls)}
        cfg = cls(**{k: v for k, v in data.items() if k in known})
        if cfg.output not in OUTPUT_MODES:
            cfg.output = "vc"
        return cfg

    def save(self, path: Path | None = None) -> None:
        path = path or config_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(asdict(self), indent=2) + "\n")
        tmp.replace(path)
