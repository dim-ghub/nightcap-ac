// State shared between the SDL side (main thread: events, cursor) and the
// Vulkan layer (present thread: drawing the menu).

#ifndef NIGHTCAP_AC_OVERLAY_H_
#define NIGHTCAP_AC_OVERLAY_H_

#include <SDL3/SDL.h>

#include <atomic>
#include <string>
#include <vector>

namespace nac::overlay {

// Input captured while the menu is open, in window (logical) coordinates.
struct Input {
  enum Type { kMousePos, kMouseButton, kWheel, kKey, kText } type;
  float x = 0, y = 0;      // kMousePos, kWheel (x/y deltas)
  int button = 0;          // kMouseButton: 0 left, 1 right, 2 middle
  bool down = false;       // kMouseButton, kKey
  SDL_Keycode key = 0;     // kKey
  SDL_Keymod mod = 0;      // kKey
  std::string text;        // kText
};

void PushInput(const Input& input);
std::vector<Input> TakeInput();

// Menu visibility. Only the main thread opens or closes the menu (it owns
// the cursor); the menu asks with RequestClose().
bool MenuOpen();
void RequestClose();
bool TakeCloseRequest();
// From the launcher: open the menu (and raise the window).
void RequestOpen();
bool TakeOpenRequest();
void SetMenuOpenFlag(bool open);

// The menu wants keyboard text (a text field is focused).
void SetWantsText(bool wants);
bool WantsText();

// Window size in logical units and the last mouse position, for mapping
// input onto the swapchain.
struct Metrics {
  int window_w = 0, window_h = 0;
  float mouse_x = -1, mouse_y = -1;
};
Metrics GetMetrics();
void SetWindowSize(int w, int h);
void SetMousePos(float x, float y);

// Where the top-bar menu button sits, in logical coordinates; false when the
// button is turned off.
bool ButtonRect(int window_w, float* x, float* y, float* size);

}  // namespace nac::overlay

#endif  // NIGHTCAP_AC_OVERLAY_H_
