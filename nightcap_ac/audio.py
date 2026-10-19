"""PipeWire plumbing: the virtual microphone and audio playback.

The virtual mic is a null sink that we play speech into, exposed as a source
through module-remap-source. Your real mic is looped into the same sink, so
Roblox hears both. Nightcap's own recording stream is moved onto the virtual
mic; other apps keep using whatever they had.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import threading
from collections.abc import Callable, Iterable

import numpy as np

from .animalese import SR

SINK = "nightcap_ac_mix"
SOURCE = "nightcap_ac_mic"
NIGHTCAP_BINARIES = {"mocktail", "nightcap"}
NIGHTCAP_NAMES = {"nightcap", "mocktail", "io.github.coderdayton.nightcap"}


class AudioError(RuntimeError):
    pass


def _pactl(*args: str, check: bool = True) -> str:
    try:
        done = subprocess.run(["pactl", *args], capture_output=True, text=True,
                              timeout=10)
    except FileNotFoundError as e:
        raise AudioError("pactl not found (install pipewire-pulse / libpulse)") from e
    except subprocess.TimeoutExpired as e:
        raise AudioError(f"pactl {args[0]} timed out") from e
    if check and done.returncode != 0:
        raise AudioError(f"pactl {' '.join(args)}: {done.stderr.strip()}")
    return done.stdout


def _pactl_json(*args: str) -> list[dict]:
    return json.loads(_pactl("-f", "json", *args) or "[]")


def list_microphones() -> list[tuple[str, str]]:
    """(name, description) for real capture devices, excluding monitors."""
    mics = []
    for s in _pactl_json("list", "sources"):
        name = s.get("name", "")
        if name.endswith(".monitor") or name.startswith("nightcap_ac"):
            continue
        if s.get("properties", {}).get("device.class") == "monitor":
            continue
        mics.append((name, s.get("description") or name))
    return mics


def is_nightcap_stream(props: dict, pids: Iterable[int]) -> bool:
    pid_text = props.get("application.process.id", "")
    if pid_text.isdigit() and int(pid_text) in set(pids):
        return True
    binary = props.get("application.process.binary", "").lower()
    name = props.get("application.name", "").lower()
    app_id = props.get("application.id", "").lower()
    return binary in NIGHTCAP_BINARIES or name in NIGHTCAP_NAMES or app_id in NIGHTCAP_NAMES


class VirtualMic:
    def __init__(self, log: Callable[[str], None]):
        self._log = log
        self._modules: list[str] = []
        self._moved: dict[int, str] = {}   # source-output index -> original source
        self._lock = threading.Lock()
        self.active = False

    @staticmethod
    def remove_stale() -> None:
        """Unload modules left behind by a previous run that crashed."""
        for line in _pactl("list", "modules", "short", check=False).splitlines():
            parts = line.split("\t")
            if len(parts) >= 3 and "nightcap_ac" in parts[2]:
                _pactl("unload-module", parts[0], check=False)

    def start(self, mix_real_mic: bool, real_mic: str = "") -> None:
        with self._lock:
            if self.active:
                return
            self.remove_stale()
            try:
                self._load("module-null-sink", f"sink_name={SINK}",
                           "sink_properties=device.description='Nightcap AC mix'")
                self._load("module-remap-source", f"master={SINK}.monitor",
                           f"source_name={SOURCE}",
                           "source_properties=device.description='Nightcap AC microphone'")
                if mix_real_mic:
                    mic = real_mic or self._default_mic()
                    if mic:
                        self._load("module-loopback", f"source={mic}", f"sink={SINK}",
                                   "latency_msec=20", "source_dont_move=true",
                                   "sink_dont_move=true",
                                   "sink_input_properties=media.name='nightcap_ac real mic'")
                        self._log(f"mixing real mic {mic} into the virtual mic")
                    else:
                        self._log("no real microphone found; VC will only hear speech")
            except AudioError:
                self._unload_all()
                raise
            self.active = True

    def stop(self) -> None:
        with self._lock:
            if not self.active:
                return
            for index, original in list(self._moved.items()):
                _pactl("move-source-output", str(index), original, check=False)
            self._moved.clear()
            self._unload_all()
            self.active = False

    def route(self, pids: Iterable[int]) -> int:
        """Move Nightcap's recording streams onto the virtual mic.

        Returns how many Nightcap streams are on the virtual mic now.
        """
        with self._lock:
            if not self.active:
                return 0
            sources = {s["index"]: s["name"] for s in _pactl_json("list", "sources")
                       if "index" in s}
            ours = next((i for i, n in sources.items() if n == SOURCE), None)
            routed = 0
            live = set()
            for so in _pactl_json("list", "source-outputs"):
                index = so.get("index")
                if index is None or not is_nightcap_stream(so.get("properties", {}), pids):
                    continue
                live.add(index)
                if so.get("source") == ours:
                    routed += 1
                    continue
                original = sources.get(so.get("source"), "")
                try:
                    _pactl("move-source-output", str(index), SOURCE)
                except AudioError as e:
                    self._log(f"could not route Nightcap mic stream {index}: {e}")
                    continue
                self._moved.setdefault(index, original or "@DEFAULT_SOURCE@")
                self._log(f"routed Nightcap mic stream {index} to the virtual mic")
                routed += 1
            for gone in set(self._moved) - live:
                del self._moved[gone]
            return routed

    def _default_mic(self) -> str:
        default = _pactl("get-default-source", check=False).strip()
        if default and not default.startswith("nightcap_ac") and not default.endswith(".monitor"):
            return default
        mics = list_microphones()
        return mics[0][0] if mics else ""

    def _load(self, module: str, *args: str) -> None:
        index = _pactl("load-module", module, *args).strip()
        self._modules.append(index)

    def _unload_all(self) -> None:
        for index in reversed(self._modules):
            _pactl("unload-module", index, check=False)
        self._modules.clear()


class StreamWatcher(threading.Thread):
    """Calls on_change whenever a recording stream appears (pactl subscribe)."""

    def __init__(self, on_change: Callable[[], None]):
        super().__init__(daemon=True, name="pactl-subscribe")
        self._on_change = on_change
        self._proc: subprocess.Popen | None = None
        self._stopping = False

    def run(self) -> None:
        while not self._stopping:
            try:
                self._proc = subprocess.Popen(["pactl", "subscribe"], text=True,
                                              stdout=subprocess.PIPE,
                                              stderr=subprocess.DEVNULL)
            except FileNotFoundError:
                return
            assert self._proc.stdout is not None
            for line in self._proc.stdout:
                if "source-output" in line and ("'new'" in line or "'change'" in line):
                    self._on_change()
            self._proc.wait()

    def stop(self) -> None:
        self._stopping = True
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()


class Player:
    """Plays mono float32 buffers to one or more PipeWire targets at once."""

    def __init__(self) -> None:
        self._procs: list[subprocess.Popen] = []
        self._lock = threading.Lock()
        self._pw_cat = shutil.which("pw-cat")
        self._paplay = shutil.which("paplay")
        if not self._pw_cat and not self._paplay:
            raise AudioError("need pw-cat (pipewire) or paplay to play audio")

    def _command(self, target: str | None) -> list[str]:
        if self._pw_cat:
            cmd = [self._pw_cat, "--playback", "--raw", "--format", "f32",
                   "--rate", str(SR), "--channels", "1",
                   "--media-role", "Communication", "-P",
                   '{ "media.name": "nightcap-ac speech", "application.name": "nightcap-ac" }']
            if target:
                cmd += ["--target", target]
            return cmd + ["-"]
        cmd = [self._paplay, "--raw", "--format=float32le", f"--rate={SR}",
               "--channels=1", "--client-name=nightcap-ac"]
        if target:
            cmd.append(f"--device={target}")
        return cmd

    def play(self, samples: np.ndarray, outputs: list[tuple[str | None, float]]) -> None:
        """Block until every output has played the buffer (or stop() is called)."""
        if len(samples) == 0 or not outputs:
            return
        procs = []
        with self._lock:
            for target, volume in outputs:
                p = subprocess.Popen(self._command(target), stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
                procs.append((p, (samples * volume).astype("<f4").tobytes()))
            self._procs = [p for p, _ in procs]

        def feed(p: subprocess.Popen, data: bytes) -> None:
            try:
                assert p.stdin is not None
                p.stdin.write(data)
                p.stdin.close()
            except (BrokenPipeError, ValueError, OSError):
                pass

        feeders = [threading.Thread(target=feed, args=pd, daemon=True) for pd in procs]
        for f in feeders:
            f.start()
        for p, _ in procs:
            try:
                p.wait(timeout=len(samples) / SR + 5)
            except subprocess.TimeoutExpired:
                p.kill()
        with self._lock:
            self._procs = []

    def stop(self) -> None:
        with self._lock:
            for p in self._procs:
                if p.poll() is None:
                    p.kill()
