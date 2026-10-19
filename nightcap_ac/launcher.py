"""Starts Nightcap with the SDL hook preloaded.

`nightcap-ac launch` decides what to do from what is running:

* Nightcap running with the hook: bring up the settings window (or, when a
  roblox:// link was opened, pass the link to the running Nightcap).
* Nightcap not running: open the settings window and start Nightcap with
  the hook.
* Nightcap running without the hook: ask the settings window to offer a
  restart, optionally rejoining the same server.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
from pathlib import Path

from . import nightcap
from .hook import is_listening, socket_path

HOOK_NAME = nightcap.HOOK_NAME
FLATPAK_ID = "io.github.CoderDayton.nightcap"


def find_hook() -> Path | None:
    override = os.environ.get("NIGHTCAP_AC_HOOK")
    candidates = [Path(override)] if override else []
    here = Path(__file__).resolve().parent
    candidates += [
        here.parent / "build" / HOOK_NAME,                    # source checkout
        here.parent.parent.parent / "lib" / "nightcap-ac" / HOOK_NAME,  # make install
        Path.home() / ".local" / "lib" / "nightcap-ac" / HOOK_NAME,
        Path("/usr/lib/nightcap-ac") / HOOK_NAME,
    ]
    return next((c for c in candidates if c.is_file()), None)


def _flatpak_installed() -> bool:
    if not shutil.which("flatpak"):
        return False
    done = subprocess.run(["flatpak", "info", FLATPAK_ID], capture_output=True)
    return done.returncode == 0


def send_to_app(command: str, payload: dict | None = None) -> None:
    """Hand a command to the settings app, starting it if needed."""
    body = json.dumps(payload or {})
    if is_listening():
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            sock.sendto(f"{command}\t{body}".encode(), str(socket_path()))
        finally:
            sock.close()
        return
    subprocess.Popen([sys.executable, "-m", "nightcap_ac", "--command", command,
                      "--payload", body],
                     cwd=str(Path(__file__).resolve().parent.parent),
                     start_new_session=True, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)


def ensure_app_running() -> None:
    if not is_listening():
        send_to_app("started")


def launch(args: list[str], use_flatpak: bool | None = None,
           start_app: bool = True) -> int:
    pid = nightcap.find_pid()
    if pid is not None and nightcap.is_hooked(pid):
        if nightcap.launch_uri_in(args):
            # Nightcap is single-instance: a second start forwards the link
            # to the running (already hooked) copy.
            return _exec_nightcap(args, None, use_flatpak)
        send_to_app("show")
        return 0
    if pid is not None:
        send_to_app("offer-restart", {"pid": pid, "args": args})
        return 0

    hook = find_hook()
    if hook is None:
        print("nightcap-ac: hook library not found; run `make` first", file=sys.stderr)
        return 1
    if start_app:
        ensure_app_running()
    return _exec_nightcap(args, hook, use_flatpak)


def _exec_nightcap(args: list[str], hook: Path | None, use_flatpak: bool | None) -> int:
    native = shutil.which("mocktail")
    if use_flatpak is None:
        use_flatpak = native is None and _flatpak_installed()
    if use_flatpak:
        cmd = ["flatpak", "run"]
        if hook is not None:
            cmd += [f"--filesystem={hook.parent}:ro",
                    f"--filesystem={socket_path().parent}",
                    f"--env=LD_PRELOAD={hook}",
                    f"--env=NIGHTCAP_AC_SOCKET={socket_path()}"]
        cmd += [FLATPAK_ID, *args]
        os.execvp(cmd[0], cmd)
    if native is None:
        print("nightcap-ac: Nightcap (mocktail) is not installed", file=sys.stderr)
        return 1
    env = dict(os.environ)
    if hook is not None:
        preload = env.get("LD_PRELOAD", "")
        env["LD_PRELOAD"] = f"{hook}:{preload}" if preload else str(hook)
    os.execvpe(native, [native, *args], env)
    return 0  # unreachable


def restart(pid: int, args: list[str], rejoin: bool) -> str:
    """Close the running Nightcap and start it again with the hook.

    Returns a short description of what was done. Raises RuntimeError if
    Nightcap would not close.
    """
    link = nightcap.launch_uri_in(args)
    game = None if link else nightcap.current_game(nightcap.session_log(pid))
    if not nightcap.stop(pid):
        raise RuntimeError("Nightcap did not close; close it yourself and try again")
    new_args = list(args)
    if rejoin and game is not None:
        new_args = ["--launch-uri", game.uri, *new_args]
    subprocess.Popen([sys.executable, "-m", "nightcap_ac", "launch", *new_args],
                     cwd=str(Path(__file__).resolve().parent.parent),
                     start_new_session=True, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)
    if link:
        return "restarted Nightcap and opened your link"
    if rejoin and game is not None:
        return f"restarted Nightcap and rejoining place {game.place_id}"
    return "restarted Nightcap"
