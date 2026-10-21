#include "ui.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "app.h"
#include "gamestate.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "icons.h"
#include "speech.h"
#include "synth.h"
#include "tts.h"

// Layout and colours follow Roblox's in-experience menu (Settings page),
// measured from screenshots: a black, slightly see-through panel; flat rows
// with a label column on the left and a control column from ~40% across;
// big thin chevrons for pickers; ten-block sliders with circled -/+; dark
// bottom buttons with key hints and a navy primary; a white rounded outline
// for the keyboard/gamepad selection.

namespace nac::ui {
namespace {

// --------------------------------------------------------------------------
// Scale, colour, animation

float g_px = 1.0f;     // swapchain pixels per logical unit
bool g_srgb = false;   // target stores linear values
float g_alpha = 1.0f;  // fade for the open / page-switch animations

float S(float logical) { return std::round(logical * g_px); }

float Linear(int v) {
  const float x = v / 255.0f;
  if (!g_srgb) return x;
  return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

ImU32 C(int r, int g, int b, float a = 1.0f) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(Linear(r), Linear(g), Linear(b), a * g_alpha));
}

ImVec4 C4(int r, int g, int b, float a = 1.0f) {
  return ImVec4(Linear(r), Linear(g), Linear(b), a * g_alpha);
}

ImU32 White(float a = 1.0f) { return C(255, 255, 255, a); }
ImU32 Muted() { return C(178, 179, 181); }         // inactive tabs, subtitles
ImU32 Panel() { return C(0, 0, 0, 0.82f); }         // see-through black
ImU32 Highlight() { return C(35, 37, 39); }         // hovered / selected row
ImU32 BlockOn() { return C(217, 217, 217); }
ImU32 BlockOff() { return C(57, 59, 61); }
ImU32 ButtonBg() { return C(24, 26, 30, 0.92f); }
ImU32 ButtonHover() { return C(38, 40, 45, 0.95f); }
ImU32 Primary() { return C(20, 38, 102); }
ImU32 PrimaryHover() { return C(29, 52, 136); }
ImU32 Divider() { return C(255, 255, 255, 0.1f); }

using Clock = std::chrono::steady_clock;
Clock::time_point g_start = Clock::now();
float Now() { return std::chrono::duration<float>(Clock::now() - g_start).count(); }

// --------------------------------------------------------------------------
// Fonts

struct Fonts {
  ImFont* title = nullptr;   // row labels (Medium 18)
  ImFont* value = nullptr;   // picker values, field text (Regular 18)
  ImFont* small = nullptr;   // subtitles (Regular 13)
  ImFont* tiny = nullptr;    // slider end labels (Regular 12.5)
  ImFont* tab = nullptr;     // tab labels (Medium 20)
  ImFont* header = nullptr;  // section headers (Bold 25)
  ImFont* button = nullptr;  // bottom buttons (Bold 18)
  ImFont* hint = nullptr;    // key hints (Bold 10)
  float px = 0;
  std::string family;
} g_fonts;

bool IsFile(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Builder Sans ships with Roblox itself: use the newest downloaded payload.
std::string FindRobloxFonts() {
  std::vector<std::string> roots;
  if (const char* data = std::getenv("XDG_DATA_HOME")) roots.push_back(std::string(data) + "/mocktail/payloads");
  if (const char* home = std::getenv("HOME")) {
    roots.push_back(std::string(home) + "/.local/share/mocktail/payloads");
    roots.push_back(std::string(home) + "/.var/app/io.github.CoderDayton.nightcap/data/mocktail/payloads");
  }
  std::string best;
  time_t best_time = 0;
  for (const std::string& root : roots) {
    DIR* dir = opendir(root.c_str());
    if (dir == nullptr) continue;
    while (dirent* entry = readdir(dir)) {
      if (entry->d_name[0] == '.') continue;
      const std::string fonts = root + "/" + entry->d_name + "/assets/content/fonts/";
      struct stat st {};
      if (stat((fonts + "BuilderSans-Medium.otf").c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
          st.st_mtime >= best_time) {
        best = fonts;
        best_time = st.st_mtime;
      }
    }
    closedir(dir);
  }
  return best;
}

ImFont* AddFont(const std::string& dir, const char* file, float size, const char* fallback) {
  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  cfg.PixelSnapH = true;
  std::string path = dir.empty() ? "" : dir + file;
  if (path.empty() || !IsFile(path)) path = fallback ? fallback : "";
  if (!path.empty() && IsFile(path)) {
    if (ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg)) return f;
  }
  cfg.SizePixels = size;
  return io.Fonts->AddFontDefault(&cfg);
}

// --------------------------------------------------------------------------
// Drawing helpers

ImDrawList* g_dl = nullptr;

void Text(ImFont* font, ImVec2 pos, ImU32 col, const char* text) {
  g_dl->AddText(font, font->FontSize, ImVec2(std::round(pos.x), std::round(pos.y)), col, text);
}

ImVec2 Measure(ImFont* font, const char* text) {
  return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
}

void TextCentered(ImFont* font, ImVec2 center, ImU32 col, const char* text) {
  const ImVec2 size = Measure(font, text);
  Text(font, ImVec2(center.x - size.x / 2, center.y - size.y / 2), col, text);
}

// Outline icons, like Roblox's menu tab icons.
void BubbleIcon(ImDrawList* dl, ImVec2 c, float size, ImU32 col, float stroke, bool filled, float dots) {
  const float w = size * 0.92f, h = size * 0.68f;
  const ImVec2 a(c.x - w / 2, c.y - h / 2 - size * 0.08f), b(c.x + w / 2, c.y + h / 2 - size * 0.08f);
  const ImVec2 tail[3] = {ImVec2(a.x + w * 0.2f, b.y - stroke * 0.5f), ImVec2(a.x + w * 0.16f, b.y + size * 0.22f),
                          ImVec2(a.x + w * 0.44f, b.y - stroke * 0.5f)};
  if (filled) {
    dl->AddRectFilled(a, b, col, h * 0.45f);
    dl->AddTriangleFilled(tail[0], tail[1], tail[2], col);
  } else {
    dl->AddRect(a, b, col, h * 0.45f, 0, stroke);
    dl->AddPolyline(tail, 3, col, 0, stroke);
  }
  const float r = size * 0.065f;
  const ImU32 dot = filled ? C(0, 0, 0, 0.85f) : col;
  for (int i = 0; i < 3; ++i) {
    float lift = 0;
    if (dots >= 0) lift = std::max(0.0f, std::sin(dots * 8.0f - i * 0.9f)) * size * 0.08f;
    dl->AddCircleFilled(ImVec2(c.x + (i - 1) * size * 0.22f, (a.y + b.y) / 2 - lift), r, dot);
  }
}

void SpeakerIcon(ImVec2 c, float s, ImU32 col, float stroke) {
  const ImVec2 body[6] = {ImVec2(c.x - s * 0.42f, c.y - s * 0.14f), ImVec2(c.x - s * 0.22f, c.y - s * 0.14f),
                          ImVec2(c.x + s * 0.02f, c.y - s * 0.36f), ImVec2(c.x + s * 0.02f, c.y + s * 0.36f),
                          ImVec2(c.x - s * 0.22f, c.y + s * 0.14f), ImVec2(c.x - s * 0.42f, c.y + s * 0.14f)};
  g_dl->AddPolyline(body, 6, col, ImDrawFlags_Closed, stroke);
  for (int i = 0; i < 2; ++i) {
    g_dl->PathArcTo(ImVec2(c.x + s * 0.04f, c.y), s * (0.18f + 0.15f * i), -0.8f, 0.8f, 12);
    g_dl->PathStroke(col, 0, stroke);
  }
}

void GearIcon(ImVec2 c, float s, ImU32 col, float stroke) {
  const int teeth = 8;
  const float outer = s * 0.48f, inner = s * 0.37f;
  for (int i = 0; i < teeth * 4; ++i) {
    // Each tooth: up the flank, across the top, down the flank, along the root.
    const float a = (i / 4.0f) * 2 * 3.14159265f / teeth;
    const float r = (i % 4 == 1 || i % 4 == 2) ? outer : inner;
    const float spread = (i % 4 == 0 || i % 4 == 1) ? -0.17f : 0.17f;
    g_dl->PathLineTo(ImVec2(c.x + std::cos(a + spread) * r, c.y + std::sin(a + spread) * r));
  }
  g_dl->PathStroke(col, ImDrawFlags_Closed, stroke);
  g_dl->AddCircle(c, s * 0.14f, col, 20, stroke);
}

// Roblox's icon font (BuilderIcons), when the payload has it.
IconFont g_icons;
ImFont* g_icon_font = nullptr;

// Draws a Roblox icon centred on c; false when the icon isn't available.
bool Icon(ImDrawList* dl, const char* name, ImVec2 c, float size, ImU32 col) {
  if (g_icon_font == nullptr) return false;
  const std::string glyph = g_icons.Utf8(name);
  if (glyph.empty()) return false;
  const ImVec2 ts = g_icon_font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph.c_str());
  dl->AddText(g_icon_font, size, ImVec2(std::round(c.x - ts.x / 2), std::round(c.y - ts.y / 2)), col,
              glyph.c_str());
  return true;
}

void Chevron(ImVec2 c, float height, bool right, ImU32 col) {
  if (Icon(g_dl, right ? "chevron-large-right" : "chevron-large-left", c, height * 1.25f, col)) return;
  const float d = right ? 1.0f : -1.0f, w = height * 0.5f;
  const ImVec2 pts[3] = {ImVec2(c.x - d * w / 2, c.y - height / 2), ImVec2(c.x + d * w / 2, c.y),
                         ImVec2(c.x - d * w / 2, c.y + height / 2)};
  g_dl->AddPolyline(pts, 3, col, 0, S(2.6f));
}

void CircleSign(ImVec2 c, bool plus, ImU32 col) {
  if (Icon(g_dl, plus ? "circle-plus" : "circle-minus", c, S(34), col)) return;
  const float r = S(14);
  g_dl->AddCircle(c, r, col, 32, S(2));
  g_dl->AddLine(ImVec2(c.x - r * 0.5f, c.y), ImVec2(c.x + r * 0.5f, c.y), col, S(2.2f));
  if (plus) g_dl->AddLine(ImVec2(c.x, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), col, S(2.2f));
}

void KeyHint(ImVec2 c, const char* key, ImU32 col) {
  const std::string glyph = std::string("square-text-") + (std::strcmp(key, "ESC") == 0 ? "esc" : key);
  if (Icon(g_dl, glyph.c_str(), c, S(32), col)) return;
  const ImVec2 text = Measure(g_fonts.hint, key);
  const float w = std::max(S(26), text.x + S(9)), h = S(26);
  g_dl->AddRect(ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2), col, S(3), 0, S(1.5f));
  TextCentered(g_fonts.hint, c, col, key);
}

