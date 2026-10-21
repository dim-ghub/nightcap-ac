// Stands in for Nightcap's microphone and chat box when testing speech
// mixing. Opens the default recording device the way Nightcap does (a stream
// with a callback that pulls fixed-size frames with SDL_GetAudioStreamData),
// records what it reads to a file, and at one second types a chat message
// into a focused text box and presses Enter.
//
//   mic_driver OUT.raw SECONDS MESSAGE
//
// OUT.raw is mono signed 16-bit at 48 kHz, as Nightcap reads it.

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace {

std::mutex g_mutex;
std::vector<int16_t> g_captured;

void SDLCALL OnAudio(void*, SDL_AudioStream* stream, int, int) {
  int16_t frame[480];
  while (SDL_GetAudioStreamAvailable(stream) >= static_cast<int>(sizeof(frame))) {
    const int got = SDL_GetAudioStreamData(stream, frame, sizeof(frame));
    if (got != sizeof(frame)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_captured.insert(g_captured.end(), frame, frame + 480);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  const double seconds = std::atof(argv[2]);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 2;
  }
  SDL_Window* window = SDL_CreateWindow("mic test", 320, 240, 0);
  SDL_AudioSpec spec{SDL_AUDIO_S16, 1, 48000};
  SDL_AudioStream* mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, OnAudio, nullptr);
  if (mic == nullptr) {
    std::fprintf(stderr, "open mic: %s\n", SDL_GetError());
    return 2;
  }
  SDL_ResumeAudioStreamDevice(mic);

  const Uint64 start = SDL_GetTicks();
  bool typed = false;
  while (SDL_GetTicks() - start < seconds * 1000) {
    if (!typed && SDL_GetTicks() - start > 1000) {
      typed = true;
      // Roblox focuses its chat bar, the text arrives, Enter sends it.
      SDL_PropertiesID props = SDL_CreateProperties();
      SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
      SDL_StartTextInputWithProperties(window, props);
      SDL_DestroyProperties(props);
      SDL_Event e;
      SDL_zero(e);
      e.type = SDL_EVENT_TEXT_INPUT;
      e.text.windowID = SDL_GetWindowID(window);
      e.text.text = SDL_strdup(argv[3]);
      SDL_PushEvent(&e);
      SDL_zero(e);
      e.type = SDL_EVENT_KEY_DOWN;
      e.key.windowID = SDL_GetWindowID(window);
      e.key.key = SDLK_RETURN;
      e.key.down = true;
      SDL_PushEvent(&e);
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_KEY_DOWN) {
        std::fprintf(stderr, "driver received key: %d\n", static_cast<int>(e.key.key));
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_ESCAPE) {
          SDL_StopTextInput(window);
        }
      }
    }
    SDL_Delay(5);
  }
  SDL_DestroyAudioStream(mic);
  std::lock_guard<std::mutex> lock(g_mutex);
  FILE* f = std::fopen(argv[1], "wb");
  std::fwrite(g_captured.data(), sizeof(int16_t), g_captured.size(), f);
  std::fclose(f);
  std::printf("captured %.2f s\n", g_captured.size() / 48000.0);
  SDL_Quit();
  return 0;
}
