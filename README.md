# Nightcap AC

Every chat message you send in Roblox (through [Nightcap](https://github.com/CoderDayton/nightcap))
is spoken into voice chat as Animal Crossing–style Animalese. You can also
hear it on your own speakers. Settings live in an in-game menu styled like
Roblox's own.

Nightcap AC is one library, `libnightcap_ac.so`, loaded into an unmodified
Nightcap. It needs no virtual microphone and no separate app.

## How it works

```
 Nightcap (unmodified)
 ┌──────────────────────────────────────────────────────────────────────┐
 │  libSDL3 ◄─── libnightcap_ac.so (LD_PRELOAD)                         │
 │                 · chat box text ──► Animalese synth ──┐              │
 │                 · mic samples   ◄── mixed in ◄────────┤──► speakers  │
 │  Vulkan  ◄─── same library as a Vulkan layer                         │
 │                 · in-game menu drawn onto each frame                 │
 └──────────────────────────────────────────────────────────────────────┘
```

* **Chat.** When a Roblox text box gets focus, Nightcap turns on SDL text
  input. The library follows what you type until Enter sends it. Nothing is
  read while no box is focused, and password boxes are never read.
* **Voice chat.** Nightcap reads your microphone with
  `SDL_GetAudioStreamData`. The library mixes the Animalese voice into those
  samples, in whatever format and rate the mic stream uses, before Roblox
  gets them.
* **Your speakers.** If you choose, the voice also plays on its own SDL
  playback stream inside Nightcap.
* **Menu.** The library is also an implicit Vulkan layer. Before each frame
  is shown, it draws the menu with Dear ImGui in Roblox's style, using
  Roblox's own font (Builder Sans) and icons (BuilderIcons), loaded from
  Nightcap's downloaded Roblox files. While the menu is open it gets the mouse and keyboard, and the cursor
  is freed from first-person mouse lock. Both are handed back when it closes.

## Requirements

* Nightcap on Linux, native install (`mocktail` on your PATH). Flatpak is
  best-effort (see below).
* Nightcap's Vulkan renderer (the default), not OpenGL
* To build: a C++17 compiler, SDL3 and Vulkan headers (`sdl3`,
  `vulkan-headers` on Arch)
* Optional: `zenity` for the restart question, and `python3` for opening the
  menu of an already running Nightcap

## Install

```bash
git clone https://github.com/dim-ghub/nightcap-ac
cd nightcap-ac
make build
make install          # into ~/.local (PREFIX=... to change)
```

`make install` adds a **Nightcap AC** launcher entry and the `nightcap-ac`
command, then asks:

```
Patch it? [y/N]
```

Answering **y** copies Nightcap's own launcher entry to
`~/.local/share/applications/io.github.CoderDayton.nightcap.desktop`, starting
Nightcap through Nightcap AC. Then the normal **Nightcap** entry and
`roblox://` links (clicking Join on the website) load it too. The copy leaves
out Nightcap's `X-Mocktail-Managed` marker, so Nightcap's
`register_url_handler.sh` won't overwrite it. Change your mind any time:

```bash
make patch-desktop      # add the patch
make unpatch-desktop    # remove it (an entry you had there before comes back)
```

`NIGHTCAP_AC_DESKTOP=y make install` (or `=n`) answers the question for
scripted installs. To update, run `git pull && make install`. To remove
everything, including the patch, run `make uninstall`.

## Use

1. Open **Nightcap AC** (or the patched **Nightcap** entry, or run
   `nightcap-ac`).
2. Join a game with voice chat and unmute your mic in Roblox.
3. Chat as usual (`/` or click the chat bar, type, press Enter). The message
   plays in voice chat as Animalese.
4. Press **F8**, or click the speech-bubble button next to Roblox's top-left
   buttons, to open the menu. **Esc**, **F8** or **Close** closes it.

The menu follows Roblox's in-experience menu, timed from a 60 fps recording
of Roblox's own:

* It slides up from below the screen as it opens, and drops away when it
  closes. Clicking outside it, Esc, F8 or **Close** closes it.
* Pages slide sideways when you switch tabs, and picker values swipe left
  or right with the arrow you pressed.
* It uses Roblox's own font (Builder Sans) and Roblox's own icons
  (BuilderIcons), both loaded from Nightcap's downloaded Roblox files.
  Nothing of Roblox's is bundled here.
* The top-bar button is a solid circle like Roblox's; its icon grows while
  the menu is open.

It works with the mouse or the keyboard:

| Key | Does |
| --- | --- |
| Tab / Shift+Tab, ↑ / ↓ | Select the next / previous setting (white outline, like Roblox's selection) |
| ← / → | Change the selected setting |
| Enter / Space | Toggle or activate the selected setting |
| Q / E | Previous / next page |
| T / S | Test voice / Stop |
| Esc or F8 | Close |

### What opening Nightcap AC does

| Nightcap is... | What happens |
| --- | --- |
| not running | Nightcap starts with Nightcap AC loaded |
| running with Nightcap AC | Its in-game menu opens. A clicked `roblox://` link goes to the running Nightcap |
| running **without** it | A dialog offers **Restart and rejoin** (back into the same server), **Restart only**, or **Not now**. If you clicked a join link, the restart opens that link instead |

The running Nightcap is found the way Nightcap's own settings window finds
it: through the process holding its instance lock. The rejoin reads the last
`Joining game '<server>' place <id>` line from the session log and restarts
with `--launch-uri roblox://experiences/start?placeId=..&gameInstanceId=..`.
The restart closes Nightcap with SIGTERM, so it leaves the game cleanly.

### The menu

**Voice**

| Setting | What it does |
| --- | --- |
| Speak my chat messages | On or Off |
| Game | *Classic*: one clipped letter sound per letter, like GameCube through New Leaf. *New Horizons*: syllable babble with a sentence melody. *Bebebese*: every letter is "be" |
| Personality | Normal, Peppy, Snooty, Sisterly, Lazy, Jock, Smug, Cranky |
| Pitch / Speed | ±12 semitones; 50 to 200% |
| Test message | Type something and press Enter to hear it |

**Output**

| Setting | What it does |
| --- | --- |
| Play the voice in | *Voice chat*, *Voice chat + me*, or *Only me* |
| Voice chat volume / My speakers volume | Separate levels |
| Keep my microphone on | Off: voice chat hears only the Animalese voice |
| Status | Whether you're in voice chat right now (Nightcap is reading your mic) |

**Settings**

| Setting | What it does |
| --- | --- |
| Skip chat commands | Don't speak `/e dance`, `/w name` and the like |
| Menu button | Top left (next to Roblox's buttons), top right, or hidden. F8 always works |
| Letter samples | A folder of `a.wav`…`z.wav`, or one WAV with all 26 letters back to back (the Animalese.js layout), for the exact in-game sound. Classic only, and not included |

Settings are saved to `~/.config/nightcap-ac/config.json`, the same file the
earlier standalone version used.

### How it imitates Animal Crossing

* Each letter (Classic) or syllable (New Horizons) is a short blip, timed
  like the text box printing characters.
* Every blip gets a random pitch around the speaker's voice.
* `?` makes the last word climb and bend upward. `!` raises and boosts the
  sentence. ALL-CAPS words are shouted.
* Commas, periods and `...` add the matching pauses. Numbers and symbols are
  read out. Emoji are silent.
* The voices are synthesized: a harmonic voice through vocal-tract formants,
  with noise bursts for consonants. No game audio is included.

## Limitations

* **VR:** the menu isn't visible inside the headset, because Nightcap sends
  VR frames through OpenXR. Speech still works. Use F8 on the desktop window.
* **OpenGL:** with Nightcap's OpenGL renderer, speech works but there's no
  menu. Edit `config.json` instead.
* **Menu button position:** the button sits where Roblox's top-left buttons
  normally end. If a game's top bar is wider, move it to the top right or
  hide it under Settings and use F8.
* **Other text boxes:** the library can't tell the chat bar from a game's
  own single-line text box, so text sent with Enter from any text box is
  spoken. Games with their own in-world keyboard (like VR Hands) don't go
  through a text box, so their messages aren't spoken.
* **Flatpak:** the launcher passes the library and layer into the sandbox,
  but the library is built against your system's libraries, which may be
  newer than the Flatpak runtime's.

## Development

```bash
make build    # the library and build/nightcap-ac-render
make test     # also builds the stand-ins below
```

The tests load the library into stand-ins for Nightcap:

* `tests/vk_driver.cc`: an SDL3 + Vulkan program. The tests open the menu,
  click, press F8, and check saved frames (`NIGHTCAP_AC_DUMP=/path.ppm` saves
  one composited frame).
* `tests/mic_driver.cc`: opens the microphone the way Nightcap does, using
  SDL's file-backed `disk` audio driver, and sends a chat message. The tests
  check the speech lands in the mic stream.
* `tests/chat_test.cc`: chat-box tracking.
* `tests/test_synth.py`: the synthesizer.
* `tests/test_launcher.py`: the launcher's pieces.

`build/nightcap-ac-render OUT.wav "text" [voice] [style] [pitch] [speed]`
renders the voice to a file.

Dear ImGui (MIT) is vendored in `third_party/imgui`.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