// The white rounded outline Roblox draws around the selected element.
void SelectionOutline(ImVec2 min, ImVec2 max, float rounding) {
  const float g = S(3);
  g_dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g), White(), rounding + g, 0, S(2));
}

// --------------------------------------------------------------------------
// Keyboard / gamepad-style selection: Tab or ↑/↓ move between items,
// ←/→ adjust, Enter or Space activates. Clicking hides the outline.

struct Nav {
  int count = 0;       // items registered this frame
  int last_count = 0;  // ... and last frame
  int focus = -1;
  bool shown = false;  // outline visible (keyboard in use)
  bool scroll = false; // bring the focused item into view
} g_nav;

bool g_editing = false;  // a text field has the keyboard

struct NavItem {
  bool focused = false;
  bool left = false, right = false, activate = false;
};

bool g_nav_off = false;  // drawing a page that is sliding away

NavItem RegisterNav(ImVec2 min, ImVec2 max) {
  NavItem item;
  if (g_nav_off) return item;
  const int id = g_nav.count++;
  item.focused = g_nav.shown && id == g_nav.focus;
  if (!item.focused) return item;
  if (g_nav.scroll) {
    ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(min, max));
    g_nav.scroll = false;
  }
  if (!g_editing) {
    item.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    item.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    item.activate = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                    ImGui::IsKeyPressed(ImGuiKey_Space);
  }
  return item;
}

void UpdateNav() {
  g_nav.last_count = g_nav.count;
  g_nav.count = 0;
  const int n = g_nav.last_count;
  if (n == 0) return;
  const bool back = ImGui::IsKeyPressed(ImGuiKey_Tab) && ImGui::GetIO().KeyShift;
  const bool next = ImGui::IsKeyPressed(ImGuiKey_Tab) && !ImGui::GetIO().KeyShift;
  const bool up = !g_editing && ImGui::IsKeyPressed(ImGuiKey_UpArrow);
  const bool down = !g_editing && ImGui::IsKeyPressed(ImGuiKey_DownArrow);
  if (next || down || back || up) {
    if (!g_nav.shown || g_nav.focus < 0) {
      g_nav.focus = (back || up) ? n - 1 : 0;
    } else {
      g_nav.focus = (g_nav.focus + ((back || up) ? n - 1 : 1)) % n;
    }
    g_nav.shown = true;
    g_nav.scroll = true;
    if (g_editing) ImGui::ClearActiveID();
  }
  if (ImGui::IsMouseClicked(0)) g_nav.shown = false;
}

// --------------------------------------------------------------------------
// Rows and controls

