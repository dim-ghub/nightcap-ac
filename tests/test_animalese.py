import random
import unittest

import numpy as np

from nightcap_ac.animalese import (
    SR, VOICES, Settings, plan, synthesize, word_syllables,
)
from nightcap_ac.text import normalize, number_to_words


class TextTest(unittest.TestCase):
    def test_numbers_and_symbols(self):
        self.assertEqual(number_to_words(1234), "one thousand two hundred thirty four")
        self.assertEqual(normalize("I have 3 fish & 2 bugs :)"),
                         "I have three fish and two bugs :")
        self.assertEqual(normalize("café 😀 ok"), "cafe ok")


class PlanTest(unittest.TestCase):
    def test_classic_is_one_blip_per_letter(self):
        units = plan("hi bob", "classic", VOICES["normal"], random.Random(0))
        blips = [u for u in units if u.syllable is not None]
        self.assertEqual(len(blips), 5)

    def test_question_rises_at_the_end(self):
        units = plan("are you ok?", "classic", VOICES["normal"], random.Random(0))
        blips = [u for u in units if u.syllable is not None]
        self.assertGreater(blips[-1].pitch, blips[0].pitch + 2)
        self.assertGreater(blips[-1].glide, 0)

    def test_statement_falls_in_new_horizons(self):
        units = plan("this is a nice day.", "nh", VOICES["normal"], random.Random(0))
        blips = [u for u in units if u.syllable is not None]
        self.assertLess(blips[-1].glide, 0)

    def test_shouting_is_louder(self):
        units = plan("hey HEY", "classic", VOICES["normal"], random.Random(0))
        blips = [u for u in units if u.syllable is not None]
        self.assertGreater(blips[-1].gain, blips[0].gain)

    def test_syllables(self):
        self.assertEqual(len(word_syllables("banana")), 3)
        self.assertEqual([s.vowel for s in word_syllables("make")], ["e"])


class SynthTest(unittest.TestCase):
    def test_renders_audio_for_every_style(self):
        for style in ("classic", "nh", "bebebese"):
            audio = synthesize("Hello there!", Settings(style=style), seed=1)
            self.assertEqual(audio.dtype, np.float32)
            self.assertGreater(len(audio), SR // 4)
            self.assertTrue(np.isfinite(audio).all())
            self.assertGreater(np.sqrt(np.mean(audio ** 2)), 0.02)

    def test_speed_changes_length(self):
        slow = synthesize("hello there friend", Settings(speed=0.5), seed=1)
        fast = synthesize("hello there friend", Settings(speed=2.0), seed=1)
        self.assertGreater(len(slow), 3 * len(fast))

    def test_pitch_follows_voice(self):
        def f0(voice):
            audio = synthesize("o", Settings(voice=voice, style="classic"), seed=2)
            mid = audio[len(audio) // 4: len(audio) // 2].astype(np.float64)
            ac = np.correlate(mid, mid, "full")[len(mid) - 1:]
            lo, hi = SR // 1000, SR // 80
            return SR / (lo + np.argmax(ac[lo:hi]))
        self.assertAlmostEqual(f0("cranky") / VOICES["cranky"].f0, 1.0, delta=0.15)
        self.assertAlmostEqual(f0("peppy") / VOICES["peppy"].f0, 1.0, delta=0.15)

    def test_silent_input(self):
        self.assertEqual(len(synthesize("😀 ... !!", Settings())), 0)


if __name__ == "__main__":
    unittest.main()
