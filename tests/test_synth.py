import struct
import unittest
import wave
from pathlib import Path

from helpers import BUILD, run, tempdir

import os

RENDER = BUILD / "nightcap-ac-render"
SR = 48000
VOICE_F0 = {"normal": 430, "peppy": 540, "cranky": 105}


def render(tmp, text, voice="normal", style="classic", pitch=0, speed=1, lang="english"):
    out = Path(tmp) / "out.wav"
    r = run([str(RENDER), str(out), text, voice, style, str(pitch), str(speed), lang], dict(os.environ))
    assert r.returncode == 0, r.stderr
    with wave.open(str(out)) as w:
        frames = w.readframes(w.getnframes())
    return [s / 32768 for s in struct.unpack(f"<{len(frames) // 2}h", frames)]


def rms(xs):
    return (sum(x * x for x in xs) / max(1, len(xs))) ** 0.5


def f0(samples):
    # Autocorrelation pitch of a steady stretch.
    mid = samples[len(samples) // 4: len(samples) // 4 + 2048]
    lo, hi = SR // 1000, SR // 80
    best, best_lag = -1e9, lo
    for lag in range(lo, hi):
        c = sum(mid[i] * mid[i + lag] for i in range(0, len(mid) - lag, 2))
        if c > best:
            best, best_lag = c, lag
    return SR / best_lag


class SynthTest(unittest.TestCase):
    def test_every_style_speaks(self):
        with tempdir() as tmp:
            for style in ("classic", "nh", "bebebese"):
                audio = render(tmp, "Hello there!", style=style)
                self.assertGreater(len(audio), SR // 4, style)
                self.assertGreater(rms(audio), 0.02, style)

    def test_speed_changes_length(self):
        with tempdir() as tmp:
            slow = render(tmp, "hello there friend", speed=0.5)
            fast = render(tmp, "hello there friend", speed=2.0)
            self.assertGreater(len(slow), 3 * len(fast))

    def test_pitch_follows_the_voice(self):
        with tempdir() as tmp:
            for voice in ("cranky", "peppy"):
                measured = f0(render(tmp, "o", voice=voice))
                self.assertAlmostEqual(measured / VOICE_F0[voice], 1.0, delta=0.15, msg=voice)

    def test_silent_input(self):
        with tempdir() as tmp:
            self.assertEqual(render(tmp, "😀 ... !!"), [])

    def test_korean_speech_speaks(self):
        with tempdir() as tmp:
            audio = render(tmp, "안녕하세요 만나서 반갑습니다", lang="korean")
            self.assertGreater(len(audio), SR // 2)
            self.assertGreater(rms(audio), 0.05)

    def test_all_authentic_voices_speak(self):
        with tempdir() as tmp:
            for voice in ("f1", "f2", "f3", "f4", "m1", "m2", "m3", "m4",
                          "normal", "peppy", "sisterly", "snooty", "jock", "lazy", "smug", "cranky"):
                audio = render(tmp, "Hello there!", voice=voice, lang="english")
                self.assertGreater(len(audio), SR // 4, voice)
                self.assertGreater(rms(audio), 0.02, voice)


if __name__ == "__main__":
    unittest.main()