bool Hit(const char* id, ImVec2 min, ImVec2 max, bool* hovered = nullptr, bool* held = nullptr) {
  ImGui::SetCursorScreenPos(min);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)));
  if (hovered) *hovered = ImGui::IsItemHovered();
  if (held) *held = ImGui::IsItemActive();
  return clicked;
}

// A settings row: label (and optional subtitle) on the left, the control in
// a column on the right. Rows are flat; hovering or selecting highlights.
struct Row {
  ImVec2 min, max;
  float ctrl_l, ctrl_r;  // control column
  float mid;             // vertical centre of the control
  NavItem nav;
  int id = 0;            // unique per frame, for widget IDs
};

Row BeginRow(const char* title, const char* subtitle, float height = 0) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x - S(10);
  const float h = height > 0 ? S(height) : S(subtitle ? 70 : 50);
  Row row;
  row.min = pos;
  row.max = ImVec2(pos.x + w, pos.y + h);
  row.ctrl_l = pos.x + std::round(w * 0.4f);
  row.ctrl_r = row.max.x - S(14);
  row.mid = (row.min.y + row.max.y) / 2;
  row.id = g_nav.count;
  row.nav = RegisterNav(row.min, row.max);
  const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(row.min, row.max);
  if (hovered || row.nav.focused) g_dl->AddRectFilled(row.min, row.max, Highlight(), S(8));
  if (row.nav.focused) SelectionOutline(row.min, row.max, S(8));
  const float title_h = g_fonts.title->FontSize;
  if (subtitle) {
    const float sub_w = row.ctrl_l - row.min.x - S(24);
    const ImVec2 sub = g_fonts.small->CalcTextSizeA(g_fonts.small->FontSize, FLT_MAX, sub_w, subtitle);
    const float top = (row.min.y + row.max.y) / 2 - (title_h + S(2) + sub.y) / 2;
    Text(g_fonts.title, ImVec2(pos.x + S(10), top), White(), title);
    g_dl->AddText(g_fonts.small, g_fonts.small->FontSize, ImVec2(pos.x + S(10), top + title_h + S(2)), Muted(),
                  subtitle, nullptr, sub_w);
  } else {
    Text(g_fonts.title, ImVec2(pos.x + S(10), (row.min.y + row.max.y) / 2 - title_h / 2), White(), title);
  }
  return row;
}

void EndRow(const Row& row) {
  ImGui::SetCursorScreenPos(ImVec2(row.min.x, row.max.y));
  ImGui::Dummy(ImVec2(row.max.x - row.min.x, S(2)));
}

void Header(const char* text, bool first = false) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x - S(10);
  float y = pos.y;
  if (!first) {
    y += S(12);
    g_dl->AddLine(ImVec2(pos.x + S(10), y), ImVec2(pos.x + w, y), Divider(), S(1));
    y += S(16);
  } else {
    y += S(6);
  }
  Text(g_fonts.header, ImVec2(pos.x + S(10), y), White(), text);
  ImGui::Dummy(ImVec2(w, y - pos.y + g_fonts.header->FontSize + S(10)));
}

// Picker values swipe when changed: the old text slides a little toward the
// side away from the arrow you pressed and fades, the new one slides in.
struct Swipe {
  float at = -10;
  float dir = 0;  // +1: pressed ›, -1: pressed ‹
  std::string old;
};
std::map<ImGuiID, Swipe> g_swipes;

void ValueText(ImGuiID id, const std::string& before, const std::string& now, float dir, ImVec2 center) {
  Swipe& sw = g_swipes[id];
  if (dir != 0 && before != now) {
    sw.at = Now();
    sw.dir = dir;
    sw.old = before;
  }
  const float t = std::clamp((Now() - sw.at) / 0.16f, 0.0f, 1.0f);
  if (t >= 1.0f) {
    TextCentered(g_fonts.value, center, White(), now.c_str());
    return;
  }
  const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
  const float dist = S(26);
  const float a = g_alpha;
  const float out = std::max(0.0f, 1.0f - e * 1.8f);  // the old value clears quickly
  g_alpha = a * out;
  TextCentered(g_fonts.value, ImVec2(center.x - sw.dir * dist * e, center.y), White(), sw.old.c_str());
  g_alpha = a * e;
  TextCentered(g_fonts.value, ImVec2(center.x + sw.dir * dist * (1.0f - e), center.y), White(), now.c_str());
  g_alpha = a;
}

void Switch(Row& row, bool* value) {
  // Roblox shows on/off settings as a picker: ‹  On  ›
  const bool before = *value;
  const float mid_x = (row.ctrl_l + row.ctrl_r) / 2;
  bool hovered;
  float dir = 0;
  ImGui::PushID(row.id);
  if (Hit("toggle", ImVec2(row.ctrl_l, row.min.y), ImVec2(row.ctrl_r, row.max.y), &hovered)) {
    dir = ImGui::GetIO().MousePos.x < mid_x ? -1.0f : 1.0f;
  }
  if (row.nav.left) dir = -1.0f;
  if (row.nav.right || row.nav.activate) dir = 1.0f;
  if (dir != 0) *value = !*value;
  const ImGuiID id = ImGui::GetID("value");
  ImGui::PopID();
  const float ch = S(28);
  Chevron(ImVec2(row.ctrl_l + S(16), row.mid), ch, false, White(hovered ? 1.0f : 0.92f));
  Chevron(ImVec2(row.ctrl_r - S(16), row.mid), ch, true, White(hovered ? 1.0f : 0.92f));
  ValueText(id, before ? "On" : "Off", *value ? "On" : "Off", dir, ImVec2(mid_x, row.mid));
}

void Picker(Row& row, const std::vector<std::string>& labels, int* index) {
  const int n = static_cast<int>(labels.size());
  const float zone = S(48);
  const int before = *index;
  bool hover_l, hover_r;
  float dir = 0;
  ImGui::PushID(row.id);
  if (Hit("prev", ImVec2(row.ctrl_l, row.min.y), ImVec2(row.ctrl_l + zone, row.max.y), &hover_l) || row.nav.left) {
    *index = (*index + n - 1) % n;
    dir = -1.0f;
  }
  if (Hit("next", ImVec2(row.ctrl_r - zone, row.min.y), ImVec2(row.ctrl_r, row.max.y), &hover_r) || row.nav.right ||
      row.nav.activate) {
    *index = (*index + 1) % n;
    dir = 1.0f;
  }
  const ImGuiID id = ImGui::GetID("value");
  ImGui::PopID();
  const float ch = S(28);
  Chevron(ImVec2(row.ctrl_l + S(16), row.mid), ch, false, White(hover_l ? 1.0f : 0.92f));
  Chevron(ImVec2(row.ctrl_r - S(16), row.mid), ch, true, White(hover_r ? 1.0f : 0.92f));
  ValueText(id, labels[before], labels[*index], dir, ImVec2((row.ctrl_l + row.ctrl_r) / 2, row.mid));
}

