// Settings for Vox TTS, stored in $XDG_CONFIG_HOME/vox-tts/config.json
// (with migration from the legacy nightcap-ac/config.json).

#ifndef VOX_TTS_CONFIG_H_
#define VOX_TTS_CONFIG_H_

#include <string>

namespace nac {

struct Config {
  bool enabled = true;
  bool intercept_chat = false;  // speak via TTS without sending text to game chat
  std::string output = "vc";  // "vc", "both" or "local"
  std::string engine = "espeak";  // "espeak", "piper" or "animalese"

  // Piper neural TTS settings
  std::string piper_model;  // path to .onnx model
  std::string piper_dir;    // custom search directory for models
  double piper_speed = 1.0;

  // eSpeak-NG settings
  std::string espeak_voice = "en-us";
  double espeak_speed = 175.0;  // words per minute (80..450)
  double espeak_pitch = 50.0;   // 0..99

  // Animal Crossing (Animalese) settings
  std::string voice = "normal";
  std::string style = "classic";
  std::string animalese_lang = "english";  // "english" or "korean"
  double pitch = 0.0;
  double speed = 1.0;
  std::string sample_pack;

  // Audio output & UI settings
  double vc_volume = 0.9;
  double local_volume = 0.7;
  bool mix_real_mic = true;   // keep your real mic in voice chat
  bool skip_commands = true;  // don't speak "/e dance", "/w name" ...
  std::string menu_button = "left";  // "left", "right" or "off"

  bool uses_vc() const { return output == "vc" || output == "both"; }
  bool uses_local() const { return output == "local" || output == "both"; }

  static std::string Path();
  static std::string LegacyPath();
  static Config Load();
  bool Save() const;
};

}  // namespace nac

#endif  // VOX_TTS_CONFIG_H_
