# Nightcap AC

Every chat message you send in Roblox (through [Nightcap](https://github.com/CoderDayton/nightcap))
is spoken into voice chat as Animal Crossing–style Animalese. You can also
hear it on your own speakers.

Nightcap AC is a separate program. It needs no changes to Nightcap: a small
preload library watches Nightcap's text boxes, and a PipeWire virtual
microphone carries the voice into Roblox voice chat.

## How it works

```
 Nightcap (unmodified)                         nightcap-ac
 ┌──────────────────────────────┐   socket    ┌──────────────────────────────┐
 │ libSDL3  ◄── libnightcap_ac_ │ ──────────► │ chat tracker → Animalese     │
 │              hook.so         │  box open/  │ synth → player               │
 │ mic stream ◄──────────┐      │  text/Enter └──────────┬───────────────────┘
 └───────────────────────│──────┘                        │ speech
                         │        PipeWire               ▼
                  "Nightcap AC microphone" ◄── nightcap_ac_mix ◄── your real mic
```

* **Chat capture.** `libnightcap_ac_hook.so` is loaded with `LD_PRELOAD`. It
  wraps a few SDL3 functions Nightcap already calls. When a Roblox text box
  gets focus, Nightcap calls `SDL_StartTextInputWithProperties`, and the hook
  then reports typed text and editing keys until the box closes. Pressing
  Enter sends the message. Nothing is reported while no box is focused, and
  password boxes are never reported. The hook only observes: every call goes
  through to SDL unchanged.
* **Voice chat.** The app creates a virtual mic that mixes your real mic with
  the speech, then moves *only Nightcap's* recording stream onto it. Discord
  and other apps keep your normal mic. Quitting the app puts Nightcap back on
  your real mic and removes the virtual devices.

## Requirements

* Linux with PipeWire (`pactl`, `pw-cat`)
* Python 3.11+ with `numpy` and `PySide6` (`python-numpy`, `pyside6` on Arch)
* A C compiler and SDL3 headers to build the hook (`sdl3` on Arch)
* Nightcap, native install (`/usr/bin/mocktail`) or Flatpak

## Install

```bash
make
make install          # into ~/.local (PREFIX=... to change)
```

This adds two launcher entries: **Nightcap AC** (the settings window) and
**Nightcap (Animalese chat)** (Nightcap started with the hook).

To run from the source tree without installing:

```bash
make
./scripts/nightcap-ac            # settings window
./scripts/nightcap-ac launch     # start Nightcap with the hook
```

## Use

1. Start Nightcap through **Nightcap (Animalese chat)**, the **Launch
   Nightcap** button, or `nightcap-ac launch`. The settings window opens
   too if it isn't running yet.
2. Join a game with voice chat and unmute your mic in Roblox.
3. Chat as usual (`/` or click the chat bar, type, press Enter). The message
   plays in voice chat as Animalese.

Nightcap only lets one copy run at a time. If Nightcap is already open
without the hook, close it first, or `launch` just forwards to the open copy.

### Settings

| Setting | What it does |
| --- | --- |
| **Play the voice to** | *Voice chat only*, *Voice chat and my speakers*, or *My speakers only* (handy for trying voices) |
| **Personality** | Normal, Peppy, Snooty, Sisterly, Lazy, Jock, Smug, Cranky. Each has its own pitch, speed and timbre |
| **Game** | *Classic*: one clipped letter sound per letter, like GameCube through New Leaf. *New Horizons*: syllable babble with a sentence melody. *Bebebese*: every letter is "be" |
| **Pitch / Speed** | Shift the voice up or down by up to 12 semitones; speed from 50 to 200% |
| **VC / Speaker volume** | Separate levels for voice chat and for your speakers |
| **Letter samples** | Use your own letter recordings instead of the built-in voice (Classic only, see below) |
| **Keep my real microphone** | Off: voice chat hears only the Animalese. On: your mic is mixed in too |
| **Don't speak chat commands** | Skips messages starting with `/` (`/e dance`, `/w name`, ...) |

Settings save to `~/.config/nightcap-ac/config.json` and apply immediately.

### How it imitates Animal Crossing

* Each letter (Classic) or syllable (New Horizons) becomes a short blip,
  timed like the text box printing characters.
* Every blip gets a random pitch around the speaker's voice, so the same
  text never sounds exactly the same twice.
* `?` makes the last word climb and bend upward. `!` raises and boosts the
  sentence. ALL-CAPS words are shouted.
* Commas, periods and `...` add the matching pauses. Numbers and symbols are
  read out (`3` becomes "three", `&` becomes "and"). Emoji are silent.
* The built-in voices are synthesized: a harmonic voice through vocal-tract
  formants, with noise bursts for consonants. No game audio is included.

### Letter sample packs

For the exact in-game sound, point **Letter samples** at either:

* a folder with `a.wav` ... `z.wav`, or
* a single WAV with all 26 letters back to back in equal slices (the
  Animalese.js sound-sprite layout).

Each letter plays its sample's start, resampled for pitch, the way the games
play their voice banks. Packs are not included; use recordings you have the
rights to.

### Command line

```bash
nightcap-ac --headless                          # no window
nightcap-ac say "Hello there!" --style nh       # try a voice on your speakers
nightcap-ac say "Hi!" --voice cranky --wav hi.wav
nightcap-ac launch --flatpak                    # force the Flatpak build
```

## Flatpak Nightcap

`nightcap-ac launch` uses the Flatpak when there is no native `mocktail`.
It passes the hook, the socket folder and `LD_PRELOAD` through
`flatpak run` options, so no permanent overrides are added.

## Limitations

* The hook can't tell the chat bar from a game's own single-line text box,
  so text sent with Enter from any text box is spoken. Password boxes are
  always skipped.
* Roblox's voice processing (noise suppression, auto-gain) still applies, so
  very quiet volumes may be cut.
* If the app crashes, the virtual devices are cleaned up the next time it
  starts. To remove them by hand:
  `pactl list modules short | grep nightcap_ac`, then `pactl unload-module <id>`.

## Development

```bash
make test     # builds the hook, then runs the unit and hook tests
```

`tests/hook_driver.c` is a small SDL3 program that drives the preload hook
the way Nightcap does. The tests check that keystrokes are reported exactly
once, that nothing leaks outside a focused box, and that password text stays
private.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
