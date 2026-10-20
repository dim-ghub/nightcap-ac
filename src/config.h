// Settings, stored in $XDG_CONFIG_HOME/nightcap-ac/config.json (the same
// file and keys the earlier standalone app used).

#ifndef NIGHTCAP_AC_CONFIG_H_
#define NIGHTCAP_AC_CONFIG_H_

#include <string>

namespace nac {

struct Config {
  bool enabled = true;
  std::string output = "vc";  // "vc", "both" or "local"
  std::string voice = "normal";
  std::string style = "classic";
  double pitch = 0.0;
  double speed = 1.0;
  double vc_volume = 0.9;
  double local_volume = 0.7;
  bool mix_real_mic = true;   // keep your real mic in voice chat
  bool skip_commands = true;  // don't speak "/e dance", "/w name" ...
  std::string sample_pack;
  std::string menu_button = "left";  // "left", "right" or "off"

  bool uses_vc() const { return output == "vc" || output == "both"; }
  bool uses_local() const { return output == "local" || output == "both"; }

  static std::string Path();
  static Config Load();
  bool Save() const;
};

}  // namespace nac

#endif  // NIGHTCAP_AC_CONFIG_H_
