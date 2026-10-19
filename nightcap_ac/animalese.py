"""Animalese speech synthesis.

Animal Crossing voices are built from tiny pitched-up speech blips that play
in step with the text box, one blip per character, at a pitch that wobbles
randomly around the speaker's voice and bends up for questions. This module
reproduces that behaviour:

* ``classic``  - GameCube / Wild World / City Folk / New Leaf. One clipped
  letter sound per letter, fixed slot timing, random pitch per letter.
* ``nh``       - New Horizons. Text is split into syllables, each blip
  co-articulates its consonants with its vowel, and the sentence carries a
  falling (or, for questions, rising) melody.
* ``bebebese`` - The "Bebebese" voice option: every letter is "be".

The sounds are synthesized (harmonic voice source shaped by formant
resonances plus filtered noise for consonants), so no game audio is needed.
A folder of your own letter samples can be used instead; see ``SamplePack``.
"""

from __future__ import annotations

import math
import random
import wave
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np

from .text import normalize

SR = 48_000


# --------------------------------------------------------------------------
# Voices


@dataclass(frozen=True)
class Voice:
    label: str
    f0: float            # base pitch in Hz
    formant_scale: float  # >1 shifts the vocal tract up (the "chipmunk" part)
    cps: float           # characters per second of the text box
    jitter: float        # random pitch spread per blip, semitones
    breath: float        # aspiration noise mixed into vowels, 0..1
    tilt: float          # source spectral slope; higher is darker


VOICES: dict[str, Voice] = {
    "normal":   Voice("Normal (sweet)",   430, 1.48, 15.0, 1.3, 0.06, 1.05),
    "peppy":    Voice("Peppy",            540, 1.58, 17.5, 2.1, 0.05, 0.95),
    "snooty":   Voice("Snooty",           390, 1.40, 13.5, 0.9, 0.08, 1.10),
    "sisterly": Voice("Sisterly",         300, 1.30, 14.5, 1.1, 0.10, 1.10),
    "lazy":     Voice("Lazy",             205, 1.18, 11.5, 0.8, 0.07, 1.20),
    "jock":     Voice("Jock",             235, 1.22, 16.5, 1.7, 0.06, 1.05),
    "smug":     Voice("Smug",             215, 1.22, 13.5, 1.0, 0.06, 1.10),
    "cranky":   Voice("Cranky",           150, 1.08, 12.0, 0.7, 0.09, 1.30),
}

STYLES = {
    "classic": "Classic (GameCube to New Leaf)",
    "nh": "New Horizons",
    "bebebese": "Bebebese",
}


# --------------------------------------------------------------------------
# Phones
#
# Formant values are adult reference values (Hz); the voice's formant_scale
# lifts them toward a small animal's vocal tract.

VOWELS: dict[str, tuple[tuple[float, float, float], tuple[float, float, float] | None]] = {
    "i":  ((280, 2250, 2900), None),
    "I":  ((400, 1950, 2550), None),
    "e":  ((450, 2050, 2650), (330, 2200, 2800)),   # "ay", glides to ee
    "E":  ((550, 1800, 2500), None),                # "eh"
    "ae": ((680, 1700, 2400), None),                # "a" in cat
    "a":  ((750, 1150, 2450), None),                # "ah"
    "A":  ((640, 1200, 2400), None),                # "uh"
    "o":  ((520, 900, 2400), (420, 980, 2300)),     # "oh", glides to oo
    "u":  ((320, 900, 2250), None),
    "U":  ((430, 1050, 2300), None),
    "ai": ((730, 1250, 2500), (400, 1950, 2600)),
    "au": ((730, 1200, 2450), (420, 950, 2300)),
    "oi": ((520, 900, 2400), (400, 1950, 2600)),
    "@":  ((520, 1450, 2500), None),                # schwa
}


@dataclass(frozen=True)
class Consonant:
    kind: str                     # stop, fric, affric, nasal, approx, aspir
    voiced: bool
    band: tuple[float, float] = (0, 0)   # noise band for bursts / frication
    locus: tuple[float, float, float] = (300, 1500, 2500)
    noise: float = 0.5            # noise level relative to the vowel
    ms: float = 35                # nominal length at normal speed


