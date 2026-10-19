import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from nightcap_ac import desktop

NATIVE = """\
[Desktop Entry]
Type=Application
Name=Nightcap
Exec=env SDL_VIDEODRIVER=wayland,x11 /usr/bin/mocktail %u
MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;
X-Mocktail-Managed=true
Actions=settings;

[Desktop Action settings]
Name=Settings
Exec=/usr/bin/mocktail --settings
"""


class PatchTest(unittest.TestCase):
    def test_only_main_exec_changes(self):
        out = desktop.patch(NATIVE, "/home/u/.local/bin/nightcap-ac")
        lines = out.splitlines()
        self.assertIn("Exec=env SDL_VIDEODRIVER=wayland,x11 "
                      "/home/u/.local/bin/nightcap-ac launch %u", lines)
        self.assertIn("Exec=/usr/bin/mocktail --settings", lines)
        self.assertIn("MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;", lines)
        # Nightcap's register script must not consider this file its own.
        self.assertNotIn("X-Mocktail-Managed=true", lines)
        self.assertEqual(lines[:2], ["[Desktop Entry]", desktop.MARKER])
        self.assertEqual(lines.count(desktop.MARKER), 1)

    def test_patching_twice_is_stable(self):
        once = desktop.patch(NATIVE, "/x/nightcap-ac")
        self.assertEqual(desktop.patch(once, "/x/nightcap-ac"), once)


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        system = root / "system"
        (system / "applications").mkdir(parents=True)
        (system / "applications" / desktop.DESKTOP_ID).write_text(NATIVE)
        self.env = mock.patch.dict(os.environ, {
            "XDG_DATA_HOME": str(root / "home"), "XDG_DATA_DIRS": str(system)})
        self.env.start()
        # Never touch the real desktop database or default handlers.
        self.run_patch = mock.patch.object(desktop.subprocess, "run")
        self.run_patch.start()

    def tearDown(self):
        self.run_patch.stop()
        self.env.stop()
        self.tmp.cleanup()

    def test_install_and_remove(self):
        desktop.install("/x/nightcap-ac")
        target = desktop.target_path()
        self.assertTrue(desktop.is_ours(target))
        self.assertIn("/x/nightcap-ac launch %u", target.read_text())
        desktop.remove()
        self.assertFalse(target.exists())

    def test_existing_entry_is_backed_up_and_restored(self):
        target = desktop.target_path()
        target.parent.mkdir(parents=True)
        target.write_text("[Desktop Entry]\nName=Mine\nExec=mine\n")
        desktop.install("/x/nightcap-ac")
        self.assertTrue(desktop.is_ours(target))
        desktop.remove()
        self.assertEqual(target.read_text(), "[Desktop Entry]\nName=Mine\nExec=mine\n")

    def test_remove_leaves_foreign_entries(self):
        target = desktop.target_path()
        target.parent.mkdir(parents=True)
        target.write_text(NATIVE)
        desktop.remove()
        self.assertEqual(target.read_text(), NATIVE)

    def test_prompt_answers(self):
        with mock.patch.dict(os.environ, {"NIGHTCAP_AC_DESKTOP": "n"}):
            desktop.ask_and_install("/x/nightcap-ac")
        self.assertFalse(desktop.target_path().exists())
        with mock.patch.dict(os.environ, {"NIGHTCAP_AC_DESKTOP": "y"}):
            desktop.ask_and_install("/x/nightcap-ac")
        self.assertTrue(desktop.is_ours(desktop.target_path()))


if __name__ == "__main__":
    unittest.main()