// Ten blocks between a circled − and +; the − hides at the minimum and the
// + at the maximum, with small labels for the two ends underneath.
void Slider(Row& row, double* value, double lo, double hi, double step, const char* low_label,
            const char* high_label) {
  const float bar_l = row.ctrl_l + S(52), bar_r = row.ctrl_r - S(53);
  const float block_h = S(24), gap = S(4);
  const float cy = row.mid - S(6);
  ImGui::PushID(row.id);
  bool hover_minus = false, hover_plus = false, held = false;
  const bool can_lower = *value > lo + step * 0.5, can_raise = *value < hi - step * 0.5;
  if (can_lower && Hit("minus", ImVec2(row.ctrl_l, cy - S(18)), ImVec2(row.ctrl_l + S(36), cy + S(18)), &hover_minus)) {
    *value -= step;
  }
  if (can_raise && Hit("plus", ImVec2(row.ctrl_r - S(36), cy - S(18)), ImVec2(row.ctrl_r, cy + S(18)), &hover_plus)) {
    *value += step;
  }
  Hit("bar", ImVec2(bar_l, cy - block_h / 2 - S(4)), ImVec2(bar_r, cy + block_h / 2 + S(4)), nullptr, &held);
  ImGui::PopID();
  if (held) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - bar_l) / (bar_r - bar_l), 0.0f, 1.0f);
    *value = lo + std::round(t * (hi - lo) / step) * step;
  }
  if (row.nav.left) *value -= step;
  if (row.nav.right) *value += step;
  *value = std::clamp(std::round(*value / step) * step, lo, hi);

  if (*value > lo + step * 0.5) CircleSign(ImVec2(row.ctrl_l + S(16), cy), false, White(hover_minus ? 1.0f : 0.85f));
  if (*value < hi - step * 0.5) CircleSign(ImVec2(row.ctrl_r - S(17), cy), true, White(hover_plus ? 1.0f : 0.85f));
  const int blocks = 10;
  const float bw = (bar_r - bar_l - gap * (blocks - 1)) / blocks;
  const float filled = static_cast<float>((*value - lo) / (hi - lo)) * blocks;
  // One bar split into square blocks; only its two ends are rounded.
  for (int i = 0; i < blocks; ++i) {
    const float x = bar_l + i * (bw + gap);
    const ImDrawFlags corners = i == 0 ? ImDrawFlags_RoundCornersLeft
                                : i == blocks - 1 ? ImDrawFlags_RoundCornersRight
                                                  : ImDrawFlags_RoundCornersNone;
    g_dl->AddRectFilled(ImVec2(x, cy - block_h / 2), ImVec2(x + bw, cy + block_h / 2),
                        filled >= i + 0.5f ? BlockOn() : BlockOff(), S(7), corners);
  }
  const float label_y = cy + block_h / 2 + S(7);
  Text(g_fonts.tiny, ImVec2(bar_l - S(2), label_y), Muted(), low_label);
  const ImVec2 hs = Measure(g_fonts.tiny, high_label);
  Text(g_fonts.tiny, ImVec2(bar_r - hs.x + S(2), label_y), Muted(), high_label);
}

// A dark rounded box like Roblox's dropdowns, holding a text field.
bool TextField(Row& row, const char* id, char* buf, size_t size, const char* placeholder, bool* done) {
  const float h = S(42);
  const ImVec2 min(row.ctrl_l, row.mid - h / 2);
  const float width = row.ctrl_r - row.ctrl_l;
  if (row.nav.activate) ImGui::SetKeyboardFocusHere();
  ImGui::PushFont(g_fonts.value);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(15), (h - g_fonts.value->FontSize) / 2));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, C4(35, 37, 39));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, C4(42, 44, 47));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, C4(42, 44, 47));
  ImGui::PushStyleColor(ImGuiCol_Text, C4(255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImGui::ColorConvertU32ToFloat4(C(51, 95, 255, 0.5f)));
  ImGui::SetCursorScreenPos(min);
  ImGui::SetNextItemWidth(width);
  const bool enter = ImGui::InputText(id, buf, size, ImGuiInputTextFlags_EnterReturnsTrue);
  if (done) *done = enter || ImGui::IsItemDeactivatedAfterEdit();
  if (ImGui::IsItemActive()) g_editing = true;
  if (buf[0] == 0 && !ImGui::IsItemActive()) {
    Text(g_fonts.value, ImVec2(min.x + S(15), row.mid - g_fonts.value->FontSize / 2), White(0.45f), placeholder);
  }
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(4);
  ImGui::PopFont();
  return enter;
}

void StatusDot(Row& row, bool on) {
  g_dl->AddCircleFilled(ImVec2(row.ctrl_r - S(8), row.mid), S(6), on ? C(57, 194, 113) : C(110, 112, 118));
}

// --------------------------------------------------------------------------
// Pages

int g_tab = 0;
char g_test[256] = "Hey there! How's it going?";
char g_pack[512] = "";
bool g_pack_loaded = false;
char g_piper_model[512] = "";
bool g_piper_model_loaded = false;
char g_piper_dir[512] = "";
bool g_piper_dir_loaded = false;

template <typename T, typename F>
std::vector<std::string> Labels(const std::vector<T>& items, F label) {
  std::vector<std::string> out;
  for (const T& item : items) out.push_back(label(item));
  return out;
}

int IndexOf(const std::vector<std::string>& keys, const std::string& key) {
  const auto it = std::find(keys.begin(), keys.end(), key);
  return it == keys.end() ? 0 : static_cast<int>(it - keys.begin());
}