CONSONANTS: dict[str, Consonant] = {
    "b":  Consonant("stop", True, (200, 1600), (250, 900, 2300), 0.35),
    "p":  Consonant("stop", False, (200, 1600), (250, 900, 2300), 0.55),
    "d":  Consonant("stop", True, (2500, 6500), (250, 1700, 2700), 0.35),
    "t":  Consonant("stop", False, (3000, 8000), (250, 1700, 2700), 0.6),
    "g":  Consonant("stop", True, (1300, 3200), (250, 2100, 2400), 0.35),
    "k":  Consonant("stop", False, (1300, 3600), (250, 2100, 2400), 0.6),
    "s":  Consonant("fric", False, (4500, 11000), (300, 1700, 2700), 0.55, 45),
    "z":  Consonant("fric", True, (4500, 11000), (300, 1700, 2700), 0.35, 40),
    "S":  Consonant("fric", False, (2000, 6500), (300, 1900, 2500), 0.55, 45),
    "Z":  Consonant("fric", True, (2000, 6500), (300, 1900, 2500), 0.35, 40),
    "f":  Consonant("fric", False, (1200, 9000), (300, 1000, 2300), 0.3, 35),
    "v":  Consonant("fric", True, (1200, 9000), (300, 1000, 2300), 0.2, 30),
    "T":  Consonant("fric", False, (1500, 9000), (300, 1500, 2600), 0.22, 35),
    "D":  Consonant("fric", True, (1500, 9000), (300, 1500, 2600), 0.15, 25),
    "h":  Consonant("aspir", False, noise=0.32, ms=30),
    "tS": Consonant("affric", False, (2000, 6500), (300, 1900, 2500), 0.55, 45),
    "dZ": Consonant("affric", True, (2000, 6500), (300, 1900, 2500), 0.4, 40),
    "m":  Consonant("nasal", True, locus=(250, 1000, 2200), ms=35),
    "n":  Consonant("nasal", True, locus=(250, 1500, 2500), ms=35),
    "N":  Consonant("nasal", True, locus=(250, 2000, 2500), ms=35),
    "l":  Consonant("approx", True, locus=(360, 1050, 2800), ms=30),
    "r":  Consonant("approx", True, locus=(420, 1250, 1650), ms=30),
    "w":  Consonant("approx", True, locus=(300, 650, 2200), ms=30),
    "y":  Consonant("approx", True, locus=(270, 2250, 3000), ms=28),
}


@dataclass(frozen=True)
class Syllable:
    onset: tuple[str, ...]
    vowel: str
    coda: tuple[str, ...] = ()


# Classic Animalese plays each letter as its clipped letter name.
LETTER_SYLLABLES: dict[str, Syllable] = {
    "a": Syllable((), "e"),          "b": Syllable(("b",), "i"),
    "c": Syllable(("s",), "i"),      "d": Syllable(("d",), "i"),
    "e": Syllable((), "i"),          "f": Syllable((), "E", ("f",)),
    "g": Syllable(("dZ",), "i"),     "h": Syllable((), "e", ("tS",)),
    "i": Syllable((), "ai"),         "j": Syllable(("dZ",), "e"),
    "k": Syllable(("k",), "e"),      "l": Syllable((), "E", ("l",)),
    "m": Syllable((), "E", ("m",)),  "n": Syllable((), "E", ("n",)),
    "o": Syllable((), "o"),          "p": Syllable(("p",), "i"),
    "q": Syllable(("k", "y"), "u"),  "r": Syllable((), "a", ("r",)),
    "s": Syllable((), "E", ("s",)),  "t": Syllable(("t",), "i"),
    "u": Syllable(("y",), "u"),      "v": Syllable(("v",), "i"),
    "w": Syllable(("d",), "A"),      "x": Syllable((), "E", ("k", "s")),
    "y": Syllable(("w",), "ai"),     "z": Syllable(("z",), "i"),
}

