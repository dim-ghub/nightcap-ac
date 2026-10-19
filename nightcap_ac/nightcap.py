"""Finding, inspecting and restarting a running Nightcap.

Nightcap holds an flock on /run/user/<uid>/mocktail.<key>.instance.lock while
it runs; the holder's pid (from /proc/locks) is the Nightcap process. That is
how Nightcap's own settings window finds it, so we do the same, with a
process scan as a fallback for Flatpak, whose /run/user is private.

Rejoining works like Cocktail's: the session log records
"! Joining game '<server id>' place <place id>" for every server joined, and
`--launch-uri roblox://experiences/start?placeId=..&gameInstanceId=..` puts
you back in that exact server.
"""

from __future__ import annotations

import glob
import os
import re
import signal
import time
from dataclasses import dataclass
from pathlib import Path

HOOK_NAME = "libnightcap_ac_hook.so"
BINARIES = {"mocktail", "nightcap"}
JOIN_RE = re.compile(r"Joining game '([0-9a-fA-F-]+)' place (\d+)")
LEAVE_MARK = "onGameLeaveBegin()"


@dataclass(frozen=True)
class Game:
    place_id: str
    server_id: str

    @property
    def uri(self) -> str:
        return (f"roblox://experiences/start?placeId={self.place_id}"
                f"&gameInstanceId={self.server_id}")


def _lock_files() -> list[str]:
    uid = os.getuid()
    return (glob.glob(f"/run/user/{uid}/mocktail.*.instance.lock")
            + glob.glob(f"/tmp/mocktail-{uid}.*.instance.lock"))


def _flock_holders(paths: list[str]) -> list[int]:
    inodes = set()
    for p in paths:
        try:
            st = os.stat(p)
        except OSError:
            continue
        inodes.add((os.major(st.st_dev), os.minor(st.st_dev), st.st_ino))
    if not inodes:
        return []
    try:
        locks = Path("/proc/locks").read_text()
    except OSError:
        return []
    pids = []
    # "1: FLOCK  ADVISORY  WRITE 1234 00:1a:5678 0 EOF"
    for line in locks.splitlines():
        parts = line.split()
        if len(parts) < 6 or parts[1] != "FLOCK":
            continue
        try:
            pid = int(parts[4])
            major, minor, inode = parts[5].split(":")
            key = (int(major, 16), int(minor, 16), int(inode))
        except ValueError:
            continue
        if key in inodes and pid not in pids:
            pids.append(pid)
    return pids


def _exe_name(pid: int) -> str:
    try:
        return os.path.basename(os.readlink(f"/proc/{pid}/exe"))
    except OSError:
        try:
            return Path(f"/proc/{pid}/comm").read_text().strip()
        except OSError:
            return ""


def _maps(pid: int) -> str:
    try:
        return Path(f"/proc/{pid}/maps").read_text()
    except OSError:
        return ""


def find_pid() -> int | None:
    """The running Nightcap's main process, or None."""
    for pid in _flock_holders(_lock_files()):
        if _exe_name(pid) in BINARIES:
            return pid
    # Fallback: the mocktail process that has SDL3 loaded (the game window,
    # not the settings or web-view helpers).
    me = os.getpid()
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit() or int(entry.name) == me:
            continue
        pid = int(entry.name)
        if _exe_name(pid) in BINARIES and "libSDL3" in _maps(pid):
            return pid
    return None


def is_hooked(pid: int) -> bool:
    return HOOK_NAME in _maps(pid)


def session_log(pid: int) -> Path | None:
    """The session log the process is writing (its tee child holds it open)."""
    candidates = [pid]
    try:
        for entry in Path("/proc").iterdir():
            if entry.name.isdigit():
                try:
                    stat = Path(f"/proc/{entry.name}/stat").read_text()
                    if int(stat.rsplit(")", 1)[1].split()[1]) == pid:
                        candidates.append(int(entry.name))
                except (OSError, ValueError, IndexError):
                    continue
    except OSError:
        pass
    for p in candidates:
        try:
            fds = os.listdir(f"/proc/{p}/fd")
        except OSError:
            continue
        for fd in fds:
            try:
                target = os.readlink(f"/proc/{p}/fd/{fd}")
            except OSError:
                continue
            if "/logs/sessions/" in target and target.endswith(".log"):
                return Path(target)
    latest = Path.home() / ".local/state/mocktail/logs/latest.log"
    return latest if latest.exists() else None


def current_game(log: Path | None) -> Game | None:
    """The server this session is in now, or None if it isn't in one."""
    if log is None:
        return None
    game: Game | None = None
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "Joining game" in line:
                    m = JOIN_RE.search(line)
                    if m:
                        game = Game(place_id=m.group(2), server_id=m.group(1))
                elif LEAVE_MARK in line:
                    game = None
    except OSError:
        return None
    return game


def launch_uri_in(args: list[str]) -> str | None:
    """A Roblox join link among Nightcap arguments, if any."""
    for i, a in enumerate(args):
        if a == "--launch-uri" and i + 1 < len(args):
            return args[i + 1]
        if a.startswith(("roblox:", "roblox-player:")):
            return a
    return None


def stop(pid: int, timeout: float = 20.0) -> bool:
    """Close Nightcap the way its settings window does: SIGTERM, then wait.

    Nightcap leaves the game gracefully on SIGTERM. If it is still running
    after the timeout it gets SIGKILL.
    """
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return True
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not Path(f"/proc/{pid}").exists() or _is_zombie(pid):
            return True
        time.sleep(0.1)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        return True
    time.sleep(0.5)
    return not Path(f"/proc/{pid}").exists() or _is_zombie(pid)


def _is_zombie(pid: int) -> bool:
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
        return stat.rsplit(")", 1)[1].split()[0] == "Z"
    except (OSError, IndexError):
        return True
