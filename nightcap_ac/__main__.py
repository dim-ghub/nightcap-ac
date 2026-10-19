"""nightcap-ac command line.

  nightcap-ac                 open the settings window (and start listening)
  nightcap-ac --headless      run without a window
  nightcap-ac launch [ARGS]   start Nightcap with the hook (ARGS go to Nightcap)
  nightcap-ac say TEXT        speak TEXT with the saved settings
"""

from __future__ import annotations

import argparse
import signal
import sys
import threading
import wave

import numpy as np


def _headless() -> int:
    from .config import Config
    from .engine import Engine

    seen = 0

    def on_status(status) -> None:
        nonlocal seen
        fresh = min(status.log_seq - seen, len(status.log))
        for line in status.log[len(status.log) - fresh:]:
            print(line, flush=True)
        seen = status.log_seq

    engine = Engine(Config.load(), on_status)
    try:
        engine.start()
    except RuntimeError as e:
        print(f"nightcap-ac: {e}", file=sys.stderr)
        return 1
    done = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: done.set())
    signal.signal(signal.SIGTERM, lambda *_: done.set())
    print("listening for Nightcap chat; Ctrl+C to quit", flush=True)
    done.wait()
    engine.stop()
    return 0


def _say(args: argparse.Namespace) -> int:
    from .animalese import SamplePack, synthesize
    from .audio import Player
    from .config import Config

    cfg = Config.load()
    for name in ("voice", "style", "pitch", "speed"):
        if getattr(args, name) is not None:
            setattr(cfg, name, getattr(args, name))
    pack = SamplePack(cfg.sample_pack) if cfg.sample_pack and cfg.style == "classic" else None
    audio = synthesize(" ".join(args.text), cfg.voice_settings(), pack=pack)
    if args.wav:
        with wave.open(args.wav, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(48_000)
            w.writeframes((np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes())
        return 0
    Player().play(audio, [(None, cfg.local_volume)])
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "launch":
        from .launcher import launch
        rest = argv[1:]
        flatpak = None
        if rest and rest[0] in ("--flatpak", "--native"):
            flatpak = rest[0] == "--flatpak"
            rest = rest[1:]
        if rest and rest[0] == "--":
            rest = rest[1:]
        return launch(rest, flatpak)

    parser = argparse.ArgumentParser(prog="nightcap-ac", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--headless", action="store_true")
    sub = parser.add_subparsers(dest="command")
    say = sub.add_parser("say", help="speak text with the current settings")
    say.add_argument("text", nargs="+")
    say.add_argument("--wav", help="write a WAV file instead of playing")
    say.add_argument("--voice")
    say.add_argument("--style", choices=["classic", "nh", "bebebese"])
    say.add_argument("--pitch", type=float)
    say.add_argument("--speed", type=float)
    sub.add_parser("launch", help="start Nightcap with the hook preloaded")
    args = parser.parse_args(argv)

    if args.command == "say":
        return _say(args)
    if args.headless:
        return _headless()
    from .gui import run
    return run()


if __name__ == "__main__":
    sys.exit(main())