BE = Syllable(("b",), "E")


# --------------------------------------------------------------------------
# New Horizons style: rough English grapheme-to-syllable conversion. It does
# not need to be correct English, only to give each syllable the consonant
# and vowel colour you would expect from the spelling.

_VOWEL_GRAPHEMES = [
    ("eau", "o"), ("igh", "ai"),
    ("ee", "i"), ("ea", "i"), ("ie", "ai"), ("ei", "e"), ("ey", "e"),
    ("ai", "e"), ("ay", "e"), ("oo", "u"), ("ou", "au"), ("ow", "o"),
    ("oi", "oi"), ("oy", "oi"), ("oa", "o"), ("au", "a"), ("aw", "a"),
    ("ue", "u"), ("ew", "u"), ("ui", "u"),
    ("a", "ae"), ("e", "E"), ("i", "I"), ("o", "a"), ("u", "A"),
]
_LONG = {"ae": "e", "I": "ai", "a": "o", "A": "u", "E": "i"}
_CONSONANT_GRAPHEMES = [
    ("tch", ("tS",)), ("sch", ("s", "k")),
    ("sh", ("S",)), ("ch", ("tS",)), ("th", ("T",)), ("ph", ("f",)),
    ("wh", ("w",)), ("ck", ("k",)), ("ng", ("N",)), ("qu", ("k", "w")),
    ("gh", ()), ("x", ("k", "s")), ("j", ("dZ",)), ("q", ("k",)),
    ("b", ("b",)), ("d", ("d",)), ("f", ("f",)), ("h", ("h",)),
    ("k", ("k",)), ("l", ("l",)), ("m", ("m",)), ("n", ("n",)),
    ("p", ("p",)), ("r", ("r",)), ("s", ("s",)), ("t", ("t",)),
    ("v", ("v",)), ("w", ("w",)), ("z", ("z",)),
]


def _word_phones(word: str) -> list[str]:
    """Spelling to a flat phone list; vowels are VOWELS keys."""
    w = word.lower()
    if w.startswith("kn") or w.startswith("wr") or w.startswith("gn"):
        w = w[1:]
    silent_e = len(w) >= 3 and w.endswith("e") and w[-2] not in "aeiouy" \
        and any(c in "aeiouy" for c in w[:-2])
    if silent_e:
        w = w[:-1]
    phones: list[str] = []
    i = 0
    while i < len(w):
        c = w[i]
        if i > 0 and c == w[i - 1] and c not in "aeiou":
            i += 1  # doubled consonant
            continue
        if c == "y":
            follows_vowel = i + 1 < len(w) and w[i + 1] in "aeiou"
            if i == 0 or follows_vowel:
                phones.append("y")
            else:
                phones.append("i" if i == len(w) - 1 else "I")
            i += 1
            continue
        if c == "c":
            nxt = w[i + 1] if i + 1 < len(w) else ""
            if nxt == "h":
                phones.append("tS")
                i += 2
            elif nxt == "k":
                phones.append("k")
                i += 2
            else:
                phones.append("s" if nxt in ("e", "i", "y") else "k")
                i += 1
            continue
        if c == "g" and w[i + 1:i + 2] in ("e", "i", "y") and w[i + 1:i + 2] != "":
            phones.append("dZ")
            i += 1
            continue
        for graph, vowel in _VOWEL_GRAPHEMES:
            if w.startswith(graph, i):
                if graph == "o" and i == len(w) - 1:
                    vowel = "o"  # go, hello
                elif graph == "e" and i == len(w) - 1 and not silent_e:
                    vowel = "i"  # me, we, be
                phones.append(vowel)
                i += len(graph)
                break
        else:
            for graph, cons in _CONSONANT_GRAPHEMES:
                if w.startswith(graph, i):
                    phones.extend(cons)
                    i += len(graph)
                    break
            else:
                i += 1
    if silent_e:
        for j in range(len(phones) - 1, -1, -1):
            if phones[j] in VOWELS:
                phones[j] = _LONG.get(phones[j], phones[j])
                break
    return phones


