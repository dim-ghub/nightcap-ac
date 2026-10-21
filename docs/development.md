# Development & Testing Guide

This guide covers building Vox TTS from source, running the automated test suite, inspecting mock drivers, and debugging the runtime.

---

## 1. Prerequisites

To build Vox TTS, the following tools and development headers are required:

- **Compiler**: C++17 compliant compiler (`g++` or `clang++`).
- **Build System**: GNU `make`.
- **SDL3**: Development library headers (`sdl3` or `libsdl3-dev`).
- **Vulkan**: Headers and loader (`vulkan-headers`, `vulkan-icd-loader`).
- **Python 3**: For running the test suite.
- **TTS Backends (Optional for testing synthesis)**:
  - `espeak-ng`
  - `piper` or `piper-tts`

On Arch Linux / Artix Linux:
```bash
sudo pacman -S base-devel sdl3 vulkan-headers vulkan-icd-loader espeak-ng
```

---

## 2. Building

Build the shared library and helper tools:

```bash
make build
```

Artifacts generated in `build/`:
- `libvox_tts.so`: The core shared library (SDL hooks + Vulkan layer).
- `libnightcap_ac.so`: Backward-compatible symlink to `libvox_tts.so`.
- `vox-tts-render`: Standalone CLI tool to render Animalese directly to a WAV file.
- `nightcap-ac-render`: Symlink to `vox-tts-render`.

---

## 3. Running Tests

The test suite runs integration and unit tests without needing an active Roblox or Nightcap session:

```bash
make test
```

### Test Drivers
The test framework includes dedicated C++ stand-ins to exercise the hooks and overlay:

1. **`build/vk_driver` (`tests/vk_driver.cc`)**:
   - Spawns a headless Vulkan window with swapchain.
   - Loads `libvox_tts.so` as a Vulkan layer.
   - Feeds scripted mouse and keyboard inputs frame-by-frame.
   - Dumps framebuffer images (`.ppm`) to verify pixel-accurate menu rendering, hover states, and animations.

2. **`build/mic_driver` (`tests/mic_driver.cc`)**:
   - Opens an SDL3 microphone capture stream.
   - Simulates hardware microphone samples and calls `MixIntoMic()`.
   - Captures output PCM data to verify audio blending, volume scaling, and muting behaviors.

3. **`build/chat_test` (`tests/chat_test.cc`)**:
   - Unit tests the `ChatTracker` class (`src/chat.cc`) against complex typing sequences, Unicode strings, backspaces, text selections, and multiline pastes.

4. **Python Test Suites (`tests/`)**:
   - `test_inprocess.py`: Tests Vulkan overlay rendering, tab switching, keyboard selection, config migration, and audio capture.
   - `test_launcher.py`: Tests the shell launcher, lock detection, and desktop patching.
   - `test_synth.py`: Tests the audio synthesizer math, pitch autocorrelation, RMS power, and duration scaling across styles.

---

## 4. Debugging & Environment Flags

Vox TTS provides several environment variables for diagnostic output and inspection:

| Environment Variable | Description |
| --- | --- |
| `VOX_TTS_MENU_OPEN=1` | Starts the Vulkan layer with the overlay menu immediately open on the first frame. |
| `VOX_TTS_SHOW_BUTTON=1` | Forces the top-bar overlay button to render even outside of an active game. |
| `VOX_TTS_DUMP=/path/to/frame.ppm` | Dumps the composited Vulkan frame buffer to a PPM image file. |
| `VOX_TTS_DUMP_FRAME=N` | Specifies the frame number to dump (default: frame 30, allowing fonts and layouts to settle). |
| `VOX_TTS_LIB=/path/to/libvox_tts.so` | Overrides the library path searched by the launcher script. |

Legacy environment variables (`NIGHTCAP_AC_*`) continue to function for backward compatibility.
