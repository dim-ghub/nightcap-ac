"""Ties the hook, the synthesizer and the audio routing together."""

from __future__ import annotations

import queue
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field, replace

from .animalese import SamplePack, synthesize
from .audio import SINK, AudioError, Player, StreamWatcher, VirtualMic
from .config import Config
from .hook import ChatTracker, HookListener


@dataclass
class Status:
    nightcap_pid: int | None = None
    text_box_open: bool = False
    vc_ready: bool = False
    routed_streams: int = 0
    last_message: str = ""
    error: str = ""
    log: list[str] = field(default_factory=list)
    log_seq: int = 0    # total lines ever logged, to find new ones


class Engine:
    def __init__(self, config: Config,
                 on_status: Callable[[Status], None] = lambda s: None):
        self.config = config
        self.status = Status()
        self._on_status = on_status
        self._tracker = ChatTracker()
        self._listener = HookListener(self._on_hook_event)
        self._mic = VirtualMic(self._log)
        self._watcher = StreamWatcher(self._schedule_route)
        self._player = Player()
        self._speech: queue.Queue[str | None] = queue.Queue()
        self._worker = threading.Thread(target=self._speak_loop, daemon=True,
                                        name="speech")
        self._route_event = threading.Event()
        self._router = threading.Thread(target=self._route_loop, daemon=True,
                                        name="router")
        self._pack: SamplePack | None = None
        self._pack_path = ""
        self._lock = threading.RLock()
        self._running = False

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        self._running = True
        self._listener.bind()
        self._listener.start()
        self._worker.start()
        self._router.start()
        self._watcher.start()
        self._apply_routing()
        self._emit()

    def stop(self) -> None:
        self._running = False
        self._listener.stop()
        self._watcher.stop()
        self._player.stop()
        self._speech.put(None)
        self._route_event.set()
        try:
            self._mic.stop()
        except AudioError as e:
            self._log(str(e))

    def update_config(self, config: Config) -> None:
        with self._lock:
            old = self.config
            self.config = config
            mic_changed = (old.mix_real_mic, old.real_mic) != (config.mix_real_mic, config.real_mic)
        if mic_changed and self._mic.active:
            self._mic.stop()
        self._apply_routing()
        self._emit()

    # -- speaking ----------------------------------------------------------

    def say(self, text: str, force: bool = False) -> None:
        """Queue a message. force speaks even while disabled (test button)."""
        with self._lock:
            cfg = self.config
        if not force and not cfg.enabled:
            return
        if self._speech.qsize() >= max(1, cfg.max_queue):
            self._log("speech queue full; dropping message")
            return
        self._speech.put(text)

    def stop_speaking(self) -> None:
        while not self._speech.empty():
            try:
                self._speech.get_nowait()
            except queue.Empty:
                break
        self._player.stop()

    def _speak_loop(self) -> None:
        while True:
            text = self._speech.get()
            if text is None:
                return
            with self._lock:
                cfg = self.config
            try:
                audio = synthesize(text, cfg.voice_settings(), pack=self._sample_pack(cfg))
                outputs: list[tuple[str | None, float]] = []
                if cfg.uses_vc and self._mic.active:
                    outputs.append((SINK, cfg.vc_volume))
                if cfg.uses_local:
                    outputs.append((None, cfg.local_volume))
                self.status.last_message = text
                self._emit()
                self._player.play(audio, outputs)
            except Exception as e:  # keep the worker alive whatever happens
                self._log(f"speech failed: {e}")

    def _sample_pack(self, cfg: Config) -> SamplePack | None:
        if not cfg.sample_pack or cfg.style != "classic":
            return None
        if cfg.sample_pack != self._pack_path:
            self._pack_path = cfg.sample_pack
            try:
                self._pack = SamplePack(cfg.sample_pack)
            except (OSError, ValueError) as e:
                self._pack = None
                self._log(f"sample pack: {e}")
        return self._pack

    # -- hook events -------------------------------------------------------

    def _on_hook_event(self, kind: str, fields: list[str]) -> None:
        if kind == "hello":
            if fields and fields[0].isdigit():
                self.status.nightcap_pid = int(fields[0])
                self._log(f"Nightcap connected (pid {fields[0]})")
                self._schedule_route()
            self._emit()
            return
        message = self._tracker.feed(kind, fields)
        if kind in ("open", "close"):
            self.status.text_box_open = self._tracker.active
            self._emit()
        if message is None:
            return
        with self._lock:
            cfg = self.config
        if cfg.skip_commands and message.startswith("/"):
            return
        self.say(message)

    # -- routing -----------------------------------------------------------

    def _apply_routing(self) -> None:
        with self._lock:
            cfg = self.config
        want = cfg.enabled and cfg.uses_vc
        try:
            if want and not self._mic.active:
                self._mic.start(cfg.mix_real_mic, cfg.real_mic)
                self._log("virtual mic ready")
            elif not want and self._mic.active:
                self._mic.stop()
                self._log("virtual mic removed; Nightcap is back on your real mic")
            self.status.error = ""
        except AudioError as e:
            self.status.error = str(e)
            self._log(str(e))
        self.status.vc_ready = self._mic.active
        self._schedule_route()

    def _schedule_route(self) -> None:
        self._route_event.set()

    def _route_loop(self) -> None:
        while True:
            # Wake on stream changes, and re-check every few seconds anyway.
            self._route_event.wait(timeout=5.0)
            self._route_event.clear()
            if not self._running:
                return
            time.sleep(0.2)  # let a new stream settle before moving it
            pids = [self.status.nightcap_pid] if self.status.nightcap_pid else []
            try:
                routed = self._mic.route(pids)
            except AudioError as e:
                self._log(str(e))
                continue
            if routed != self.status.routed_streams:
                self.status.routed_streams = routed
                self._emit()

    # -- status ------------------------------------------------------------

    def _log(self, line: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        self.status.log = (self.status.log + [f"{stamp} {line}"])[-200:]
        self.status.log_seq += 1
        self._emit()

    def _emit(self) -> None:
        self._on_status(replace(self.status, log=list(self.status.log)))