def word_syllables(word: str) -> list[Syllable]:
    phones = _word_phones(word)
    if not any(p in VOWELS for p in phones):
        # "brb", "hmm", "psst": no vowel to babble, so read the letters.
        return [LETTER_SYLLABLES[c] for c in word.lower() if c in LETTER_SYLLABLES]
    syllables: list[Syllable] = []
    pending: list[str] = []
    for p in phones:
        if p in VOWELS:
            # Keep at most two consonants as the onset; extras close the
            # previous syllable.
            onset = pending[-2:] if len(pending) > 1 else pending
            spill = pending[:len(pending) - len(onset)]
            if spill and syllables:
                last = syllables[-1]
                syllables[-1] = replace(last, coda=last.coda + tuple(spill))
            syllables.append(Syllable(tuple(onset), p))
            pending = []
        else:
            pending.append(p)
    if pending:
        last = syllables[-1]
        syllables[-1] = replace(last, coda=last.coda + tuple(pending))
    return syllables


# --------------------------------------------------------------------------
# Utterance plan


@dataclass
class Unit:
    syllable: Syllable | None   # None is a pause
    slots: float                # length in text-box character slots
    pitch: float = 0.0          # semitones relative to the voice
    glide: float = 0.0          # pitch bend across the blip, semitones
    gain: float = 1.0
    letter: str = ""            # source letter, for sample packs


_PAUSE_SLOTS = {",": 3.0, ";": 3.0, ":": 3.0, "-": 1.0,
                ".": 5.0, "!": 5.0, "?": 5.0, "\n": 5.0}


@dataclass
class _Word:
    text: str
    shout: bool


@dataclass
class _Sentence:
    words: list[_Word]
    end: str          # ".", "!", "?", or "" when the message just stops
    pause: float      # slots of silence after the sentence


def _sentences(text: str) -> list[_Sentence]:
    sentences: list[_Sentence] = []
    words: list[_Word] = []
    word = ""

    def flush_word() -> None:
        nonlocal word
        if word:
            shout = len(word) >= 2 and word.isupper()
            words.append(_Word(word, shout))
            word = ""

    i = 0
    while i < len(text):
        c = text[i]
        if c.isalpha():
            word += c
        elif c == " ":
            flush_word()
        elif c in _PAUSE_SLOTS:
            flush_word()
            run = c
            while i + 1 < len(text) and text[i + 1] in _PAUSE_SLOTS:
                i += 1
                run += text[i]
            if "?" in run:
                end = "?"
            elif "!" in run:
                end = "!"
            elif "." in run or "\n" in run:
                end = "."
            else:
                end = ""
            pause = max(_PAUSE_SLOTS[ch] for ch in run)
            if run.count(".") >= 3:
                pause = 9.0  # ellipsis: the long thinking pause
            if end:
                sentences.append(_Sentence(words, end, pause))
                words = []
            elif words:
                # Comma-style pause inside a sentence, kept as a marker word.
                words.append(_Word(f"\0{pause}", False))
        i += 1
    flush_word()
    if words:
        sentences.append(_Sentence(words, "", 0.0))
    return sentences


def plan(text: str, style: str, voice: Voice, rng: random.Random) -> list[Unit]:
    """Turn chat text into a list of blips and pauses with prosody."""
    units: list[Unit] = []
    for sentence in _sentences(normalize(text)):
        sentence_units: list[tuple[Unit, int]] = []  # unit, word index
        for wi, word in enumerate(sentence.words):
            if word.text.startswith("\0"):
                units_pause = float(word.text[1:])
                sentence_units.append((Unit(None, units_pause), wi))
                continue
            if sentence_units and sentence_units[-1][0].syllable is not None:
                gap = 0.8 if style != "nh" else 0.45
                sentence_units.append((Unit(None, gap), -1))
            if style == "nh":
                syls = word_syllables(word.text)
                for s in syls:
                    weight = 1.0 + 0.3 * (len(s.onset) + len(s.coda))
                    sentence_units.append((Unit(s, 1.3 * weight), wi))
            else:
                for ch in word.text:
                    s = BE if style == "bebebese" else LETTER_SYLLABLES.get(ch.lower())
                    if s is not None:
                        sentence_units.append((Unit(s, 1.0, letter=ch.lower()), wi))
        _prosody(sentence, sentence_units, style, voice, rng)
        units.extend(u for u, _ in sentence_units)
        if sentence.pause:
            units.append(Unit(None, sentence.pause))
    while units and units[-1].syllable is None:
        units.pop()
    return units


