#include "overlay.h"

#include <mutex>

#include "app.h"

namespace nac::overlay {
namespace {

std::mutex g_mutex;
std::vector<Input> g_input;
Metrics g_metrics;
std::atomic<bool> g_open{false};
std::atomic<bool> g_close_requested{false};
std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_wants_text{false};

}  // namespace

void PushInput(const Input& input) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_input.size() < 4096) g_input.push_back(input);
}

std::vector<Input> TakeInput() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Input> out;
  out.swap(g_input);
  return out;
}

bool MenuOpen() { return g_open.load(); }
void RequestClose() { g_close_requested = true; }
bool TakeCloseRequest() { return g_close_requested.exchange(false); }
void RequestOpen() { g_open_requested = true; }
bool TakeOpenRequest() { return g_open_requested.exchange(false); }

void SetMenuOpenFlag(bool open) {
  g_open = open;
  if (!open) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_input.clear();
  }
}

void SetWantsText(bool wants) { g_wants_text = wants; }
bool WantsText() { return g_wants_text.load(); }

Metrics GetMetrics() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_metrics;
}

void SetWindowSize(int w, int h) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_metrics.window_w = w;
  g_metrics.window_h = h;
}

void SetMousePos(float x, float y) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_metrics.mouse_x = x;
  g_metrics.mouse_y = y;
}

bool ButtonRect(int window_w, float* x, float* y, float* size) {
  const std::string where = GetConfig().menu_button;
  if (where == "off") return false;
  // Roblox's top bar: a 44 px tall row 12 px from the top. The Roblox
  // button and the menu/chat/mic pill end around x = 208, so ours sits just
  // after them, or mirrored on the right.
  *size = 44;
  *y = 12;
  *x = where == "right" ? window_w - 16 - *size : 216;
  return true;
}

}  // namespace nac::overlay
