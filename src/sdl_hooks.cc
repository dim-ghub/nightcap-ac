// SDL3 wrappers. This library is LD_PRELOADed into Nightcap, so these
// definitions come before libSDL3's. Each one calls the real SDL function and
// adds a little on top:
//
//   * Text input: track the focused Roblox text box to know what you sent.
//   * Events: open/close the menu (F8 or the top-bar button) and, while it
//     is open, give mouse and keyboard to the menu instead of the game.
//   * Mouse mode: while the menu is open, keep the cursor free and visible
//     even if Nightcap asks for first-person mouse lock; restore it after.
//   * Audio: remember which streams are microphones and mix speech into
//     what Nightcap reads from them.

#include <SDL3/SDL.h>

#include <mutex>
#include <unordered_set>

#include "app.h"
#include "chat.h"
#include "control.h"
#include "overlay.h"
#include "speech.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace nac {
namespace {

// ---------------------------------------------------------------------------
// Chat

ChatTracker g_chat;              // main thread only
bool g_roblox_text_active = false;

void OnSent(const std::string& message) {
  const Config cfg = GetConfig();
  if (!cfg.enabled) return;
  if (cfg.skip_commands && !message.empty() && message[0] == '/') return;
  Log("speaking a chat message (%zu bytes)", message.size());
  Say(message);
}

void ObserveChat(const SDL_Event& e) {
  if (!g_chat.active()) return;
  if (e.type == SDL_EVENT_TEXT_INPUT && e.text.text != nullptr) {
    g_chat.Text(e.text.text);
    return;
  }
  if (e.type != SDL_EVENT_KEY_DOWN) return;
  const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
  std::optional<std::string> sent;
  switch (e.key.key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: sent = g_chat.Key(ChatKey::kReturn, ctrl); break;
    case SDLK_ESCAPE: g_chat.Key(ChatKey::kEscape, ctrl); break;
    case SDLK_BACKSPACE: g_chat.Key(ChatKey::kBackspace, ctrl); break;
    case SDLK_DELETE: g_chat.Key(ChatKey::kDelete, ctrl); break;
    case SDLK_LEFT: g_chat.Key(ChatKey::kLeft, ctrl); break;
    case SDLK_RIGHT: g_chat.Key(ChatKey::kRight, ctrl); break;
    case SDLK_HOME: g_chat.Key(ChatKey::kHome, ctrl); break;
    case SDLK_END: g_chat.Key(ChatKey::kEnd, ctrl); break;
    default: break;
  }
  if (sent) OnSent(*sent);
}

// ---------------------------------------------------------------------------
// Menu and cursor

SDL_WindowID g_window_id = 0;
// What Nightcap last asked for while the menu held the cursor.
bool g_want_relative = false;
bool g_want_cursor_visible = true;
bool g_swallow_button_up = false;
bool g_menu_started_text = false;

SDL_Window* Window() { return g_window_id ? SDL_GetWindowFromID(g_window_id) : nullptr; }

void SetMenuOpen(bool open) {
  if (open == overlay::MenuOpen()) return;
  SDL_Window* window = Window();
  if (open) {
    g_want_relative = window && SDL_GetWindowRelativeMouseMode(window);
    g_want_cursor_visible = SDL_CursorVisible();
    overlay::SetMenuOpenFlag(true);
    if (window) Real<bool (*)(SDL_Window*, bool)>("SDL_SetWindowRelativeMouseMode")(window, false);
    Real<bool (*)()>("SDL_ShowCursor")();
  } else {
    overlay::SetMenuOpenFlag(false);
    if (window) {
      Real<bool (*)(SDL_Window*, bool)>("SDL_SetWindowRelativeMouseMode")(window, g_want_relative);
    }
    if (!g_want_cursor_visible) Real<bool (*)()>("SDL_HideCursor")();
    if (g_menu_started_text && !g_roblox_text_active && window) {
      Real<bool (*)(SDL_Window*)>("SDL_StopTextInput")(window);
    }
    g_menu_started_text = false;
    overlay::SetWantsText(false);
  }
}

// Once per event pump: apply requests coming from the menu.
void MainThreadTick() {
  StartControl();
  if (overlay::TakeCloseRequest()) SetMenuOpen(false);
  if (overlay::TakeOpenRequest() && Window() != nullptr) {
    SetMenuOpen(true);
    SDL_RaiseWindow(Window());
  }
  if (!overlay::MenuOpen()) return;
  SDL_Window* window = Window();
  if (window == nullptr) return;
  const bool wants = overlay::WantsText();
  if (wants && !g_menu_started_text && !g_roblox_text_active) {
    g_menu_started_text = Real<bool (*)(SDL_Window*)>("SDL_StartTextInput")(window);
  } else if (!wants && g_menu_started_text) {
    if (!g_roblox_text_active) Real<bool (*)(SDL_Window*)>("SDL_StopTextInput")(window);
    g_menu_started_text = false;
  }
}

bool InButton(float x, float y) {
  const overlay::Metrics m = overlay::GetMetrics();
  float bx, by, size;
  if (!overlay::ButtonRect(m.window_w, &bx, &by, &size)) return false;
  return x >= bx && x < bx + size && y >= by && y < by + size;
}

void TrackWindow(SDL_WindowID id) {
  if (id == 0) return;
  g_window_id = id;
  if (SDL_Window* w = SDL_GetWindowFromID(id)) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    overlay::SetWindowSize(ww, wh);
  }
}