def _prosody(sentence: _Sentence, items: list[tuple[Unit, int]], style: str,
             voice: Voice, rng: random.Random) -> None:
    voiced = [(u, wi) for u, wi in items if u.syllable is not None]
    if not voiced:
        return
    n = len(voiced)
    last_word = voiced[-1][1]
    tail = [u for u, wi in voiced if wi == last_word]
    for idx, (u, wi) in enumerate(voiced):
        jitter = rng.uniform(-voice.jitter, voice.jitter)
        if style == "nh":
            # Gentle declination over the sentence, smaller random wobble.
            pos = idx / max(1, n - 1)
            u.pitch = 1.2 - 2.4 * pos + 0.6 * jitter
            u.glide = rng.uniform(-0.6, 0.3)
        else:
            u.pitch = jitter
            u.glide = -0.5  # clipped letter samples droop slightly
        if sentence.words[wi].shout:
            u.pitch += 2.5
            u.gain *= 1.3
        if sentence.end == "!":
            u.pitch += 1.8
            u.gain *= 1.25
    if sentence.end == "?":
        # Questions climb over the last word and bend up at the very end.
        for k, u in enumerate(tail):
            u.pitch += 1.5 + 3.5 * (k + 1) / len(tail)
        tail[-1].glide = 4.0 if style == "nh" else 2.0
        if style == "nh":
            tail[-1].slots *= 1.35
    elif style == "nh":
        tail[-1].glide = -2.5 if sentence.end != "!" else -1.0
        tail[-1].slots *= 1.25


# --------------------------------------------------------------------------
# Rendering


def _resonance(f: np.ndarray, fc: np.ndarray, bw: np.ndarray) -> np.ndarray:
    """Magnitude of a two-pole resonator, normalized to 1 at DC."""
    b = bw / 2.0
    num = fc * fc + b * b
    den = np.sqrt(((f - fc) ** 2 + b * b) * ((f + fc) ** 2 + b * b))
    return num / den


def _formant_gain(f: np.ndarray, formants: np.ndarray, scale: float) -> np.ndarray:
    """Cascade formant envelope at frequencies f. formants: (3, ...) arrays."""
    bw_scale = math.sqrt(scale)
    gain = _resonance(f, formants[0], 80.0 * bw_scale)
    gain = gain * _resonance(f, formants[1], 110.0 * bw_scale)
    gain = gain * _resonance(f, formants[2], 170.0 * bw_scale)
    gain = gain * _resonance(f, np.float64(3500.0 * scale), np.float64(250.0 * bw_scale))
    gain = gain * _resonance(f, np.float64(4600.0 * scale), np.float64(320.0 * bw_scale))
    return gain


def _shaped_noise(n: int, rng: np.random.Generator, shape) -> np.ndarray:
    if n <= 0:
        return np.zeros(0)
    size = max(64, 1 << (n - 1).bit_length())
    spectrum = np.fft.rfft(rng.standard_normal(size))
    freqs = np.fft.rfftfreq(size, 1.0 / SR)
    spectrum *= shape(freqs)
    out = np.fft.irfft(spectrum, size)[:n]
    rms = np.sqrt(np.mean(out * out)) or 1.0
    return out / rms


def _band(lo: float, hi: float):
    def shape(freqs: np.ndarray) -> np.ndarray:
        edge = 0.25
        lo_ramp = np.clip((freqs - lo * (1 - edge)) / (lo * edge * 2 + 1), 0, 1)
        hi_ramp = np.clip((hi * (1 + edge) - freqs) / (hi * edge * 2 + 1), 0, 1)
        return lo_ramp * hi_ramp
    return shape


