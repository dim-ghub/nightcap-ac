import os
import shutil
import socket
import subprocess
import tempfile
import unittest
from pathlib import Path

from nightcap_ac.hook import ChatTracker

ROOT = Path(__file__).resolve().parent.parent
HOOK = ROOT / "build" / "libnightcap_ac_hook.so"


def feed_all(tracker, events):
    sent = []
    for kind, *fields in events:
        msg = tracker.feed(kind, list(fields))
        if msg is not None:
            sent.append(msg)
    return sent


class ChatTrackerTest(unittest.TestCase):
    def test_typed_message_is_sent_on_enter(self):
        t = ChatTracker()
        sent = feed_all(t, [("open", "0", "0"), ("text", "hello"), ("text", " world"),
                            ("key", "return", "0"), ("close",)])
        self.assertEqual(sent, ["hello world"])

    def test_editing_keys(self):
        t = ChatTracker()
        sent = feed_all(t, [
            ("open", "0", "0"), ("text", "helo wrld"),
            ("key", "left", "1"),            # cursor before "wrld"
            ("key", "right", "0"), ("text", "o"),
            ("key", "home", "0"), ("key", "right", "0"), ("key", "right", "0"),
            ("key", "right", "0"), ("text", "l"),
            ("key", "end", "0"), ("text", " foo"), ("key", "backspace", "1"),
            ("key", "backspace", "0"),
            ("key", "return", "0"),
        ])
        self.assertEqual(sent, ["hello world"])

    def test_escape_and_close_discard(self):
        t = ChatTracker()
        sent = feed_all(t, [("open", "0", "0"), ("text", "nope"), ("key", "escape", "0"),
                            ("key", "return", "0"), ("text", "also no"), ("close",),
                            ("text", "closed"), ("key", "return", "0")])
        self.assertEqual(sent, [])

    def test_password_boxes_are_ignored(self):
        t = ChatTracker()
        sent = feed_all(t, [("open", "4", "0"), ("text", "hunter2"), ("key", "return", "0")])
        self.assertEqual(sent, [])

    def test_paste_and_multiline(self):
        t = ChatTracker()
        sent = feed_all(t, [("open", "0", "0"), ("paste", "copied\ntext"),
                            ("key", "return", "0")])
        self.assertEqual(sent, ["copied text"])
        sent = feed_all(t, [("open", "0", "1"), ("text", "a"), ("key", "return", "0")])
        self.assertEqual(sent, [])


@unittest.skipUnless(HOOK.exists() and shutil.which("cc"), "build the hook first (make)")
class PreloadHookTest(unittest.TestCase):
    def test_hook_reports_text_box_activity(self):
        with tempfile.TemporaryDirectory() as tmp:
            driver = Path(tmp) / "driver"
            cflags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl3"],
                                             text=True).split()
            subprocess.check_call(["cc", "-o", str(driver),
                                   str(ROOT / "tests" / "hook_driver.c"), *cflags])
            sock_path = Path(tmp) / "hook.sock"
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            sock.bind(str(sock_path))
            sock.settimeout(0.2)
            env = dict(os.environ, LD_PRELOAD=str(HOOK), NIGHTCAP_AC_SOCKET=str(sock_path),
                       SDL_VIDEO_DRIVER="dummy")
            subprocess.run([str(driver)], env=env, check=True, timeout=20)
            events = []
            while True:
                try:
                    events.append(sock.recv(8192).decode().split("\t"))
                except TimeoutError:
                    break
            sock.close()

        kinds = [e[0] for e in events]
        self.assertEqual(kinds[0], "hello")
        self.assertNotIn(["text", "w"], events, "text outside a focused box leaked")
        self.assertNotIn(["text", "hunter2"], events, "password text leaked")
        self.assertIn(["open", "0", "0"], events)
        self.assertIn(["open", "4", "0"], events)
        # Every keystroke exactly once, even with a peek in between.
        self.assertEqual(kinds.count("text"), 2)
        self.assertEqual(events.count(["key", "backspace", "0"]), 1)

        tracker = ChatTracker()
        sent = [m for e in events if (m := tracker.feed(e[0], e[1:])) is not None]
        self.assertEqual(sent, ["hi ther"])


if __name__ == "__main__":
    unittest.main()
