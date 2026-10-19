// LD_PRELOAD shim that reports Nightcap's Roblox text-box activity to the
// nightcap-ac app. It wraps a few public SDL3 entry points, forwards every
// call to the real SDL unchanged, and sends one datagram per interesting event
// to $XDG_RUNTIME_DIR/nightcap-ac/hook.sock. No events are altered or
// swallowed, and nothing is sent when the app isn't listening.
//
// Datagram format: "<kind>\t<field>\t<field>..." in UTF-8.
//   hello  <pid>
//   open   <SDL_TextInputType> <multiline 0|1>  (password boxes send no text)
//   close
//   text   <utf8>
//   key    <return|escape|backspace|delete|left|right|home|end> <ctrl 0|1>
//   paste  <utf8>

#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

static int g_sock = -1;
static struct sockaddr_un g_addr;
static socklen_t g_addr_len;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
// Text input is only reported while a Roblox text box is focused, so a stray
// keypress in gameplay never reaches the app.
static volatile int g_text_active = 0;

static void init_socket(void) {
  const char* override = getenv("NIGHTCAP_AC_SOCKET");
  const char* runtime = getenv("XDG_RUNTIME_DIR");
  char path[sizeof(g_addr.sun_path)];
  if (override != NULL && override[0] != '\0') {
    snprintf(path, sizeof(path), "%s", override);
  } else if (runtime != NULL && runtime[0] != '\0') {
    snprintf(path, sizeof(path), "%s/nightcap-ac/hook.sock", runtime);
  } else {
    return;
  }
  g_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (g_sock < 0) return;
  memset(&g_addr, 0, sizeof(g_addr));
  g_addr.sun_family = AF_UNIX;
  memcpy(g_addr.sun_path, path, strlen(path) + 1);
  g_addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                           strlen(path) + 1);
}

static void send_msg(const char* msg, size_t len) {
  pthread_once(&g_once, init_socket);
  if (g_sock < 0) return;
  // Best effort: a missing or busy listener just drops the event.
  (void)sendto(g_sock, msg, len, MSG_DONTWAIT | MSG_NOSIGNAL,
               (const struct sockaddr*)&g_addr, g_addr_len);
}

static void send_fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void send_fmt(const char* fmt, ...) {
  char buf[4096];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n <= 0) return;
  if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
  send_msg(buf, (size_t)n);
}

static void send_text(const char* kind, const char* text) {
  if (text == NULL || text[0] == '\0') return;
  char buf[4096];
  int n = snprintf(buf, sizeof(buf), "%s\t%s", kind, text);
  if (n <= 0) return;
  if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
  send_msg(buf, (size_t)n);
}

__attribute__((constructor)) static void on_load(void) {
  send_fmt("hello\t%d", (int)getpid());
}

static void* real(const char* name) {
  void* fn = dlsym(RTLD_NEXT, name);
  if (fn == NULL) {
    fprintf(stderr, "[nightcap-ac] missing SDL symbol %s\n", name);
    abort();
  }
  return fn;
}

static const char* key_name(SDL_Keycode key) {
  switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      return "return";
    case SDLK_ESCAPE:
      return "escape";
    case SDLK_BACKSPACE:
      return "backspace";
    case SDLK_DELETE:
      return "delete";
    case SDLK_LEFT:
      return "left";
    case SDLK_RIGHT:
      return "right";
    case SDLK_HOME:
      return "home";
    case SDLK_END:
      return "end";
    default:
      return NULL;
  }
}

static void observe(const SDL_Event* event) {
  if (!g_text_active) return;
  switch (event->type) {
    case SDL_EVENT_TEXT_INPUT:
      send_text("text", event->text.text);
      break;
    case SDL_EVENT_KEY_DOWN: {
      const char* name = key_name(event->key.key);
      if (name != NULL) {
        send_fmt("key\t%s\t%d", name, (event->key.mod & SDL_KMOD_CTRL) ? 1 : 0);
      }
      break;
    }
    default:
      break;
  }
}

EXPORT bool SDL_StartTextInputWithProperties(SDL_Window* window,
                                             SDL_PropertiesID props) {
  static bool (*fn)(SDL_Window*, SDL_PropertiesID);
  if (fn == NULL) fn = real("SDL_StartTextInputWithProperties");
  const bool ok = fn(window, props);
  if (ok) {
    static Sint64 (*get_number)(SDL_PropertiesID, const char*, Sint64);
    static bool (*get_bool)(SDL_PropertiesID, const char*, bool);
    if (get_number == NULL) get_number = real("SDL_GetNumberProperty");
    if (get_bool == NULL) get_bool = real("SDL_GetBooleanProperty");
    const Sint64 type = get_number(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, 0);
    const bool multiline =
        get_bool(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
    // Password boxes are announced but their keystrokes are never reported.
    const bool secret = type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN ||
                        type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE ||
                        type == SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN ||
                        type == SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE;
    g_text_active = secret ? 0 : 1;
    send_fmt("open\t%lld\t%d", (long long)type, multiline ? 1 : 0);
  }
  return ok;
}

EXPORT bool SDL_StartTextInput(SDL_Window* window) {
  static bool (*fn)(SDL_Window*);
  if (fn == NULL) fn = real("SDL_StartTextInput");
  const bool ok = fn(window);
  if (ok) {
    g_text_active = 1;
    send_fmt("open\t0\t0");
  }
  return ok;
}

EXPORT bool SDL_StopTextInput(SDL_Window* window) {
  static bool (*fn)(SDL_Window*);
  if (fn == NULL) fn = real("SDL_StopTextInput");
  g_text_active = 0;
  send_fmt("close");
  return fn(window);
}

EXPORT bool SDL_PollEvent(SDL_Event* event) {
  static bool (*fn)(SDL_Event*);
  if (fn == NULL) fn = real("SDL_PollEvent");
  const bool got = fn(event);
  if (got && event != NULL) observe(event);
  return got;
}

EXPORT bool SDL_WaitEvent(SDL_Event* event) {
  static bool (*fn)(SDL_Event*);
  if (fn == NULL) fn = real("SDL_WaitEvent");
  const bool got = fn(event);
  if (got && event != NULL) observe(event);
  return got;
}

EXPORT bool SDL_WaitEventTimeout(SDL_Event* event, Sint32 timeout_ms) {
  static bool (*fn)(SDL_Event*, Sint32);
  if (fn == NULL) fn = real("SDL_WaitEventTimeout");
  const bool got = fn(event, timeout_ms);
  if (got && event != NULL) observe(event);
  return got;
}

EXPORT int SDL_PeepEvents(SDL_Event* events, int numevents,
                          SDL_EventAction action, Uint32 min_type,
                          Uint32 max_type) {
  static int (*fn)(SDL_Event*, int, SDL_EventAction, Uint32, Uint32);
  if (fn == NULL) fn = real("SDL_PeepEvents");
  const int count = fn(events, numevents, action, min_type, max_type);
  // Only events actually removed from the queue count; a peek would report
  // the same keystroke twice.
  if (action == SDL_GETEVENT && events != NULL) {
    for (int i = 0; i < count; ++i) observe(&events[i]);
  }
  return count;
}

EXPORT char* SDL_GetClipboardText(void) {
  static char* (*fn)(void);
  if (fn == NULL) fn = real("SDL_GetClipboardText");
  char* text = fn();
  // Roblox reads the clipboard when you paste into a focused box.
  if (g_text_active && text != NULL) send_text("paste", text);
  return text;
}