void TTSPage(Config& cfg) {
  Header("Text-to-Speech", true);
  Row row = BeginRow("Speak My Chat Messages", "Read chat aloud in Roblox voice chat and/or speakers");
  Switch(row, &cfg.enabled);
  EndRow(row);

  const std::vector<std::string> engines = {"piper", "espeak", "animalese"};
  int engine_idx = IndexOf(engines, cfg.engine);
  row = BeginRow("Speech Engine", "Select your local TTS backend");
  Picker(row, {"Piper (Neural)", "eSpeak-NG", "Animal Crossing"}, &engine_idx);
  cfg.engine = engines[engine_idx];
  EndRow(row);

  if (cfg.engine == "piper") {
    Header("Piper Neural Voice Settings");
    auto piper_models = DiscoverPiperModels(cfg.piper_dir);
    if (!piper_models.empty()) {
      std::vector<std::string> model_labels;
      std::vector<std::string> model_paths;
      for (const auto& m : piper_models) {
        model_labels.push_back(m.name);
        model_paths.push_back(m.path);
      }
      int m_idx = IndexOf(model_paths, cfg.piper_model);
      row = BeginRow("Model Voice", "Locally installed Piper voice model");
      Picker(row, model_labels, &m_idx);
      cfg.piper_model = model_paths[m_idx];
      EndRow(row);
    } else {
      row = BeginRow("Piper Models", "No models found in ~/.local/share/piper-voices");
      EndRow(row);
    }

    if (!g_piper_model_loaded) {
      std::snprintf(g_piper_model, sizeof(g_piper_model), "%s", cfg.piper_model.c_str());
      g_piper_model_loaded = true;
    }
    row = BeginRow("Custom Model File", "Direct path to an .onnx model file");
    bool done = false;
    TextField(row, "##piper_model_input", g_piper_model, sizeof(g_piper_model), "Auto-detect or custom path", &done);
    if (done) cfg.piper_model = g_piper_model;
    EndRow(row);

    char sub[64];
    std::snprintf(sub, sizeof(sub), "%.0f%% of normal pace", cfg.piper_speed * 100);
    row = BeginRow("Speed", sub);
    Slider(row, &cfg.piper_speed, 0.5, 2.0, 0.05, "Slower", "Faster");
    EndRow(row);

    const bool piper_ok = IsPiperInstalled();
    row = BeginRow("Piper Binary", piper_ok ? "Found in PATH" : "Not found; install piper-tts or place in ~/.local/bin");
    StatusDot(row, piper_ok);
    EndRow(row);
  } else if (cfg.engine == "espeak") {
    Header("eSpeak-NG Settings");
    const auto& espeak_voices = GetESpeakVoices();
    std::vector<std::string> voice_labels;
    std::vector<std::string> voice_codes;
    for (const auto& v : espeak_voices) {
      voice_labels.push_back(v.name);
      voice_codes.push_back(v.code);
    }
    int v_idx = IndexOf(voice_codes, cfg.espeak_voice);
    row = BeginRow("Voice / Accent", "Language and accent style");
    Picker(row, voice_labels, &v_idx);
    cfg.espeak_voice = voice_codes[v_idx];
    EndRow(row);

    char sub[64];
    std::snprintf(sub, sizeof(sub), "%.0f words per minute", cfg.espeak_speed);
    row = BeginRow("Speed", sub);
    Slider(row, &cfg.espeak_speed, 80, 350, 5, "Slower", "Faster");
    EndRow(row);

    std::snprintf(sub, sizeof(sub), "Pitch: %.0f", cfg.espeak_pitch);
    row = BeginRow("Pitch", sub);
    Slider(row, &cfg.espeak_pitch, 0, 99, 1, "Lower", "Higher");
    EndRow(row);
  } else if (cfg.engine == "animalese") {
    Header("Animal Crossing Settings");

    const auto voice_keys = Labels(Voices(), [](const VoiceInfo& v) { return std::string(v.key); });
    int voice = IndexOf(voice_keys, cfg.voice);
    row = BeginRow("Personality", "Each villager type has its own pitch and pace");
    Picker(row, Labels(Voices(), [](const VoiceInfo& v) { return std::string(v.label); }), &voice);
    cfg.voice = voice_keys[voice];
    EndRow(row);

    const std::vector<std::string> lang_keys = {"english", "korean"};
    int lang_idx = IndexOf(lang_keys, cfg.animalese_lang);
    row = BeginRow("Language", "Authentic English or Korean villager sound bank");
    Picker(row, {"English", "Korean"}, &lang_idx);
    cfg.animalese_lang = lang_keys[lang_idx];
    EndRow(row);

    char sub[64];
    std::snprintf(sub, sizeof(sub), "%+.0f semitones", cfg.pitch);
    row = BeginRow("Pitch", sub);
    Slider(row, &cfg.pitch, -12, 12, 1, "Lower", "Higher");
    EndRow(row);

    std::snprintf(sub, sizeof(sub), "%.0f%% of the villager's pace", cfg.speed * 100);
    row = BeginRow("Speed", sub);
    Slider(row, &cfg.speed, 0.5, 2.0, 0.05, "Slower", "Faster");
    EndRow(row);

    if (!g_pack_loaded) {
      std::snprintf(g_pack, sizeof(g_pack), "%s", cfg.sample_pack.c_str());
      g_pack_loaded = true;
    }
    row = BeginRow("Letter Samples", "a.wav to z.wav for the exact game sound (Classic only)");
    bool done = false;
    TextField(row, "##pack", g_pack, sizeof(g_pack), "Built-in voice", &done);
    if (done) cfg.sample_pack = g_pack;
    EndRow(row);
  }

  Header("Try It");
  row = BeginRow("Test Message", "Press Enter to hear the voice");
  if (TextField(row, "##test_tts", g_test, sizeof(g_test), "Type something", nullptr) && g_test[0]) Say(g_test, true);
  EndRow(row);
}

void OutputPage(Config& cfg) {
  Header("Where the Voice Plays", true);
  const std::vector<std::string> keys = {"vc", "both", "local"};
  int output = IndexOf(keys, cfg.output);
  Row row = BeginRow("Play the Voice In", "Choose who hears it");
  Picker(row, {"Voice Chat", "Voice Chat and Me", "Only Me"}, &output);
  cfg.output = keys[output];
  EndRow(row);

  char sub[48];
  std::snprintf(sub, sizeof(sub), "%.0f%%", cfg.vc_volume * 100);
  row = BeginRow("Voice Chat Volume", sub);
  Slider(row, &cfg.vc_volume, 0, 1.5, 0.05, "Quiet", "Loud");
  EndRow(row);

  std::snprintf(sub, sizeof(sub), "%.0f%%", cfg.local_volume * 100);
  row = BeginRow("My Speakers Volume", sub);
  Slider(row, &cfg.local_volume, 0, 1.5, 0.05, "Quiet", "Loud");
  EndRow(row);

  row = BeginRow("Keep My Microphone On", "Off: voice chat only hears the TTS voice");
  Switch(row, &cfg.mix_real_mic);
  EndRow(row);

  Header("Status");
  const bool mic = MicInUse();
  row = BeginRow("Voice Chat", mic ? "Your mic is open, so speech goes straight into voice chat"
                                   : "Not in voice chat right now. Join a voice server and unmute");
  StatusDot(row, mic);
  EndRow(row);
}

