"""Starts Nightcap with the SDL hook preloaded."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

from .hook import is_listening, socket_path

HOOK_NAME = "libnightcap_ac_hook.so"
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


def ensure_app_running() -> None:
    """Start the nightcap-ac window in the background if it isn't up yet."""
    if is_listening():
        return
    subprocess.Popen([sys.executable, "-m", "nightcap_ac"],
                     cwd=str(Path(__file__).resolve().parent.parent),
                     start_new_session=True, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)


def launch(args: list[str], use_flatpak: bool | None = None,
           start_app: bool = True) -> int:
    hook = find_hook()
    if hook is None:
        print("nightcap-ac: hook library not found; run `make` first", file=sys.stderr)
        return 1
    if start_app:
        ensure_app_running()
    native = shutil.which("mocktail")
    if use_flatpak is None:
        use_flatpak = native is None and _flatpak_installed()
    if use_flatpak:
        sock_dir = socket_path().parent
        cmd = ["flatpak", "run",
               f"--filesystem={hook.parent}:ro",
               f"--filesystem={sock_dir}",
               f"--env=LD_PRELOAD={hook}",
               f"--env=NIGHTCAP_AC_SOCKET={socket_path()}",
               FLATPAK_ID, *args]
        os.execvp(cmd[0], cmd)
    if native is None:
        print("nightcap-ac: Nightcap (mocktail) is not installed", file=sys.stderr)
        return 1
    env = dict(os.environ)
    preload = env.get("LD_PRELOAD", "")
    env["LD_PRELOAD"] = f"{hook}:{preload}" if preload else str(hook)
    os.execvpe(native, [native, *args], env)
    return 0  # unreachable