@dataclass
class _Track:
    """Piecewise-linear control tracks for one blip."""
    times: list[float]
    formants: list[tuple[float, float, float]]
    voice: list[float]


def _blip_tracks(syl: Syllable, dur: float, rate: float):
    """Lay out consonant/vowel timing; returns tracks and noise events.

    rate < 1 compresses consonants so short blips stay mostly vowel, which is
    what makes Animalese sound clipped rather than slurred.
    """
    v_start, v_glide = VOWELS[syl.vowel]
    v_end = v_glide or v_start
    tr = _Track([0.0], [], [0.0])
    noise: list[tuple[float, float, str, Consonant, tuple[float, float, float]]] = []
    t = 0.0
    first = syl.onset[0] if syl.onset else None
    start_f = CONSONANTS[first].locus if first and CONSONANTS[first].kind != "aspir" else v_start
    tr.formants.append(start_f)

    def at(time: float, formants, voice: float) -> None:
        tr.times.append(time)
        tr.formants.append(formants)
        tr.voice.append(voice)

    for name in syl.onset:
        c = CONSONANTS[name]
        length = c.ms / 1000.0 * rate
        mix = tuple(0.55 * l + 0.45 * v for l, v in zip(c.locus, v_start))
        if c.kind == "stop":
            closure = 0.006 * rate
            at(t + 0.002, c.locus, 0.12 if c.voiced else 0.0)
            at(t + closure, c.locus, 0.12 if c.voiced else 0.0)
            burst = 0.006 * rate
            noise.append((t + closure, burst, "band", c, v_start))
            t += closure + burst
            if not c.voiced:
                asp = 0.016 * rate
                noise.append((t, asp, "formant", c, mix))
                at(t, mix, 0.0)
                t += asp
            at(t + 0.004, mix, 1.0)
            t += 0.004
        elif c.kind in ("fric", "affric"):
            if c.kind == "affric":
                closure = 0.006 * rate
                at(t + closure, c.locus, 0.1 if c.voiced else 0.0)
                t += closure
            at(t + 0.003, c.locus, 0.35 if c.voiced else 0.0)
            noise.append((t, length, "band", c, v_start))
            t += length
            at(t, c.locus, 0.35 if c.voiced else 0.0)
            at(t + 0.004, mix, 1.0)
            t += 0.004
        elif c.kind == "aspir":
            noise.append((t, length, "formant", c, v_start))
            at(t + length, v_start, 0.0)
            t += length
            at(t + 0.004, v_start, 1.0)
            t += 0.004
        else:  # nasal / approximant: voiced, formants glide into the vowel
            level = 0.55 if c.kind == "nasal" else 0.75
            at(t + 0.004, c.locus, level)
            at(t + length, c.locus, level)
            t += length
    # Vowel, with formant transitions out of the onset.
    coda_len = sum(CONSONANTS[c].ms for c in syl.coda) / 1000.0 * rate * 0.6
    coda_len = min(coda_len, 0.35 * dur)
    v_len = max(0.02, dur - t - coda_len)
    at(t + min(0.025, v_len * 0.4), v_start, 1.0)
    at(t + v_len, v_end, 1.0)
    t += v_len
    for name in syl.coda:
        c = CONSONANTS[name]
        length = coda_len / len(syl.coda)
        if c.kind in ("fric", "affric", "stop", "aspir"):
            at(t + 0.005, c.locus if c.kind != "aspir" else v_end,
               0.3 if c.voiced else 0.0)
            noise.append((t, length, "band" if c.kind != "aspir" else "formant", c, v_end))
        else:
            at(t + length, c.locus, 0.6)
        t += length
    at(t + 0.004, tr.formants[-1], 0.0)
    return tr, noise, t + 0.004


