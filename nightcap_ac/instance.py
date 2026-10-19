"""The one running nightcap-ac app, and replacing it after an update.

The app owns the hook socket. It also writes app.json next to it with its
pid and a fingerprint of the code it was started from. A later start hands
its request to that app only if the fingerprints match. Otherwise the
running app is older code (for example still sitting in the tray after an
update) that may not understand the request, so it is closed and replaced.
"""

from __future__ import annotations

import hashlib
import json
import os
import signal
import time
from pathlib import Path

from .hook import is_listening, socket_path

_PACKAGE = Path(__file__).resolve().parent


def fingerprint() -> str:
    h = hashlib.sha256(str(_PACKAGE).encode())
    for f in sorted(_PACKAGE.glob("*.py")):
        st = f.stat()
        h.update(f"{f.name}:{st.st_size}:{st.st_mtime_ns}".encode())
    return h.hexdigest()[:16]


def info_path() -> Path:
    return socket_path().parent / "app.json"


def write_info() -> None:
    path = info_path()
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps({"pid": os.getpid(), "fingerprint": fingerprint()}))
    tmp.replace(path)


def clear_info() -> None:
    try:
        data = json.loads(info_path().read_text())
        if data.get("pid") == os.getpid():
            info_path().unlink()
    except (OSError, ValueError):
        pass


def _read_info() -> dict:
    try:
        data = json.loads(info_path().read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def socket_owner() -> int | None:
    """Pid of the process bound to the hook socket (via /proc/net/unix)."""
    path = str(socket_path())
    inode = None
    try:
        for line in Path("/proc/net/unix").read_text().splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 8 and parts[7] == path:
                inode = parts[6]
                break
    except OSError:
        return None
    if inode is None:
        return None
    target = f"socket:[{inode}]"
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            for fd in os.listdir(entry / "fd"):
                if os.readlink(entry / "fd" / fd) == target:
                    return int(entry.name)
        except OSError:
            continue
    return None


def running_app() -> str:
    """'none', 'current' (same code as us) or 'stale'."""
    if not is_listening():
        return "none"
    info = _read_info()
    owner = socket_owner()
    if info.get("fingerprint") == fingerprint() and (owner is None or owner == info.get("pid")):
        return "current"
    return "stale"


def replace_stale(timeout: float = 8.0) -> bool:
    """Close an out-of-date app. True once the socket is free."""
    pid = socket_owner() or _read_info().get("pid")
    if not isinstance(pid, int) or pid == os.getpid():
        return not is_listening()
    # SIGTERM lets it shut down cleanly: Nightcap's mic goes back to your
    # real microphone and the virtual devices are removed.
    for sig, wait in ((signal.SIGTERM, timeout), (signal.SIGKILL, 2.0)):
        try:
            os.kill(pid, sig)
        except ProcessLookupError:
            break
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            if not Path(f"/proc/{pid}").exists() or not is_listening():
                return True
            time.sleep(0.1)
    return not is_listening()
