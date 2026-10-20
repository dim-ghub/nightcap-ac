#include "icons.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace nac {
namespace {

using Bytes = std::vector<unsigned char>;

uint32_t U16(const Bytes& b, size_t at) { return at + 2 <= b.size() ? (b[at] << 8) | b[at + 1] : 0; }
uint32_t U32(const Bytes& b, size_t at) {
  return at + 4 <= b.size() ? (uint32_t(b[at]) << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3] : 0;
}

bool FindTable(const Bytes& font, const char* tag, size_t* offset, size_t* length) {
  const uint32_t tables = U16(font, 4);
  for (uint32_t i = 0; i < tables; ++i) {
    const size_t rec = 12 + i * 16;
    if (rec + 16 > font.size()) return false;
    if (std::equal(tag, tag + 4, font.begin() + rec)) {
      *offset = U32(font, rec + 8);
      *length = U32(font, rec + 12);
      return *offset + *length <= font.size();
    }
  }
  return false;
}

// glyph id -> name, from a version 2.0 'post' table (custom names only; the
// 258 standard Macintosh names are never icons).
std::map<uint32_t, std::string> GlyphNames(const Bytes& font) {
  std::map<uint32_t, std::string> names;
  size_t post, len;
  if (!FindTable(font, "post", &post, &len) || U32(font, post) != 0x00020000) return names;
  const uint32_t glyphs = U16(font, post + 32);
  const size_t index_at = post + 34;
  std::vector<std::string> custom;
  for (size_t p = index_at + glyphs * 2; p < post + len;) {
    const size_t n = font[p];
    if (p + 1 + n > post + len) break;
    custom.emplace_back(reinterpret_cast<const char*>(&font[p + 1]), n);
    p += 1 + n;
  }
  for (uint32_t g = 0; g < glyphs; ++g) {
    const uint32_t idx = U16(font, index_at + g * 2);
    if (idx >= 258 && idx - 258 < custom.size()) names[g] = custom[idx - 258];
  }
  return names;
}

// glyph id -> codepoint, from the Unicode 'cmap' subtables (formats 4 and 12).
std::map<uint32_t, uint32_t> GlyphCodepoints(const Bytes& font) {
  std::map<uint32_t, uint32_t> out;
  size_t cmap, len;
  if (!FindTable(font, "cmap", &cmap, &len)) return out;
  const uint32_t subtables = U16(font, cmap + 2);
  for (uint32_t i = 0; i < subtables; ++i) {
    const uint32_t platform = U16(font, cmap + 4 + i * 8);
    const size_t sub = cmap + U32(font, cmap + 4 + i * 8 + 4);
    if (platform != 0 && platform != 3) continue;
    const uint32_t format = U16(font, sub);
    if (format == 12) {
      const uint32_t groups = U32(font, sub + 12);
      for (uint32_t gi = 0; gi < groups; ++gi) {
        const size_t g = sub + 16 + gi * 12;
        const uint32_t start = U32(font, g), end = U32(font, g + 4), glyph = U32(font, g + 8);
        for (uint32_t cp = start; cp <= end && cp - start < 0x10000; ++cp) out.emplace(glyph + (cp - start), cp);
      }
    } else if (format == 4) {
      const uint32_t segs = U16(font, sub + 6) / 2;
      const size_t ends = sub + 14, starts = ends + segs * 2 + 2, deltas = starts + segs * 2,
                   ranges = deltas + segs * 2;
      for (uint32_t s = 0; s < segs; ++s) {
        const uint32_t end = U16(font, ends + s * 2), start = U16(font, starts + s * 2);
        const uint32_t delta = U16(font, deltas + s * 2), range = U16(font, ranges + s * 2);
        for (uint32_t cp = start; cp <= end && cp != 0xFFFF; ++cp) {
          uint32_t glyph;
          if (range == 0) {
            glyph = (cp + delta) & 0xFFFF;
          } else {
            const size_t at = ranges + s * 2 + range + (cp - start) * 2;
            glyph = U16(font, at);
            if (glyph != 0) glyph = (glyph + delta) & 0xFFFF;
          }
          if (glyph != 0) out.emplace(glyph, cp);
        }
      }
    }
  }
  return out;
}

}  // namespace

bool IconFont::Load(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const Bytes font((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (font.size() < 12) return false;
  const auto names = GlyphNames(font);
  const auto codepoints = GlyphCodepoints(font);
  codepoints_.clear();
  for (const auto& [glyph, name] : names) {
    auto cp = codepoints.find(glyph);
    if (cp != codepoints.end()) codepoints_[name] = cp->second;
  }
  path_ = path;
  return !codepoints_.empty();
}

uint32_t IconFont::Find(const std::string& name) const {
  auto it = codepoints_.find(name);
  return it == codepoints_.end() ? 0 : it->second;
}

std::string IconFont::Utf8(const std::string& name) const {
  const uint32_t cp = Find(name);
  std::string out;
  if (cp == 0) return out;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

std::string FindRobloxIconFont() {
  std::vector<std::string> roots;
  if (const char* data = std::getenv("XDG_DATA_HOME")) roots.push_back(std::string(data) + "/mocktail/payloads");
  if (const char* home = std::getenv("HOME")) {
    roots.push_back(std::string(home) + "/.local/share/mocktail/payloads");
    roots.push_back(std::string(home) + "/.var/app/io.github.CoderDayton.nightcap/data/mocktail/payloads");
  }
  const char* rel = "/assets/ExtraContent/LuaPackages/Packages/_Index/BuilderIcons/BuilderIcons/Font/"
                    "BuilderIcons-Regular.ttf";
  std::string best;
  time_t best_time = 0;
  for (const std::string& root : roots) {
    DIR* dir = opendir(root.c_str());
    if (dir == nullptr) continue;
    while (dirent* entry = readdir(dir)) {
      if (entry->d_name[0] == '.') continue;
      const std::string path = root + "/" + entry->d_name + rel;
      struct stat st {};
      if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_mtime >= best_time) {
        best = path;
        best_time = st.st_mtime;
      }
    }
    closedir(dir);
  }
  return best;
}

}  // namespace nac