// Returns true when the event belongs to us and must not reach Nightcap.
bool Filter(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: TrackWindow(e.key.windowID); break;
    case SDL_EVENT_MOUSE_MOTION: TrackWindow(e.motion.windowID); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: TrackWindow(e.button.windowID); break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: TrackWindow(e.window.windowID); break;
    default: break;
  }

  const bool open = overlay::MenuOpen();
  if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.key == SDLK_F8) {
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) SetMenuOpen(!open);
    return true;
  }
  if (!open) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) overlay::SetMousePos(e.motion.x, e.motion.y);
    // The top-bar button works whenever the cursor is free (not in
    // first-person mouse lock).
    SDL_Window* window = Window();
    const bool cursor_free = window == nullptr || !SDL_GetWindowRelativeMouseMode(window);
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT &&
        cursor_free && InButton(e.button.x, e.button.y)) {
      g_swallow_button_up = true;
      SetMenuOpen(true);
      return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT &&
        g_swallow_button_up) {
      g_swallow_button_up = false;
      return true;
    }
    ObserveChat(e);
    return false;
  }

  // Menu open: it gets the mouse and keyboard.
  overlay::Input in;
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
      if (e.key.key == SDLK_ESCAPE) {
        SetMenuOpen(false);
        return true;
      }
      [[fallthrough]];
    case SDL_EVENT_KEY_UP:
      in.type = overlay::Input::kKey;
      in.down = e.type == SDL_EVENT_KEY_DOWN;
      in.key = e.key.key;
      in.mod = e.key.mod;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_TEXT_INPUT:
      if (e.text.text != nullptr) {
        in.type = overlay::Input::kText;
        in.text = e.text.text;
        overlay::PushInput(in);
      }
      return true;
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
      return true;
    case SDL_EVENT_MOUSE_MOTION:
      overlay::SetMousePos(e.motion.x, e.motion.y);
      in.type = overlay::Input::kMousePos;
      in.x = e.motion.x;
      in.y = e.motion.y;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      in.type = overlay::Input::kMouseButton;
      in.down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      in.button = e.button.button == SDL_BUTTON_LEFT    ? 0
                  : e.button.button == SDL_BUTTON_RIGHT ? 1
                                                        : 2;
      in.x = e.button.x;
      in.y = e.button.y;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_MOUSE_WHEEL:
      in.type = overlay::Input::kWheel;
      in.x = e.wheel.x;
      in.y = e.wheel.y;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Microphone streams

std::mutex g_audio_mutex;
std::unordered_set<SDL_AudioStream*> g_mic_streams;