void SettingsPage(Config& cfg) {
  Header("Chat", true);
  Row row = BeginRow("Intercept Chat", "Speak messages via TTS instead of sending them to Roblox chat");
  Switch(row, &cfg.intercept_chat);
  EndRow(row);

  row = BeginRow("Skip Chat Commands", "Don't speak messages that start with /, like /e dance");
  Switch(row, &cfg.skip_commands);
  EndRow(row);

  const std::vector<std::string> keys = {"left", "right", "off"};
  int where = IndexOf(keys, cfg.menu_button);
  row = BeginRow("Menu Button", "F8 always opens this menu");
  Picker(row, {"Top Left", "Top Right", "Hidden"}, &where);
  cfg.menu_button = keys[where];
  EndRow(row);

  Header("Piper Models Directory");
  if (!g_piper_dir_loaded) {
    std::snprintf(g_piper_dir, sizeof(g_piper_dir), "%s", cfg.piper_dir.c_str());
    g_piper_dir_loaded = true;
  }
  row = BeginRow("Custom Voices Folder", "Additional directory to scan for .onnx models");
  bool done = false;
  TextField(row, "##piper_dir", g_piper_dir, sizeof(g_piper_dir), "Default (~/.local/share/piper-voices)", &done);
  if (done) cfg.piper_dir = g_piper_dir;
  EndRow(row);

  Header("About");
  const std::string last = LastMessage();
  row = BeginRow("Vox TTS", last.empty() ? "Nothing spoken yet" : ("Last spoken: " + last).c_str());
  EndRow(row);
}

bool Same(const Config& a, const Config& b) {
  return a.enabled == b.enabled && a.intercept_chat == b.intercept_chat && a.output == b.output && a.engine == b.engine &&
         a.piper_model == b.piper_model && a.piper_dir == b.piper_dir &&
         a.piper_speed == b.piper_speed && a.espeak_voice == b.espeak_voice &&
         a.espeak_speed == b.espeak_speed && a.espeak_pitch == b.espeak_pitch &&
         a.voice == b.voice && a.style == b.style && a.animalese_lang == b.animalese_lang &&
         a.pitch == b.pitch && a.speed == b.speed && a.vc_volume == b.vc_volume &&
         a.local_volume == b.local_volume && a.mix_real_mic == b.mix_real_mic &&
         a.skip_commands == b.skip_commands && a.sample_pack == b.sample_pack &&
         a.menu_button == b.menu_button;
}

// --------------------------------------------------------------------------
// The menu
//
// Timings follow Roblox's menu, measured from a 60 fps recording: it opens
// by sliding up from below the screen (~0.18 s, decelerating) while the
// game dims; pages slide sideways (~0.12 s), the new one coming from the
// side of the tab you picked; closing drops the dim and the page at once
// and the empty panel falls off the bottom (~0.35 s, accelerating).

float g_opened_at = -10;
float g_closed_at = -10;
float g_switched_at = -10;
int g_prev_tab = 0;

constexpr float kOpenTime = 0.18f;
constexpr float kPageTime = 0.12f;
constexpr float kCloseTime = 0.35f;
constexpr float kDim = 0.45f;

float EaseOutQuart(float since, float seconds) {
  const float t = std::clamp((Now() - since) / seconds, 0.0f, 1.0f);
  const float u = 1.0f - t;
  return 1.0f - u * u * u * u;
}

bool Closing() { return Now() - g_closed_at < kCloseTime; }

void SwitchTab(int tab) {
  if (tab == g_tab) return;
  g_prev_tab = g_tab;
  g_tab = tab;
  g_switched_at = Now();
  g_nav.focus = -1;
}

void DrawPage(int tab, Config& cfg) {
  if (tab == 0) TTSPage(cfg);
  if (tab == 1) OutputPage(cfg);
  if (tab == 2) SettingsPage(cfg);
  ImGui::Dummy(ImVec2(1, S(8)));
}

struct PanelRect {
  ImVec2 min, max;
};
PanelRect g_panel;

