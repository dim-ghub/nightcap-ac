# Configuration Reference

Vox TTS stores user preferences in a standard JSON format under the user's XDG configuration directory:

```
$XDG_CONFIG_HOME/vox-tts/config.json
(typically ~/.config/vox-tts/config.json)
```

Settings can be changed dynamically using the in-game overlay menu (press **F8** or click the top-bar button) or edited by modifying the JSON file directly.

---

## Configuration Schema

Below is an example configuration file with default values:

```json
{
  "enabled": true,
  "intercept_chat": false,
  "output": "vc",
  "engine": "piper",

  "piper_model": "",
  "piper_dir": "",
  "piper_speed": 1.0,

  "espeak_voice": "en-us",
  "espeak_speed": 175.0,
  "espeak_pitch": 50.0,

  "style": "classic",
  "voice": "normal",
  "pitch": 0.0,
  "speed": 1.0,
  "sample_pack": "",

  "vc_volume": 0.9,
  "local_volume": 0.7,
  "mix_real_mic": true,
  "skip_commands": true,
  "menu_button": "left"
}
```

---

## Field Details

### General & Routing

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | boolean | `true` | Master switch for Text-to-Speech synthesis. |
| `intercept_chat` | boolean | `false` | When true, chat messages are spoken via TTS and intercepted so they are not sent to Roblox text chat. |
| `output` | string | `"vc"` | Routing destination for speech audio. Options: `"vc"` (Voice Chat only), `"both"` (Voice Chat and local speakers), `"local"` (Speakers only). |
| `engine` | string | `"piper"` | Active speech engine: `"piper"` (Piper Neural), `"espeak"` (eSpeak-NG), or `"animalese"` (Animal Crossing). |
| `skip_commands` | boolean | `true` | If true, messages starting with `/` (e.g. `/e dance`, `/w player`) are ignored and not spoken aloud. |
| `menu_button` | string | `"left"` | Placement of the in-game menu button: `"left"` (top-left near Roblox icon), `"right"` (top-right), or `"off"` (hidden, menu accessible via F8 only). |

---

### Piper Settings (`engine == "piper"`)

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `piper_model` | string | `""` | Absolute path to the selected `.onnx` voice model file. If empty, the first discovered model is used. |
| `piper_dir` | string | `""` | Custom directory path to scan for additional `.onnx` models. |
| `piper_speed` | float | `1.0` | Speed multiplier for speech pace ($0.5$ to $2.0$). |

---

### eSpeak-NG Settings (`engine == "espeak"`)

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `espeak_voice` | string | `"en-us"` | Voice / accent code passed to eSpeak (e.g., `"en-us"`, `"en-gb"`, `"es"`, `"fr"`, `"ja"`). |
| `espeak_speed` | float | `175.0` | Speaking speed in Words Per Minute (WPM) ($80$ to $350$). |
| `espeak_pitch` | float | `50.0` | Pitch contour level ($0$ to $99$). |

---

### Animal Crossing Settings (`engine == "animalese"`)

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `voice` | string | `"normal"` | Villager personality archetype or voice code: `"normal"` (`f1`), `"peppy"` (`f2`), `"sisterly"` (`f3`), `"snooty"` (`f4`), `"jock"` (`m1`), `"lazy"` (`m2`), `"smug"` (`m3`), or `"cranky"` (`m4`). |
| `animalese_lang` | string | `"english"` | Authentic sound bank language: `"english"` or `"korean"`. |
| `pitch` | float | `0.0` | Pitch offset in semitones ($-12$ to $+12$). |
| `speed` | float | `1.0` | Tempo multiplier on the villager pace ($0.5$ to $2.0$). |
| `sample_pack` | string | `""` | Optional path to custom `a.wav`...`z.wav` recordings or an `Animalese.js` sound sprite. |

---

### Audio & Mixing

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `vc_volume` | float | `0.9` | Volume gain applied when injecting audio into voice chat ($0.0$ to $1.5$). |
| `local_volume` | float | `0.7` | Volume gain applied to local speaker playback ($0.0$ to $1.5$). |
| `mix_real_mic` | boolean | `true` | If `true`, your physical microphone input is blended with the TTS speech. If `false`, physical microphone input is silenced while synthetic speech is playing. |

---

## Automatic Migration

If you previously used **Nightcap AC**, your configuration at `~/.config/nightcap-ac/config.json` is automatically detected and migrated to `~/.config/vox-tts/config.json` on the first launch. All voice, personality, volume, and style preferences are seamlessly preserved.
