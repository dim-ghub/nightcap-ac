// Drives the preload hook the way Nightcap does: focus a text box, deliver
// typed text and keys through the event queue, then close the box. Built and
// run by test_hook.py with LD_PRELOAD pointing at the hook.

#include <SDL3/SDL.h>
#include <stdio.h>

static void push_text(const char* text) {
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_EVENT_TEXT_INPUT;
  e.text.text = text;
  SDL_PushEvent(&e);
}

static void push_key(SDL_Keycode key, SDL_Keymod mod) {
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_EVENT_KEY_DOWN;
  e.key.key = key;
  e.key.mod = mod;
  e.key.down = true;
  SDL_PushEvent(&e);
}

static void drain(int use_peep) {
  SDL_Event e;
  if (use_peep) {
    // A peek must not be reported; only the GETEVENT that follows.
    SDL_PeepEvents(&e, 1, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
    }
  } else {
    while (SDL_PollEvent(&e)) {
    }
  }
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 2;
  }
  SDL_Window* window = SDL_CreateWindow("hook test", 64, 64, 0);
  if (window == NULL) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 2;
  }
  drain(0);

  // Typing while no box is focused must not be reported.
  push_text("w");
  drain(0);

  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
  SDL_StartTextInputWithProperties(window, props);
  push_text("hi");
  push_text(" there");
  push_key(SDLK_BACKSPACE, SDL_KMOD_NONE);
  drain(1);
  push_key(SDLK_RETURN, SDL_KMOD_NONE);
  drain(0);
  SDL_StopTextInput(window);

  // A password box: opened and closed, but its text stays private.
  SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER,
                        SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN);
  SDL_StartTextInputWithProperties(window, props);
  push_text("hunter2");
  drain(0);
  SDL_StopTextInput(window);
  SDL_DestroyProperties(props);

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