void DrawMenu(Config& cfg, float W, float H, bool closing) {
  const float pw = std::min(S(864), W - S(32));
  const float ph = std::min(S(736), H - S(110));
  const float rest_y = std::round(std::max(S(64), (H - ph) / 2 + S(20)));
  float y = rest_y, dim = 0;
  if (!closing) {
    const float e = EaseOutQuart(g_opened_at, kOpenTime);
    y = std::round(H + (rest_y - H) * e);  // top edge starts at the bottom of the screen
    dim = kDim * e;
  } else {
    const float t = std::clamp((Now() - g_closed_at) / kCloseTime, 0.0f, 1.0f);
    y = std::round(rest_y + (H - rest_y) * t * t);
  }
  if (dim > 0) ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), C(0, 0, 0, dim));
  const ImVec2 p0(std::round((W - pw) / 2), y);
  const ImVec2 p1(p0.x + pw, p0.y + ph);
  g_panel = {p0, p1};
  g_dl->AddRectFilled(p0, p1, Panel(), S(8));

  // Tabs: Roblox icon + label; the active one is white with an underline.
  struct Tab { const char* label; const char* icon; } tabs[] = {
      {"TTS", "frame-bubble-soundwave"}, {"Output", "speaker"}, {"Settings", "gear"}};
  constexpr int kNumTabs = 3;
  const float inset = S(20), tab_h = S(60);
  const float tab_w = (pw - inset * 2) / kNumTabs;
  for (int i = 0; i < kNumTabs; ++i) {
    const ImVec2 a(p0.x + inset + i * tab_w, p0.y), b(a.x + tab_w, p0.y + tab_h);
    bool hovered = false;
    if (!closing) {
      ImGui::PushID(i);
      if (Hit("tab", a, b, &hovered)) SwitchTab(i);
      ImGui::PopID();
    }
    const bool active = g_tab == i;
    const ImU32 col = active ? White() : (hovered ? White(0.9f) : Muted());
    const ImVec2 ts = Measure(g_fonts.tab, tabs[i].label);
    const float icon = S(24), gap = S(10);
    const float x = (a.x + b.x) / 2 - (icon + gap + ts.x) / 2;
    const ImVec2 ic(x + icon / 2, (a.y + b.y) / 2);
    if (!Icon(g_dl, tabs[i].icon, ic, S(26), col)) {
      if (i == 0) BubbleIcon(g_dl, ic, icon, col, S(1.6f), false, -1);
      if (i == 1) SpeakerIcon(ic, icon, col, S(1.6f));
      if (i == 2) GearIcon(ic, icon, col, S(1.6f));
    }
    Text(g_fonts.tab, ImVec2(x + icon + gap, ic.y - ts.y / 2), col, tabs[i].label);
  }
  // The thin line under the whole tab row; the active underline sits on it.
  g_dl->AddRectFilled(ImVec2(p0.x + inset, p0.y + tab_h - S(1)), ImVec2(p1.x - inset, p0.y + tab_h), C(51, 51, 51));
  if (!closing) {
    const ImVec2 a(p0.x + inset + g_tab * tab_w, p0.y + tab_h - S(2));
    g_dl->AddRectFilled(a, ImVec2(a.x + tab_w, p0.y + tab_h), White());
  }
  // The page and buttons vanish as soon as the menu closes.
  if (closing && Now() - g_closed_at > 0.1f) return;

  // Q / E switch pages too, like the gamepad bumpers.
  if (!closing && !g_editing && (ImGui::IsKeyPressed(ImGuiKey_Q) || ImGui::IsKeyPressed(ImGuiKey_E))) {
    SwitchTab((g_tab + (ImGui::IsKeyPressed(ImGuiKey_E) ? 1 : 2)) % 3);
  }

  // Page content in a clipped viewport; on a page switch the old page slides
  // out one way while the new one slides in from the other.
  const float bar_h = S(48), bar_y = p1.y - S(20) - bar_h;
  // The area starts a few pixels outside the rows so the selection outline,
  // drawn just outside a row, isn't clipped.
  const float pad = S(6);
  const ImVec2 c0(p0.x + S(32) - pad, p0.y + tab_h + S(10) - pad);
  const ImVec2 csize(pw - S(32) - S(17) + pad, bar_y - S(12) - c0.y);
  const float e = EaseOutQuart(g_switched_at, kPageTime);
  const float dir = g_tab > g_prev_tab ? 1.0f : -1.0f;
  const bool sliding = e < 1.0f && g_prev_tab != g_tab;

  ImGui::SetCursorScreenPos(c0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("viewport", csize, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(3));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, C4(128, 128, 128));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, C4(160, 160, 160));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, C4(190, 190, 190));
  ImDrawList* outer = g_dl;
  auto page = [&](int tab, float offset, bool live) {
    char id[16];
    std::snprintf(id, sizeof(id), "page%d", tab);
    ImGui::SetCursorScreenPos(ImVec2(c0.x + std::round(offset), c0.y));
    ImGui::BeginChild(id, csize, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    g_dl = ImGui::GetWindowDrawList();
    if (live && !closing) {
      DrawPage(tab, cfg);
    } else {
      // A page sliding away (or the closing frame): drawn, not interactive.
      Config scratch = cfg;
      g_nav_off = true;
      ImGui::BeginDisabled();
      DrawPage(tab, scratch);
      ImGui::EndDisabled();
      g_nav_off = false;
    }
    ImGui::EndChild();
  };
  if (sliding) page(g_prev_tab, -dir * e * csize.x, false);
  page(g_tab, sliding ? dir * (1.0f - e) * csize.x : 0.0f, true);
  g_dl = outer;
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(5);
  ImGui::EndChild();

  // Bottom buttons, like Leave / Respawn / Resume.
  const float gap = S(8);
  const float bw = (pw - inset * 2 - gap * 2) / 3;
  struct Button { const char* label; const char* icon; bool primary; } buttons[] = {
      {"Test Voice", "play-large", false}, {"Stop", "stop-large", false}, {"Close", nullptr, true}};
  for (int i = 0; i < 3; ++i) {
    const Button& bt = buttons[i];
    const ImVec2 a(p0.x + inset + i * (bw + gap), bar_y), b(a.x + bw, bar_y + bar_h);
    bool hovered = false, clicked = false;
    NavItem nav;
    if (!closing) {
      ImGui::PushID(100 + i);
      clicked = Hit("button", a, b, &hovered);
      ImGui::PopID();
      nav = RegisterNav(a, b);
      clicked = clicked || nav.activate;
      if (!g_editing && i < 2 && ImGui::IsKeyPressed(i == 0 ? ImGuiKey_T : ImGuiKey_S)) clicked = true;
    }
    g_dl->AddRectFilled(a, b, bt.primary ? (hovered ? PrimaryHover() : Primary()) : (hovered ? ButtonHover() : ButtonBg()),
                        S(8));
    if (nav.focused) SelectionOutline(a, b, S(8));
    const ImVec2 ts = Measure(g_fonts.button, bt.label);
    const float lead = S(28), lgap = S(12);
    const float x = (a.x + b.x) / 2 - (lead + lgap + ts.x) / 2;
    const ImVec2 lc(x + lead / 2, (a.y + b.y) / 2);
    if (bt.icon == nullptr) {
      KeyHint(lc, "ESC", White());
    } else if (!Icon(g_dl, bt.icon, lc, S(24), White())) {
      KeyHint(lc, i == 0 ? "T" : "S", White());
    }
    Text(g_fonts.button, ImVec2(x + lead + lgap, (a.y + b.y) / 2 - ts.y / 2), White(), bt.label);
    if (clicked) {
      if (i == 0 && g_test[0]) Say(g_test, true);
      if (i == 1) StopSpeaking();
      if (i == 2) overlay::RequestClose();
    }
  }
}

// The top-bar button: a solid circle like Roblox's own top-bar buttons.
// Its icon grows while the menu is open (the circle stays the same).
float g_icon_scale = 1.0f;

void DrawTopBarButton(bool open, bool closing, float W) {
  const overlay::Metrics m = overlay::GetMetrics();
  float x, y, size;
  if (!overlay::ButtonRect(m.window_w > 0 ? m.window_w : static_cast<int>(W / g_px), &x, &y, &size)) return;
  const bool forced = std::getenv("VOX_TTS_SHOW_BUTTON") != nullptr || std::getenv("NIGHTCAP_AC_SHOW_BUTTON") != nullptr;
  if (!open && !closing && !forced && !InGame()) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 c(S(x + size / 2), S(y + size / 2));
  const float r = S(size / 2);
  const ImVec2 mouse = open ? ImGui::GetIO().MousePos : ImVec2(m.mouse_x * g_px, m.mouse_y * g_px);
  const bool hovered = (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) <= r * r;
  const float target = open ? 1.18f : 1.0f;
  g_icon_scale += (target - g_icon_scale) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
  float scale = g_icon_scale;
  if (IsSpeaking()) scale *= 1.0f + 0.04f * std::sin(Now() * 12.0f);  // talking
  dl->AddCircleFilled(c, r, hovered ? C(27, 28, 33) : C(6, 6, 8), 48);
  if (!Icon(dl, "frame-bubble-soundwave", c, S(26) * scale, White())) {
    BubbleIcon(dl, c, S(22) * scale, White(), S(1.8f), true, IsSpeaking() ? Now() : -1.0f);
  }
  if (open) {
    ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
    if (ImGui::InvisibleButton("topbar", ImVec2(r * 2, r * 2))) overlay::RequestClose();
  }
}

bool g_was_open = false;

}  // namespace

void ResetFonts() { g_fonts.px = 0; }

