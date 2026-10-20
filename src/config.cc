#include "config.h"

#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace nac {
namespace {

// Just enough JSON for a flat object of strings, numbers and booleans.
std::map<std::string, std::string> ParseFlatJson(const std::string& text) {
  std::map<std::string, std::string> values;
  size_t i = 0;
  auto skip = [&]() {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  };
  auto read_string = [&](std::string* out) {
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    while (i < text.size() && text[i] != '"') {
      if (text[i] == '\\' && i + 1 < text.size()) {
        ++i;
        switch (text[i]) {
          case 'n': *out += '\n'; break;
          case 't': *out += '\t'; break;
          default: *out += text[i];
        }
      } else {
        *out += text[i];
      }
      ++i;
    }
    ++i;
    return true;
  };
  skip();
  if (i >= text.size() || text[i] != '{') return values;
  ++i;
  while (true) {
    skip();
    std::string key;
    if (!read_string(&key)) break;
    skip();
    if (i >= text.size() || text[i] != ':') break;
    ++i;
    skip();
    std::string value;
    if (i < text.size() && text[i] == '"') {
      read_string(&value);
      value = "\"" + value;  // mark strings
    } else {
      while (i < text.size() && text[i] != ',' && text[i] != '}') value += text[i++];
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    }
    values[key] = value;
    skip();
    if (i < text.size() && text[i] == ',') {
      ++i;
      continue;
    }
    break;
  }
  return values;
}

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out + "\"";
}

}  // namespace

std::string Config::Path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::string base = xdg && *xdg ? xdg : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config";
  return base + "/nightcap-ac/config.json";
}

Config Config::Load() {
  Config c;
  std::ifstream file(Path());
  if (!file) return c;
  std::stringstream ss;
  ss << file.rdbuf();
  const auto v = ParseFlatJson(ss.str());
  auto str = [&](const char* key, std::string* out) {
    auto it = v.find(key);
    if (it != v.end() && !it->second.empty() && it->second[0] == '"') *out = it->second.substr(1);
  };
  auto num = [&](const char* key, double* out) {
    auto it = v.find(key);
    if (it != v.end() && !it->second.empty() && it->second[0] != '"') *out = std::atof(it->second.c_str());
  };
  auto flag = [&](const char* key, bool* out) {
    auto it = v.find(key);
    if (it != v.end()) *out = it->second == "true";
  };
  flag("enabled", &c.enabled);
  str("output", &c.output);
  str("voice", &c.voice);
  str("style", &c.style);
  num("pitch", &c.pitch);
  num("speed", &c.speed);
  num("vc_volume", &c.vc_volume);
  num("local_volume", &c.local_volume);
  flag("mix_real_mic", &c.mix_real_mic);
  flag("skip_commands", &c.skip_commands);
  str("sample_pack", &c.sample_pack);
  str("menu_button", &c.menu_button);
  if (!c.uses_vc() && !c.uses_local()) c.output = "vc";
  return c;
}

bool Config::Save() const {
  const std::string path = Path();
  const std::string dir = path.substr(0, path.rfind('/'));
  mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  char buf[64];
  auto number = [&](double d) {
    std::snprintf(buf, sizeof(buf), "%.3g", d);
    return std::string(buf);
  };
  std::string json = "{\n";
  json += "  \"enabled\": " + std::string(enabled ? "true" : "false") + ",\n";
  json += "  \"output\": " + Quote(output) + ",\n";
  json += "  \"voice\": " + Quote(voice) + ",\n";
  json += "  \"style\": " + Quote(style) + ",\n";
  json += "  \"pitch\": " + number(pitch) + ",\n";
  json += "  \"speed\": " + number(speed) + ",\n";
  json += "  \"vc_volume\": " + number(vc_volume) + ",\n";
  json += "  \"local_volume\": " + number(local_volume) + ",\n";
  json += "  \"mix_real_mic\": " + std::string(mix_real_mic ? "true" : "false") + ",\n";
  json += "  \"skip_commands\": " + std::string(skip_commands ? "true" : "false") + ",\n";
  json += "  \"sample_pack\": " + Quote(sample_pack) + ",\n";
  json += "  \"menu_button\": " + Quote(menu_button) + "\n}\n";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) return false;
    out << json;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace nac
