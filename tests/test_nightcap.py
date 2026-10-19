import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from nightcap_ac import launcher, nightcap

ROOT = Path(__file__).resolve().parent.parent
HOOK = ROOT / "build" / nightcap.HOOK_NAME

LOG = """\
[I/Roblox] 2026-10-08T00:14:18.693Z,12.69,d1,6 [FLog::Output] ! Joining game 'f0adcb34-df75-4e59-9a5e-0008641fedea' place 18667984660 at 10.0.0.1
[D/rbx.jni] onGameLoaded() SessionReporterState_GameLoaded placeId:18667984660
"""
LEAVE = "[D/rbx.jni] onGameLeaveBegin() SessionReporterState_GameExitRequested placeId:18667984660\n"
SECOND = ("[I/Roblox] x [FLog::Output] ! Joining game "
          "'11111111-2222-3333-4444-555555555555' place 42 at 10.0.0.2\n")


def wait_for(predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return False


class GameLogTest(unittest.TestCase):
    def parse(self, text):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(text)
        try:
            return nightcap.current_game(Path(f.name))
        finally:
            os.unlink(f.name)

    def test_in_game(self):
        game = self.parse(LOG)
        self.assertEqual(game.place_id, "18667984660")
        self.assertEqual(game.uri, "roblox://experiences/start?placeId=18667984660"
                                   "&gameInstanceId=f0adcb34-df75-4e59-9a5e-0008641fedea")

    def test_left_the_game(self):
        self.assertIsNone(self.parse(LOG + LEAVE))

    def test_latest_join_wins(self):
        self.assertEqual(self.parse(LOG + LEAVE + SECOND).place_id, "42")

    def test_launch_uri_in(self):
        self.assertEqual(nightcap.launch_uri_in(["roblox://x"]), "roblox://x")
        self.assertEqual(nightcap.launch_uri_in(["--launch-uri", "roblox-player:1"]),
                         "roblox-player:1")
        self.assertIsNone(nightcap.launch_uri_in(["--settings"]))


@unittest.skipUnless(shutil.which("flock"), "needs util-linux flock")
class ProcessTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        self.tmp.cleanup()

    def spawn(self, cmd, **kw):
        p = subprocess.Popen(cmd, **kw)
        self.procs.append(p)
        return p

    def test_finds_the_lock_holder_named_mocktail(self):
        # A copy of flock(1) named "mocktail" holding an instance lock looks
        # exactly like Nightcap to the detection code.
        fake = self.dir / "mocktail"
        shutil.copy(shutil.which("flock"), fake)
        lock = self.dir / "mocktail.test.instance.lock"
        p = self.spawn([str(fake), str(lock), "sleep", "30"])
        self.assertTrue(wait_for(lambda: p.pid in nightcap._flock_holders([str(lock)])))
        with mock.patch.object(nightcap, "_lock_files", return_value=[str(lock)]):
            self.assertEqual(nightcap.find_pid(), p.pid)
        self.assertFalse(nightcap.is_hooked(p.pid))

    @unittest.skipUnless(HOOK.exists(), "build the hook first (make)")
    def test_detects_the_preloaded_hook(self):
        env = dict(os.environ, LD_PRELOAD=str(HOOK), NIGHTCAP_AC_SOCKET=str(self.dir / "s"))
        p = self.spawn(["sleep", "30"], env=env)
        self.assertTrue(wait_for(lambda: nightcap.is_hooked(p.pid)))

    def test_stop_terms_then_kills(self):
        polite = self.spawn(["sleep", "30"])
        self.assertTrue(nightcap.stop(polite.pid, timeout=5))
        stubborn = self.spawn(["bash", "-c", "trap '' TERM; while :; do sleep 0.1; done"])
        time.sleep(0.3)
        start = time.monotonic()
        self.assertTrue(nightcap.stop(stubborn.pid, timeout=0.5))
        self.assertLess(time.monotonic() - start, 3)


class RestartTest(unittest.TestCase):
    def run_restart(self, args, rejoin, game):
        with mock.patch.object(launcher.nightcap, "stop", return_value=True), \
             mock.patch.object(launcher.nightcap, "session_log", return_value=None), \
             mock.patch.object(launcher.nightcap, "current_game", return_value=game), \
             mock.patch.object(launcher.subprocess, "Popen") as popen:
            launcher.restart(123, args, rejoin)
        return popen.call_args[0][0][3:]  # after python -m nightcap_ac

    def test_rejoin_adds_the_server(self):
        game = nightcap.Game("42", "abc")
        self.assertEqual(self.run_restart([], True, game),
                         ["launch", "--launch-uri", game.uri])

    def test_no_rejoin(self):
        self.assertEqual(self.run_restart([], False, nightcap.Game("42", "abc")), ["launch"])

    def test_clicked_link_wins(self):
        self.assertEqual(self.run_restart(["roblox://join"], True, nightcap.Game("1", "2")),
                         ["launch", "roblox://join"])

    def test_refuses_when_it_will_not_close(self):
        with mock.patch.object(launcher.nightcap, "stop", return_value=False), \
             mock.patch.object(launcher.nightcap, "current_game", return_value=None), \
             mock.patch.object(launcher.subprocess, "Popen") as popen:
            with self.assertRaises(RuntimeError):
                launcher.restart(123, [], True)
        popen.assert_not_called()


class LaunchDecisionTest(unittest.TestCase):
    def decide(self, pid, hooked, args=()):
        with mock.patch.object(launcher.nightcap, "find_pid", return_value=pid), \
             mock.patch.object(launcher.nightcap, "is_hooked", return_value=hooked), \
             mock.patch.object(launcher, "send_to_app") as send, \
             mock.patch.object(launcher, "ensure_app_running") as ensure, \
             mock.patch.object(launcher, "_exec_nightcap", return_value=0) as run:
            launcher.launch(list(args))
        return send, ensure, run

    def test_hooked_just_shows_the_window(self):
        send, _, run = self.decide(10, True)
        send.assert_called_once_with("show")
        run.assert_not_called()

    def test_hooked_with_a_link_forwards_it(self):
        send, _, run = self.decide(10, True, ["roblox://x"])
        send.assert_not_called()
        self.assertEqual(run.call_args[0][0], ["roblox://x"])

    def test_unhooked_offers_a_restart(self):
        send, _, run = self.decide(10, False, ["roblox://x"])
        send.assert_called_once_with("offer-restart", {"pid": 10, "args": ["roblox://x"]})
        run.assert_not_called()

    def test_not_running_starts_both(self):
        _, ensure, run = self.decide(None, False)
        ensure.assert_called_once()
        self.assertIsNotNone(run.call_args[0][1])  # with the hook


class OpenAppTest(unittest.TestCase):
    """Opening Nightcap AC itself (the launcher entry)."""

    def open(self, pid, hooked):
        with mock.patch.object(launcher.nightcap, "find_pid", return_value=pid), \
             mock.patch.object(launcher.nightcap, "is_hooked", return_value=hooked), \
             mock.patch.object(launcher, "find_hook", return_value=Path("/x/hook.so")), \
             mock.patch.object(launcher, "nightcap_command",
                               return_value=(["mocktail"], {})) as build, \
             mock.patch.object(launcher.subprocess, "Popen") as popen:
            result = launcher.open_app()
        return result, build, popen

    def test_not_running_starts_nightcap_with_the_hook(self):
        result, build, popen = self.open(None, False)
        self.assertEqual(result, ("show", {}))
        build.assert_called_once_with([], Path("/x/hook.so"))
        popen.assert_called_once()

    def test_running_with_the_hook_just_opens_settings(self):
        result, _, popen = self.open(10, True)
        self.assertEqual(result, ("show", {}))
        popen.assert_not_called()

    def test_running_without_the_hook_offers_a_restart(self):
        result, _, popen = self.open(10, False)
        self.assertEqual(result, ("offer-restart", {"pid": 10, "args": []}))
        popen.assert_not_called()


class MainDispatchTest(unittest.TestCase):
    def test_spawned_app_keeps_its_command(self):
        from nightcap_ac import __main__ as cli
        with mock.patch("nightcap_ac.gui.run", return_value=0) as run, \
             mock.patch.object(launcher, "open_app") as open_app:
            cli.main(["--command", "offer-restart", "--payload", '{"pid": 5}'])
        run.assert_called_once_with("offer-restart", {"pid": 5})
        open_app.assert_not_called()

    def test_plain_start_runs_the_launch_logic(self):
        from nightcap_ac import __main__ as cli
        with mock.patch("nightcap_ac.gui.run", return_value=0) as run, \
             mock.patch.object(launcher, "open_app", return_value=("show", {})):
            cli.main([])
        run.assert_called_once_with("show", {})

    def test_no_launch_only_opens_settings(self):
        from nightcap_ac import __main__ as cli
        with mock.patch("nightcap_ac.gui.run", return_value=0) as run, \
             mock.patch.object(launcher, "open_app") as open_app:
            cli.main(["--no-launch"])
        run.assert_called_once_with("show", {})
        open_app.assert_not_called()


if __name__ == "__main__":
    unittest.main()
