import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from nightcap_ac import instance

ROOT = Path(__file__).resolve().parent.parent

# A stand-in for a running app: binds the socket and writes app.json with the
# given fingerprint ("REAL" means this checkout's), then waits to be killed.
APP = """
import json, os, signal, sys, time
from nightcap_ac import instance
from nightcap_ac.hook import HookListener
listener = HookListener(lambda kind, fields: None)
listener.bind()
fp = instance.fingerprint() if sys.argv[1] == "REAL" else sys.argv[1]
instance.info_path().write_text(json.dumps({"pid": os.getpid(), "fingerprint": fp}))
signal.signal(signal.SIGTERM, lambda *_: (listener.stop(), sys.exit(0)))
print("ready", flush=True)
while True:
    time.sleep(1)
"""


class InstanceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sock = Path(self.tmp.name) / "hook.sock"
        self.env = mock.patch.dict(os.environ, {"NIGHTCAP_AC_SOCKET": str(self.sock)})
        self.env.start()
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        self.env.stop()
        self.tmp.cleanup()

    def start_app(self, fp):
        p = subprocess.Popen([sys.executable, "-c", APP, fp], cwd=ROOT,
                             env=dict(os.environ, PYTHONPATH=str(ROOT)),
                             stdout=subprocess.PIPE, text=True)
        self.procs.append(p)
        self.assertEqual(p.stdout.readline().strip(), "ready")
        return p

    def test_nothing_running(self):
        self.assertEqual(instance.running_app(), "none")

    def test_same_code_is_current(self):
        p = self.start_app("REAL")
        self.assertEqual(instance.running_app(), "current")
        self.assertEqual(instance.socket_owner(), p.pid)

    def test_older_code_is_replaced(self):
        p = self.start_app("0ld")
        self.assertEqual(instance.running_app(), "stale")
        self.assertTrue(instance.replace_stale(timeout=5))
        p.wait(timeout=5)
        self.assertEqual(instance.running_app(), "none")

    def test_app_from_before_app_json_is_replaced(self):
        p = self.start_app("0ld")
        instance.info_path().unlink()   # versions before this change wrote none
        self.assertEqual(instance.running_app(), "stale")
        self.assertTrue(instance.replace_stale(timeout=5))
        p.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
