"""Receives text-box events from the SDL preload hook running in Nightcap."""

from __future__ import annotations

import os
import socket
import threading
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

# SDL_TextInputType values whose contents must never be spoken.
PASSWORD_TYPES = {4, 5, 7, 8}


def socket_path() -> Path:
    override = os.environ.get("NIGHTCAP_AC_SOCKET")
    if override:
        return Path(override)
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return Path(runtime) / "nightcap-ac" / "hook.sock"


def is_listening(path: Path | None = None) -> bool:
    """True when another nightcap-ac process already owns the socket."""
    path = path or socket_path()
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        probe.sendto(b"ping", str(path))
        return True
    except OSError:
        return False
    finally:
        probe.close()


@dataclass
class ChatTracker:
    """Rebuilds what is in the focused Roblox text box from its keystrokes.

    Roblox closes the chat bar after you press Enter, so a message is the
    box's contents at the moment Enter is pressed. Escape or losing focus
    throws the draft away, the same as in-game.
    """

    active: bool = False
    secret: bool = False
    multiline: bool = False
    chars: list[str] = field(default_factory=list)
    cursor: int = 0

    def feed(self, kind: str, fields: list[str]) -> str | None:
        """Process one hook event; returns the message text when one is sent."""
        if kind == "open":
            input_type = int(fields[0]) if fields and fields[0].lstrip("-").isdigit() else 0
            self.active = True
            self.secret = input_type in PASSWORD_TYPES
            self.multiline = len(fields) > 1 and fields[1] == "1"
            self._clear()
        elif kind == "close":
            self.active = False
            self._clear()
        elif not self.active or self.secret:
            return None
        elif kind in ("text", "paste"):
            text = fields[0] if fields else ""
            if kind == "paste":
                text = " ".join(text.split())
            self._insert(text)
        elif kind == "key" and fields:
            ctrl = len(fields) > 1 and fields[1] == "1"
            return self._key(fields[0], ctrl)
        return None

    def _clear(self) -> None:
        self.chars = []
        self.cursor = 0

    def _insert(self, text: str) -> None:
        self.chars[self.cursor:self.cursor] = list(text)
        self.cursor += len(text)

    def _word_left(self) -> int:
        i = self.cursor
        while i > 0 and self.chars[i - 1] == " ":
            i -= 1
        while i > 0 and self.chars[i - 1] != " ":
            i -= 1
        return i

    def _word_right(self) -> int:
        i = self.cursor
        n = len(self.chars)
        while i < n and self.chars[i] == " ":
            i += 1
        while i < n and self.chars[i] != " ":
            i += 1
        return i

    def _key(self, name: str, ctrl: bool) -> str | None:
        if name == "return":
            if self.multiline:
                self._insert("\n")
                return None
            message = "".join(self.chars).strip()
            self._clear()
            return message or None
        if name == "escape":
            self._clear()
        elif name == "backspace":
            start = self._word_left() if ctrl else max(0, self.cursor - 1)
            del self.chars[start:self.cursor]
            self.cursor = start
        elif name == "delete":
            end = self._word_right() if ctrl else min(len(self.chars), self.cursor + 1)
            del self.chars[self.cursor:end]
        elif name == "left":
            self.cursor = self._word_left() if ctrl else max(0, self.cursor - 1)
        elif name == "right":
            self.cursor = self._word_right() if ctrl else min(len(self.chars), self.cursor + 1)
        elif name == "home":
            self.cursor = 0
        elif name == "end":
            self.cursor = len(self.chars)
        return None


class HookListener(threading.Thread):
    """Owns the datagram socket and hands each event to a callback."""

    def __init__(self, on_event: Callable[[str, list[str]], None],
                 path: Path | None = None):
        super().__init__(daemon=True, name="hook-listener")
        self.path = path or socket_path()
        self._on_event = on_event
        self._sock: socket.socket | None = None
        self._stopping = False

    def bind(self) -> None:
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if is_listening(self.path):
            raise RuntimeError("nightcap-ac is already running")
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        sock.bind(str(self.path))
        os.chmod(self.path, 0o600)
        sock.settimeout(0.5)
        self._sock = sock

    def run(self) -> None:
        assert self._sock is not None, "bind() first"
        while not self._stopping:
            try:
                data = self._sock.recv(8192)
            except TimeoutError:
                continue
            except OSError:
                break
            parts = data.decode("utf-8", "replace").split("\t")
            if parts[0] == "ping":
                continue
            self._on_event(parts[0], parts[1:])

    def stop(self) -> None:
        self._stopping = True
        if self._sock is not None:
            self._sock.close()
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass
