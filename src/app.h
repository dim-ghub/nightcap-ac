// Process-wide state shared by the SDL hooks, the speech engine and the
// in-game menu.

#ifndef VOX_TTS_APP_H_
#define VOX_TTS_APP_H_

#include <dlfcn.h>

#include <cstdio>
#include <string>

#include "config.h"

namespace nac {

// Settings, safe to read from any thread. SetConfig saves to disk shortly
// after the last change.
Config GetConfig();
void SetConfig(const Config& config);
void SaveConfigIfDirty();

// Lines go to stderr, which Nightcap copies into its session log.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// The next definition of an SDL function after ours (the real SDL), for the
// functions this library wraps.
template <typename Fn>
Fn Real(const char* name) {
  return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

}  // namespace nac

#endif  // VOX_TTS_APP_H_
