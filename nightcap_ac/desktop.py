"""Per-user override of Nightcap's .desktop entry.

A copy of Nightcap's own entry is written to ~/.local/share/applications
under the same name, so it takes precedence over the system one. Its main
Exec goes through `nightcap-ac launch`, which makes the Nightcap launcher and
roblox:// links start Nightcap with the chat hook. The Settings action is left
alone. The copy drops Nightcap's X-Mocktail-Managed marker so Nightcap's
register_url_handler.sh won't overwrite it, and carries X-Nightcap-AC instead
so we only ever remove our own file.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

DESKTOP_ID = "io.github.CoderDayton.nightcap.desktop"
MARKER = "X-Nightcap-AC=true"
SCHEMES = ("x-scheme-handler/roblox", "x-scheme-handler/roblox-player")
BACKUP_SUFFIX = ".nightcap-ac-backup"


def _data_home() -> Path:
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")


def target_path() -> Path:
    return _data_home() / "applications" / DESKTOP_ID


def find_source() -> Path | None:
    """Nightcap's installed entry (native first, then Flatpak exports)."""
    dirs = [Path(d) / "applications" for d in
            (os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":") if d]
    dirs += [_data_home() / "flatpak/exports/share/applications",
             Path("/var/lib/flatpak/exports/share/applications")]
    for d in dirs:
        candidate = d / DESKTOP_ID
        if candidate.is_file() and candidate.resolve() != target_path().resolve():
            return candidate
    return None


def is_ours(path: Path) -> bool:
    try:
        return MARKER in path.read_text().splitlines()
    except OSError:
        return False


def default_launcher() -> str:
    found = shutil.which("nightcap-ac")
    if found:
        return found
    return str(Path(__file__).resolve().parent.parent / "scripts" / "nightcap-ac")


def patch(text: str, launcher: str) -> str:
    """Rewrite the main entry's Exec; keep actions and everything else."""
    out: list[str] = []
    group = ""
    patched = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            group = stripped
            out.append(line)
            if group == "[Desktop Entry]":
                out.append(MARKER)
            continue
        if group == "[Desktop Entry]":
            if stripped in ("X-Mocktail-Managed=true", MARKER):
                continue
            if stripped.startswith("Exec=") and not patched:
                line = f"Exec=env SDL_VIDEODRIVER=wayland,x11 {launcher} launch %u"
                patched = True
        out.append(line)
    if not patched:
        raise ValueError("Nightcap's desktop entry has no Exec line")
    return "\n".join(out) + "\n"


def _refresh(applications: Path) -> None:
    if shutil.which("update-desktop-database"):
        subprocess.run(["update-desktop-database", "-q", str(applications)], check=False)


def install(launcher: str | None = None) -> str:
    source = find_source()
    if source is None:
        raise FileNotFoundError("Nightcap's desktop entry was not found; is Nightcap installed?")
    target = target_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and not is_ours(target):
        backup = target.with_name(target.name + BACKUP_SUFFIX)
        shutil.copy2(target, backup)
    text = patch(source.read_text(), launcher or default_launcher())
    tmp = target.with_name(f".{target.name}.tmp")
    tmp.write_text(text)
    tmp.chmod(0o644)
    if shutil.which("desktop-file-validate"):
        check = subprocess.run(["desktop-file-validate", str(tmp)],
                               capture_output=True, text=True)
        # Only hard errors block; Nightcap's own entry may carry hints.
        if check.returncode != 0 and "error:" in check.stdout:
            tmp.unlink()
            raise ValueError(f"patched entry failed validation:\n{check.stdout}")
    tmp.replace(target)
    _refresh(target.parent)
    if shutil.which("xdg-mime"):
        for scheme in SCHEMES:
            subprocess.run(["xdg-mime", "default", DESKTOP_ID, scheme], check=False)
    return f"wrote {target} (from {source})"


def remove() -> str:
    target = target_path()
    if not target.exists():
        return "no override installed"
    if not is_ours(target):
        return f"left {target} alone: it was not written by nightcap-ac"
    target.unlink()
    backup = target.with_name(target.name + BACKUP_SUFFIX)
    if backup.exists():
        backup.replace(target)
        message = f"restored your previous {target}"
    else:
        message = f"removed {target}; the system Nightcap entry is used again"
    _refresh(target.parent)
    return message


def status() -> str:
    target = target_path()
    if is_ours(target):
        return f"installed: {target}"
    return "not installed"


def ask_and_install(launcher: str | None = None) -> int:
    """The y/n prompt used by `make install`."""
    if is_ours(target_path()):
        print(install(launcher))  # refresh the paths in our existing copy
        return 0
    choice = os.environ.get("NIGHTCAP_AC_DESKTOP", "").lower()
    if choice not in ("y", "yes", "n", "no"):
        if not sys.stdin.isatty():
            print("nightcap-ac: skipping the Nightcap launcher patch (no terminal). "
                  "Run `nightcap-ac desktop install` to add it later.")
            return 0
        print("\nPatch Nightcap's launcher so the Nightcap app entry and roblox:// links")
        print("(joining from the website) start Nightcap with Animalese chat?")
        print(f"This writes a copy of Nightcap's entry to {target_path()}.")
        print("Undo any time with `nightcap-ac desktop remove`.")
        try:
            choice = input("Patch it? [y/N] ").strip().lower()
        except EOFError:
            choice = "n"
    if choice in ("y", "yes"):
        try:
            print(install(launcher))
        except (OSError, ValueError) as e:
            print(f"nightcap-ac: {e}", file=sys.stderr)
            return 1
    else:
        print("Skipped. Run `nightcap-ac desktop install` to add it later.")
    return 0