bool IsRecording(SDL_AudioDeviceID devid) {
  if (devid == SDL_AUDIO_DEVICE_DEFAULT_RECORDING) return true;
  if (devid == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK || devid == 0) return false;
  return !SDL_IsAudioDevicePlayback(devid);
}

void TrackMic(SDL_AudioStream* stream) {
  std::lock_guard<std::mutex> lock(g_audio_mutex);
  g_mic_streams.insert(stream);
}

bool IsMic(SDL_AudioStream* stream) {
  std::lock_guard<std::mutex> lock(g_audio_mutex);
  return g_mic_streams.count(stream) != 0;
}

template <typename Fn>
Fn Cached(Fn& slot, const char* name) {
  if (slot == nullptr) slot = Real<Fn>(name);
  return slot;
}

}  // namespace
}  // namespace nac

using namespace nac;

// --- text input -------------------------------------------------------------

EXPORT bool SDL_StartTextInputWithProperties(SDL_Window* window, SDL_PropertiesID props) {
  static bool (*fn)(SDL_Window*, SDL_PropertiesID);
  const bool ok = Cached(fn, "SDL_StartTextInputWithProperties")(window, props);
  if (ok) {
    const Sint64 type = SDL_GetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, 0);
    const bool multiline = SDL_GetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
    const bool secret = type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN ||
                        type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE ||
                        type == SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN ||
                        type == SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE;
    g_roblox_text_active = true;
    g_chat.Open(secret, multiline);
  }
  return ok;
}

EXPORT bool SDL_StartTextInput(SDL_Window* window) {
  static bool (*fn)(SDL_Window*);
  const bool ok = Cached(fn, "SDL_StartTextInput")(window);
  if (ok) {
    g_roblox_text_active = true;
    g_chat.Open(false, false);
  }
  return ok;
}

EXPORT bool SDL_StopTextInput(SDL_Window* window) {
  static bool (*fn)(SDL_Window*);
  g_roblox_text_active = false;
  g_chat.Close();
  // Keep text flowing to the menu if one of its fields still has focus.
  if (overlay::MenuOpen() && g_menu_started_text) return true;
  return Cached(fn, "SDL_StopTextInput")(window);
}

EXPORT char* SDL_GetClipboardText(void) {
  static char* (*fn)(void);
  char* text = Cached(fn, "SDL_GetClipboardText")();
  // Roblox reads the clipboard when you paste into a focused box.
  if (text != nullptr && g_chat.active() && !overlay::MenuOpen()) g_chat.Paste(text);
  return text;
}

// --- events -----------------------------------------------------------------

EXPORT bool SDL_PollEvent(SDL_Event* event) {
  static bool (*fn)(SDL_Event*);
  Cached(fn, "SDL_PollEvent");
  MainThreadTick();
  if (event == nullptr) return fn(nullptr);
  while (fn(event)) {
    if (!Filter(*event)) return true;
  }
  return false;
}

EXPORT bool SDL_WaitEvent(SDL_Event* event) {
  static bool (*fn)(SDL_Event*);
  Cached(fn, "SDL_WaitEvent");
  MainThreadTick();
  if (event == nullptr) return fn(nullptr);
  while (fn(event)) {
    if (!Filter(*event)) return true;
  }
  return false;
}

EXPORT bool SDL_WaitEventTimeout(SDL_Event* event, Sint32 timeout_ms) {
  static bool (*fn)(SDL_Event*, Sint32);
  Cached(fn, "SDL_WaitEventTimeout");
  MainThreadTick();
  if (event == nullptr) return fn(nullptr, timeout_ms);
  while (fn(event, timeout_ms)) {
    if (!Filter(*event)) return true;
    timeout_ms = 0;  // one consumed event already used up the wait
  }
  return false;
}

EXPORT int SDL_PeepEvents(SDL_Event* events, int numevents, SDL_EventAction action,
                          Uint32 min_type, Uint32 max_type) {
  static int (*fn)(SDL_Event*, int, SDL_EventAction, Uint32, Uint32);
  const int count = Cached(fn, "SDL_PeepEvents")(events, numevents, action, min_type, max_type);
  // Only removed events count; a peek would see the same event again later.
  if (action != SDL_GETEVENT || events == nullptr || count <= 0) return count;
  MainThreadTick();
  int kept = 0;
  for (int i = 0; i < count; ++i) {
    if (!Filter(events[i])) events[kept++] = events[i];
  }
  return kept;
}