bool PrepareFonts(float px) {
  if (g_fonts.px > 0 && std::fabs(g_fonts.px - px) < 0.01f) return false;
  g_fonts.px = px;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  const std::string dir = FindRobloxFonts();
  if (g_fonts.family != dir) {
    Log(dir.empty() ? "overlay: Builder Sans not found, using a fallback font"
                    : "overlay: using Roblox's Builder Sans from %s", dir.c_str());
    g_fonts.family = dir;
  }
  const char* fallback = "/usr/share/fonts/noto/NotoSans-Medium.ttf";
  // Sizes match Roblox's menu text (its cap heights: tabs 11 px, rows and
  // buttons 10 px, headers 14 px, subtitles 7 px; Builder Sans capitals are
  // ~0.56 of the size ImGui is given).
  g_fonts.title = AddFont(dir, "BuilderSans-Medium.otf", std::round(18 * px), fallback);
  g_fonts.value = AddFont(dir, "BuilderSans-Regular.otf", std::round(18 * px), fallback);
  g_fonts.small = AddFont(dir, "BuilderSans-Regular.otf", std::round(13 * px), fallback);
  g_fonts.tiny = AddFont(dir, "BuilderSans-Regular.otf", std::round(12.5f * px), fallback);
  g_fonts.tab = AddFont(dir, "BuilderSans-Medium.otf", std::round(20 * px), fallback);
  g_fonts.header = AddFont(dir, "BuilderSans-Bold.otf", std::round(25 * px), fallback);
  g_fonts.button = AddFont(dir, "BuilderSans-Bold.otf", std::round(18 * px), fallback);
  g_fonts.hint = AddFont(dir, "BuilderSans-Bold.otf", std::round(10 * px), fallback);
  io.FontDefault = g_fonts.value;

  // Roblox's icons, baked large and drawn at the sizes each spot needs.
  g_icon_font = nullptr;
  const std::string icon_path = FindRobloxIconFont();
  if (!icon_path.empty() && (g_icons.path() == icon_path || g_icons.Load(icon_path))) {
    static ImVector<ImWchar> ranges;
    ranges.clear();
    for (const char* name : {"frame-bubble-soundwave", "speaker", "gear", "chevron-large-left",
                             "chevron-large-right", "circle-minus", "circle-plus", "square-text-esc",
                             "play-large", "stop-large"}) {
      if (const uint32_t cp = g_icons.Find(name); cp != 0 && cp <= 0xFFFF) {
        ranges.push_back(static_cast<ImWchar>(cp));
        ranges.push_back(static_cast<ImWchar>(cp));
      }
    }
    ranges.push_back(0);
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    g_icon_font = io.Fonts->AddFontFromFileTTF(icon_path.c_str(), std::round(40 * px), &cfg, ranges.Data);
  }
  if (g_icon_font == nullptr) Log("overlay: Roblox's icon font not found, drawing icons instead");
  return true;
}
void FeedInput(const std::vector<overlay::Input>& input, float px) {
  ImGuiIO& io = ImGui::GetIO();
  for (const overlay::Input& in : input) {
    switch (in.type) {
      case overlay::Input::kMousePos:
        io.AddMousePosEvent(in.x * px, in.y * px);
        break;
      case overlay::Input::kMouseButton:
        io.AddMousePosEvent(in.x * px, in.y * px);
        io.AddMouseButtonEvent(in.button, in.down);
        break;
      case overlay::Input::kWheel:
        io.AddMouseWheelEvent(in.x, in.y);
        break;
      case overlay::Input::kText:
        io.AddInputCharactersUTF8(in.text.c_str());
        break;
      case overlay::Input::kKey: {
        io.AddKeyEvent(ImGuiMod_Ctrl, (in.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (in.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (in.mod & SDL_KMOD_ALT) != 0);
        ImGuiKey key = ImGuiKey_None;
        switch (in.key) {
          case SDLK_TAB: key = ImGuiKey_Tab; break;
          case SDLK_LEFT: key = ImGuiKey_LeftArrow; break;
          case SDLK_RIGHT: key = ImGuiKey_RightArrow; break;
          case SDLK_UP: key = ImGuiKey_UpArrow; break;
          case SDLK_DOWN: key = ImGuiKey_DownArrow; break;
          case SDLK_HOME: key = ImGuiKey_Home; break;
          case SDLK_END: key = ImGuiKey_End; break;
          case SDLK_DELETE: key = ImGuiKey_Delete; break;
          case SDLK_BACKSPACE: key = ImGuiKey_Backspace; break;
          case SDLK_RETURN: key = ImGuiKey_Enter; break;
          case SDLK_SPACE: key = ImGuiKey_Space; break;
          case SDLK_Q: key = ImGuiKey_Q; break;
          case SDLK_E: key = ImGuiKey_E; break;
          case SDLK_S: key = ImGuiKey_S; break;
          case SDLK_T: key = ImGuiKey_T; break;
          case SDLK_KP_ENTER: key = ImGuiKey_KeypadEnter; break;
          case SDLK_A: key = ImGuiKey_A; break;
          case SDLK_C: key = ImGuiKey_C; break;
          case SDLK_V: key = ImGuiKey_V; break;
          case SDLK_X: key = ImGuiKey_X; break;
          case SDLK_Z: key = ImGuiKey_Z; break;
          default: break;
        }
        if (key != ImGuiKey_None) io.AddKeyEvent(key, in.down);
        break;
      }
    }
  }
}

bool Draw(float px, bool srgb) {
  g_px = px;
  g_srgb = srgb;
  ImGuiIO& io = ImGui::GetIO();
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  const bool open = overlay::MenuOpen();
  if (!open) {
    // No input reaches the menu while it is closed; clear stale state.
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, false);
  }
  if (open && !g_was_open) {
    g_opened_at = Now();
    g_closed_at = -10;
    g_prev_tab = g_tab;
    g_nav = Nav{};
  } else if (!open && g_was_open) {
    g_closed_at = Now();
  }
  g_was_open = open;
  const bool closing = !open && Closing();
  Config cfg = GetConfig();
  const Config before = cfg;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(W, H));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
  if (!open) flags |= ImGuiWindowFlags_NoInputs;
  ImGui::Begin("##vox-tts", nullptr, flags);
  g_dl = ImGui::GetWindowDrawList();
  if (open) {
    UpdateNav();
    DrawMenu(cfg, W, H, false);
    g_editing = io.WantTextInput;
    // Clicking outside the panel closes the menu, like Roblox's.
    const ImVec2 mp = io.MousePos;
    const bool inside = mp.x >= g_panel.min.x && mp.x < g_panel.max.x && mp.y >= g_panel.min.y && mp.y < g_panel.max.y;
    if (ImGui::IsMouseClicked(0) && !inside) overlay::RequestClose();
  } else if (closing) {
    DrawMenu(cfg, W, H, true);
  }
  DrawTopBarButton(open, closing, W);
  ImGui::End();
  ImGui::PopStyleVar(2);

  overlay::SetWantsText(open && io.WantTextInput);
  if (!Same(cfg, before)) SetConfig(cfg);
  SaveConfigIfDirty();
  const bool button_on = cfg.menu_button != "off" &&
                         (InGame() || std::getenv("VOX_TTS_SHOW_BUTTON") || std::getenv("NIGHTCAP_AC_SHOW_BUTTON"));
  return open || closing || button_on;
}

}  // namespace nac::ui
