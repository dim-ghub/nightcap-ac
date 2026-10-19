"""Chat text normalization.

Animalese only "speaks" letters, so everything else in a chat message is
turned into letters (numbers, symbols) or into pauses (punctuation) before
the synthesizer sees it.
"""

from __future__ import annotations

import re
import unicodedata

_ONES = [
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
    "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
    "sixteen", "seventeen", "eighteen", "nineteen",
]
_TENS = ["", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
         "eighty", "ninety"]
_SCALES = [(1_000_000_000, "billion"), (1_000_000, "million"),
           (1_000, "thousand"), (100, "hundred")]

_SYMBOLS = {
    "&": " and ", "@": " at ", "%": " percent ", "+": " plus ", "=": " equals ",
    "#": " number ", "$": " dollars ", "<3": " heart ",
}

# Pause punctuation is kept for the prosody pass; everything else that is not
# a letter, digit or space is dropped.
PAUSE_CHARS = ",.;:!?-\n"


def number_to_words(n: int) -> str:
    if n < 20:
        return _ONES[n]
    if n < 100:
        tens, ones = divmod(n, 10)
        return _TENS[tens] + ("" if ones == 0 else " " + _ONES[ones])
    for value, name in _SCALES:
        if n >= value:
            head, rest = divmod(n, value)
            words = number_to_words(head) + " " + name
            return words if rest == 0 else words + " " + number_to_words(rest)
    raise AssertionError(n)


def _spell_number(match: re.Match[str]) -> str:
    digits = match.group(0)
    # Long digit runs (codes, ids) read digit by digit, like a phone number.
    if len(digits) > 9 or (len(digits) > 1 and digits.startswith("0")):
        return " " + " ".join(_ONES[int(d)] for d in digits) + " "
    return " " + number_to_words(int(digits)) + " "


def normalize(text: str) -> str:
    """Return text containing only ASCII letters, spaces and pause marks."""
    for symbol, words in _SYMBOLS.items():
        text = text.replace(symbol, words)
    text = unicodedata.normalize("NFKD", text)
    text = "".join(c for c in text if not unicodedata.combining(c))
    text = text.replace("…", "...")
    text = re.sub(r"\d+", _spell_number, text)
    out = []
    for c in text:
        if c.isascii() and c.isalpha():
            out.append(c)
        elif c in PAUSE_CHARS:
            out.append(c)
        elif c.isspace() or c in "_/\\|":
            out.append(" ")
        # Anything else (emoji, brackets, quotes) is silent, like in-game.
    collapsed = re.sub(r"[ \t]+", " ", "".join(out))
    return collapsed.strip()