def render_blip(syl: Syllable, dur: float, f0: float, glide: float,
                voice: Voice, gain: float, rng: np.random.Generator,
                scale_mult: float = 1.0) -> np.ndarray:
    rate = min(1.0, max(0.45, dur / 0.085))
    tracks, noise_events, length = _blip_tracks(syl, dur, rate)
    n = int(length * SR)
    t = np.arange(n) / SR
    times = np.array(tracks.times)
    order = np.argsort(times, kind="stable")
    times = times[order]
    scale = voice.formant_scale * scale_mult
    formants = np.array(tracks.formants)[order].T * scale
    form_t = np.stack([np.interp(t, times, f) for f in formants])
    amp_t = np.interp(t, times, np.array(tracks.voice)[order])

    # Voice: harmonic sum with a formant-shaped spectrum.
    f0_t = f0 * 2.0 ** (glide * (t / max(length, 1e-6)) / 12.0)
    f0_t *= 1.0 + 0.004 * np.sin(2 * np.pi * 5.5 * t + rng.uniform(0, 6.28))
    phase = 2 * np.pi * np.cumsum(f0_t) / SR
    nh = int(min(80, 10_500.0 / f0_t.min()))  # nothing survives the low-pass above
    k = np.arange(1, nh + 1)[:, None]
    freqs = k * f0_t[None, :]
    amps = _formant_gain(freqs, form_t[:, None, :], scale) / k ** voice.tilt
    amps *= freqs < 0.45 * SR
    voiced = np.sum(amps * np.sin(k * phase[None, :]), axis=0)
    loud = amp_t > 0.5
    ref = np.sqrt(np.mean(voiced[loud] ** 2)) if loud.any() else 1.0
    voiced = voiced / (ref or 1.0) * amp_t * 0.25

    out = voiced
    if voice.breath > 0:
        breath = _shaped_noise(n, rng, lambda f: _formant_gain(
            f, form_t[:, n // 2][:, None] * np.ones((3, 1)), scale) * (f > 400))
        out = out + breath * amp_t * 0.25 * voice.breath

    for start, seg, mode, cons, target in noise_events:
        i0 = int(start * SR)
        m = max(1, int(seg * SR))
        if i0 >= n:
            continue
        m = min(m, n - i0)
        if mode == "band":
            lo, hi = cons.band
            shape = _band(lo * min(scale, 1.25), min(hi * min(scale, 1.25), 0.47 * SR))
        else:
            tgt = np.array(target)[:, None] * scale
            shape = lambda f, tgt=tgt: _formant_gain(f, tgt, scale) * (f > 300)
        seg_noise = _shaped_noise(m, rng, shape)
        env = np.ones(m)
        ramp = min(m // 2, int(0.003 * SR))
        if ramp > 0:
            env[:ramp] = np.linspace(0, 1, ramp)
            env[-ramp:] = np.linspace(1, 0, ramp)
        out[i0:i0 + m] += seg_noise * env * 0.25 * cons.noise

    fade_in = min(n, int(0.003 * SR))
    fade_out = min(n, int(0.010 * SR))
    if fade_in:
        out[:fade_in] *= np.linspace(0, 1, fade_in)
    if fade_out:
        out[-fade_out:] *= np.linspace(1, 0, fade_out) ** 1.5
    return out * gain


# --------------------------------------------------------------------------
# Optional sample packs


class SamplePack:
    """Your own per-letter recordings, played back Animalese style.

    Point it at a folder holding ``a.wav`` ... ``z.wav``, or at a single WAV
    containing the 26 letters back to back in equal slices (the layout used
    by the Animalese.js sound sprite). Each blip plays the start of the
    letter's sample, resampled for pitch, exactly as the games do.
    """

    def __init__(self, path: str | Path):
        path = Path(path).expanduser()
        self.letters: dict[str, np.ndarray] = {}
        if path.is_dir():
            for letter in "abcdefghijklmnopqrstuvwxyz":
                f = path / f"{letter}.wav"
                if f.exists():
                    self.letters[letter] = _read_wav(f)
        elif path.is_file():
            data = _read_wav(path)
            size = len(data) // 26
            for i, letter in enumerate("abcdefghijklmnopqrstuvwxyz"):
                self.letters[letter] = data[i * size:(i + 1) * size]
        if not self.letters:
            raise ValueError(f"no letter samples found in {path}")
        peak = max(float(np.max(np.abs(s))) for s in self.letters.values()) or 1.0
        for k in self.letters:
            self.letters[k] = self.letters[k] / peak

    def blip(self, letter: str, dur: float, rate: float, gain: float) -> np.ndarray:
        sample = self.letters.get(letter)
        if sample is None:
            sample = self.letters.get("e", next(iter(self.letters.values())))
        n = int(dur * SR)
        pos = np.arange(n) * rate
        pos = pos[pos < len(sample) - 1]
        out = np.interp(pos, np.arange(len(sample)), sample)
        fade = min(len(out), int(0.008 * SR))
        if fade:
            out[-fade:] *= np.linspace(1, 0, fade)
        return out * gain * 0.8


def _read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        width, channels, rate = w.getsampwidth(), w.getnchannels(), w.getframerate()
        raw = w.readframes(w.getnframes())
    if width == 1:
        data = (np.frombuffer(raw, np.uint8).astype(np.float64) - 128) / 128
    elif width == 2:
        data = np.frombuffer(raw, "<i2") / 32768.0
    elif width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        data = ((b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) << 8 >> 8) / 8388608.0
    else:
        data = np.frombuffer(raw, "<i4") / 2147483648.0
    data = data.reshape(-1, channels).mean(axis=1)
    if rate != SR:
        pos = np.arange(int(len(data) * SR / rate)) * (rate / SR)
        data = np.interp(pos, np.arange(len(data)), data)
    return data


# --------------------------------------------------------------------------
# Public entry point


@dataclass
class Settings:
    voice: str = "normal"
    style: str = "classic"
    pitch: float = 0.0     # semitones on top of the voice
    speed: float = 1.0     # multiplier on the text-box speed
    sample_pack: str = ""


def synthesize(text: str, settings: Settings, seed: int | None = None,
               pack: SamplePack | None = None) -> np.ndarray:
    """Render chat text to mono float32 audio at SR."""
    voice = VOICES.get(settings.voice, VOICES["normal"])
    pyrng = random.Random(seed)
    nprng = np.random.default_rng(seed)
    units = plan(text, settings.style, voice, pyrng)
    if not units:
        return np.zeros(0, np.float32)
    slot = 1.0 / (voice.cps * max(0.25, settings.speed))
    total = sum(u.slots for u in units) * slot + 0.3
    out = np.zeros(int(total * SR) + SR // 10)
    cursor = 0.0
    for u in units:
        dur = u.slots * slot
        if u.syllable is not None:
            semis = settings.pitch + u.pitch
            blip_dur = dur * 1.12  # neighbouring blips overlap a little
            if pack is not None:
                rate = 2.0 ** (semis / 12.0) * (voice.f0 / 300.0)
                b = pack.blip(u.letter or "e", blip_dur, rate, u.gain)
            else:
                f0 = voice.f0 * 2.0 ** (semis / 12.0)
                # Pitching up a recording also lifts its formants; follow
                # the pitch a little so high blips get brighter, like the
                # resampled samples in the games.
                b = render_blip(u.syllable, blip_dur, f0, u.glide, voice,
                                u.gain, nprng, 2.0 ** (u.pitch / 12.0 * 0.35))
            i0 = int(cursor * SR)
            end = min(len(out), i0 + len(b))
            out[i0:end] += b[:end - i0]
        cursor += dur
    out = out[:int((cursor + 0.08) * SR)]
    out = _lowpass(out, 9500.0)
    peak = float(np.max(np.abs(out))) or 1.0
    return (out / peak * 0.85).astype(np.float32)


def _lowpass(x: np.ndarray, cutoff: float) -> np.ndarray:
    """Gentle FFT low-pass: the games' 32 kHz samples have no air on top."""
    if len(x) == 0:
        return x
    spectrum = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(len(x), 1.0 / SR)
    spectrum *= 1.0 / np.sqrt(1.0 + (freqs / cutoff) ** 8)
    return np.fft.irfft(spectrum, len(x))