// --- mouse mode while the menu is open -----------------------------------------

EXPORT bool SDL_SetWindowRelativeMouseMode(SDL_Window* window, bool enabled) {
  static bool (*fn)(SDL_Window*, bool);
  if (overlay::MenuOpen()) {
    g_want_relative = enabled;
    return true;
  }
  return Cached(fn, "SDL_SetWindowRelativeMouseMode")(window, enabled);
}

EXPORT bool SDL_GetWindowRelativeMouseMode(SDL_Window* window) {
  static bool (*fn)(SDL_Window*);
  if (overlay::MenuOpen()) return g_want_relative;
  return Cached(fn, "SDL_GetWindowRelativeMouseMode")(window);
}

EXPORT bool SDL_HideCursor(void) {
  static bool (*fn)(void);
  if (overlay::MenuOpen()) {
    g_want_cursor_visible = false;
    return true;
  }
  return Cached(fn, "SDL_HideCursor")();
}

EXPORT bool SDL_ShowCursor(void) {
  static bool (*fn)(void);
  if (overlay::MenuOpen()) {
    g_want_cursor_visible = true;
    return true;
  }
  return Cached(fn, "SDL_ShowCursor")();
}

EXPORT void SDL_WarpMouseInWindow(SDL_Window* window, float x, float y) {
  static void (*fn)(SDL_Window*, float, float);
  if (overlay::MenuOpen()) return;  // first-person re-centering
  Cached(fn, "SDL_WarpMouseInWindow")(window, x, y);
}

// --- microphone ---------------------------------------------------------------

EXPORT SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec,
                                                  SDL_AudioStreamCallback callback, void* userdata) {
  static SDL_AudioStream* (*fn)(SDL_AudioDeviceID, const SDL_AudioSpec*, SDL_AudioStreamCallback, void*);
  SDL_AudioStream* stream = Cached(fn, "SDL_OpenAudioDeviceStream")(devid, spec, callback, userdata);
  if (stream != nullptr && IsRecording(devid)) TrackMic(stream);
  return stream;
}

EXPORT bool SDL_BindAudioStream(SDL_AudioDeviceID devid, SDL_AudioStream* stream) {
  static bool (*fn)(SDL_AudioDeviceID, SDL_AudioStream*);
  const bool ok = Cached(fn, "SDL_BindAudioStream")(devid, stream);
  if (ok && stream != nullptr && IsRecording(devid)) TrackMic(stream);
  return ok;
}

EXPORT bool SDL_BindAudioStreams(SDL_AudioDeviceID devid, SDL_AudioStream* const* streams, int num) {
  static bool (*fn)(SDL_AudioDeviceID, SDL_AudioStream* const*, int);
  const bool ok = Cached(fn, "SDL_BindAudioStreams")(devid, streams, num);
  if (ok && streams != nullptr && IsRecording(devid)) {
    for (int i = 0; i < num; ++i) TrackMic(streams[i]);
  }
  return ok;
}

EXPORT void SDL_DestroyAudioStream(SDL_AudioStream* stream) {
  static void (*fn)(SDL_AudioStream*);
  {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_mic_streams.erase(stream);
  }
  Cached(fn, "SDL_DestroyAudioStream")(stream);
}

EXPORT int SDL_GetAudioStreamData(SDL_AudioStream* stream, void* buf, int len) {
  static int (*fn)(SDL_AudioStream*, void*, int);
  const int got = Cached(fn, "SDL_GetAudioStreamData")(stream, buf, len);
  if (got > 0 && IsMic(stream)) {
    SDL_AudioSpec dst{};
    if (SDL_GetAudioStreamFormat(stream, nullptr, &dst)) MixIntoMic(buf, got, dst);
  }
  return got;
}
