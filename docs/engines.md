# Speech Engines Guide

Vox TTS supports three distinct speech synthesis backends. Each engine caters to different preferences, ranging from modern neural voices to retro speech synthesizers and game audio emulation.

---

## 1. Piper (Neural AI)

[Piper](https://github.com/rhasspy/piper) is a fast, local neural text-to-speech engine running with ONNX Runtime. It produces natural, high-fidelity human voices with low latency and low CPU utilization.

### Capabilities
- **Natural Intonation**: Neural acoustic models sound natural and expressive.
- **Multiple Qualities**: Available in `low`, `medium`, and `high` quality presets.
- **Wide Voice Library**: Dozens of voices and accents available across many languages.
- **Speed Adjustment**: Real-time pace adjustment from 50% to 200% of normal speaking speed.

### Model Discovery
Vox TTS automatically discovers `.onnx` models (and accompanying `.onnx.json` config files) by scanning directories recursively up to 8 levels deep:
- `$XDG_DATA_HOME/piper-voices` (default: `~/.local/share/piper-voices`)
- `/usr/share/piper-voices` (used by distribution packages such as Arch/Artix `piper-voices-*`)
- `/usr/local/share/piper-voices`
- `$XDG_DATA_HOME/vox-tts/voices` and `~/.config/piper/voices`
- Custom user-configured directory specified in the **Settings** menu.

### Downloading Neural Models
Vox TTS provides a built-in helper utility to easily download voice models from Hugging Face into `~/.local/share/piper-voices`:

```bash
# Download the default high-quality voice (en_US-lessac-medium)
vox-tts-download-voice

# Download other voices:
vox-tts-download-voice en_US-amy-medium
vox-tts-download-voice en_US-ryan-medium
vox-tts-download-voice en_GB-alan-medium
```

Any `.onnx` files downloaded or copied into these folders appear automatically in the **Model Voice** picker in the in-game menu.

### Execution Pipeline
Vox TTS supports both standalone binary distributions (`piper`) and Python package distributions (`piper-tts`):
- Invoked with `-m <model.onnx>` and `--length-scale <speed>`.
- Audio is output to a temporary WAV container and decoded via `SDL_LoadWAV` into 32-bit floating point audio at 48 kHz.

---

## 2. eSpeak-NG (Retro / Lightweight)

[eSpeak-NG](https://github.com/espeak-ng/espeak-ng) is a compact, open-source software speech synthesizer based on formant synthesis.

### Capabilities
- **Instantaneous Generation**: Synthesis takes almost zero milliseconds, running with minimal memory and CPU footprint.
- **Extensive Accents & Languages**:
  - English: US, UK (Received Pronunciation), Scottish, Australian, Caribbean, etc.
  - International: Spanish, French, German, Italian, Japanese, Portuguese, Polish, Russian, and more.
- **Speed Control**: Configurable from 80 to 350 words per minute (WPM).
- **Pitch Control**: Adjustable from 0 to 99 for low to high pitch contours.

### Audio Pipeline
- Executes `espeak-ng` (or fallback `espeak`) with `--stdout -s <wpm> -p <pitch> -v <voice>`.
- Raw audio is streamed directly and resampled to 48 kHz mono float.

---

## 3. Animal Crossing (Animalese)

Animalese is the iconic fictional language spoken by villagers throughout the *Animal Crossing* series. Rather than playing static audio clips, Vox TTS features a full algorithmic formant and noise synthesizer written from scratch (`src/synth.cc`).

### How It Works
Animalese works by converting input text into short phonetic speech "blips" timed in step with text display:
- **Harmonic Voice Source**: Generates pitched vocal fold vibrations shaped by vocal tract resonances (formants).
- **Consonant Noise Generators**: Mixes filtered aspiration and turbulent noise bursts for consonants (`k`, `s`, `t`, `p`, etc.).
- **Vocal Tract Scaling**: Shifts formants higher to achieve the distinct "chipmunk" sound of animal characters.
- **Intonation & Jitter**: Subtle random pitch variations per blip, bending upwards automatically for questions (`?`).

### Game Styles
1. **Classic** (*GameCube, Wild World, City Folk, New Leaf*):
   - Fast, clipped letter blips with fixed slot timing.
   - Compatible with custom sound packs.
2. **New Horizons** (*Animal Crossing: New Horizons*):
   - Articulates consonants into vowels, flowing over an expressive falling or rising sentence pitch contour.
3. **Bebebese**:
   - The comical "Bebebese" dialect where every letter blip repeats the syllable "be".

### Villager Personalities
Each personality type configures base frequency ($f_0$), vocal tract formant scaling, speech tempo (characters per second), aspiration noise, and spectral tilt:
- **Normal**: Sweet, balanced, moderate pitch ($430\text{ Hz}$).
- **Peppy**: High-pitched, enthusiastic, fast tempo ($540\text{ Hz}$).
- **Snooty**: Higher pitch, smooth spectral tilt ($490\text{ Hz}$).
- **Sisterly / Uchi**: Grounded, slightly deeper tone ($330\text{ Hz}$).
- **Lazy**: Slow, warm, deep blips ($230\text{ Hz}$).
- **Jock**: Energetic, brisk cadence ($290\text{ Hz}$).
- **Smug**: Melodic, self-assured intonation ($310\text{ Hz}$).
- **Cranky**: Very deep, gravelly voice with high spectral tilt ($150\text{ Hz}$).

### Custom Letter Samples
For Classic style, you can supply your own folder of recorded letter blips (`a.wav` through `z.wav`) or an `Animalese.js` sound-sprite sheet. Specify the folder path in the **Letter Samples** setting to use authentic sound samples.
