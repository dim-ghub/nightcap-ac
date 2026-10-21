# Vox TTS

Every chat message you send in Roblox (through [Nightcap](https://github.com/CoderDayton/nightcap))
is synthesized into voice chat using fast, local Text-to-Speech (TTS). You can also
hear it on your own speakers. Settings live in an in-game menu styled like
Roblox's own.

Vox TTS (formerly Nightcap AC) is one library, `libvox_tts.so`, loaded into an unmodified
Nightcap. It requires no virtual audio cables, no virtual microphone, and no separate app.

```
 Nightcap (unmodified)
 ┌──────────────────────────────────────────────────────────────────────┐
 │  libSDL3 ◄─── libvox_tts.so (LD_PRELOAD)                             │
 │                 · chat box text ──► TTS engines (Piper / eSpeak / AC) ─┤ │
 │                 · mic samples   ◄── mixed in ◄─────────────────────┤ │
 │                                                                    ▼ │
 │                                                                speakers
 │  Vulkan  ◄─── same library as a Vulkan layer                         │
 │                 · in-game menu drawn onto each frame                 │
 └──────────────────────────────────────────────────────────────────────┘
```

## Features

* **Local Text-to-Speech Engines:**
  * **Piper TTS (Neural AI):** High-quality, natural offline AI voices (Lessac, Amy, Ryan, etc.) running locally with ONNX models. Fast, low latency, and realistic.
  * **eSpeak-NG (Retro / Lightweight):** Fast, classic synthesized speech with support for many accents and languages (US English, British, Scottish, Spanish, French, German, Japanese, etc.), pitch, and WPM speed adjustments.
  * **Animal Crossing (Animalese):** Turn your chat into Animalese blips!
    * Choose between *Classic* (GC / WW / CF / NL), *New Horizons* (syllable babble with sentence melody), or *Bebebese*.
    * Select villager personalities (Normal, Peppy, Snooty, Sisterly, Lazy, Jock, Smug, Cranky).
    * Load custom letter sound packs (`a.wav`...`z.wav`).
* **In-Game Roblox Overlay:**
  * Press **F8** (or click the top bar button) to open the 3-tab in-game menu: **TTS**, **Output**, and **Settings**.
  * Styled after Roblox's own settings menu, using Roblox's own Builder Sans font and BuilderIcons.
* **Audio Routing:**
  * Play voice directly into voice chat, to your speakers only, or both.
  * Adjust voice chat volume and local speaker volume independently.
  * Optional "Keep My Microphone On" to mix synthetic speech over your real voice.

## Requirements

* Nightcap on Linux, native install (`mocktail` on your PATH). Flatpak is best-effort.
* Nightcap's Vulkan renderer (the default), not OpenGL.
* Local TTS engines:
  * `espeak-ng` (commonly installed on most Linux distributions)
  * `piper` (optional, for neural AI voices)
* To build: C++17 compiler, SDL3, and Vulkan headers (`sdl3`, `vulkan-headers` on Arch/Artix).

## Install

```bash
git clone https://github.com/dim-ghub/nightcap-ac vox-tts
cd vox-tts
make build
make install          # into ~/.local (PREFIX=... to change)
```

`make install` installs:
* `vox-tts` (and backwards-compatible `nightcap-ac` alias)
* `libvox_tts.so` (and `libnightcap_ac.so` symlink)
* `vox-tts-download-voice` helper script
* Desktop launcher entries and Vulkan layer manifests

It will then prompt:
```
Patch it? [y/N]
```
Answering **y** updates Nightcap's launcher entry so normal launches and `roblox://` links (joining games from the website) launch with Vox TTS automatically.

To remove or undo anytime:
```bash
make unpatch-desktop    # restore previous desktop entry
make uninstall          # remove all installed files
```

## Downloading Neural Piper Voices

Vox TTS includes a helper script to download Piper neural voice models directly from Hugging Face into `~/.local/share/piper-voices`:

```bash
# Download default voice (en_US-lessac-medium)
vox-tts-download-voice

# Or download other voices:
vox-tts-download-voice en_US-amy-medium
vox-tts-download-voice en_US-ryan-medium
```

Any `.onnx` models placed in `~/.local/share/piper-voices` (or configured via a custom directory in Settings) appear in the in-game model picker.

## In-Game Controls & Menu

Press **F8** or click the top-bar button to open the menu.

### Navigation

| Key | Action |
| --- | --- |
| Tab / Shift+Tab, ↑ / ↓ | Select next / previous option |
| ← / → | Adjust value or switch option |
| Enter / Space | Toggle or activate setting |
| Q / E | Switch between menu tabs |
| Esc or F8 | Close menu |

### Menu Tabs

1. **TTS (Text-to-Speech):**
   * **Speak My Chat Messages:** Master toggle for speaking chat.
   * **Speech Engine:** Choose between `Piper (Neural)`, `eSpeak-NG`, or `Animal Crossing`.
   * **Active Engine Settings:** Each engine only displays its own controls when active:
     * *Piper*: Voice model picker, custom model file path, speed multiplier, binary status.
     * *eSpeak-NG*: Voice/accent picker, speed (WPM), pitch slider.
     * *Animal Crossing*: Game style (Classic, NH, Bebebese), villager personality, pitch, speed, and letter samples pack.
   * **Test Message:** Type any message and press Enter to test speech immediately.
2. **Output:**
   * **Play Voice In:** Voice Chat, Voice Chat + Me, or Only Me.
   * **Voice Chat / Local Volume:** Independent sliders.
   * **Keep Microphone On:** Allow speaking through real mic alongside synthetic speech.
   * **Voice Chat Status:** Live indicator showing if Roblox is currently capturing audio.
3. **Settings:**
   * **Skip Chat Commands:** Automatically ignore commands starting with `/` (e.g. `/e dance`).
   * **Menu Button Position:** Top Left, Top Right, or Hidden.
   * **Custom Piper Voices Folder:** Additional directory to scan for `.onnx` models.
   * **About:** Version info and status of the last message spoken.

## Configuration & Migration

Settings are stored at:
`~/.config/vox-tts/config.json`

If an existing configuration from `~/.config/nightcap-ac/config.json` is detected, it is automatically migrated on first run while preserving all your existing settings.

## Documentation

Detailed documentation is available in the [`docs/`](docs/) directory:

* [Architecture & Internals](docs/architecture.md): How SDL3 hooking, keystroke tracking, microphone injection, and Vulkan overlay rendering work under the hood.
* [Speech Engines Guide](docs/engines.md): Guide to Piper neural models, eSpeak-NG accents, and the custom Animalese speech synthesizer.
* [Configuration Reference](docs/configuration.md): Complete schema documentation for `config.json`, audio routing modes, and migration details.
* [Development & Testing Guide](docs/development.md): Build targets, test harness structure, and debugging environment variables.

## Development & Testing

```bash
make build   # builds library and render tool
make test    # runs unit and integration tests
```

Tests use stand-ins for Nightcap (`vk_driver`, `mic_driver`) to test Vulkan overlay rendering, input events, mic mixing, audio formats, and engine execution.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
