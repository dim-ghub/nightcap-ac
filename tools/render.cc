// Renders text to a WAV file with the in-app synthesizer:
//   nightcap-ac-render OUT.wav "text" [voice] [style] [pitch] [speed] [pack]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/synth.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s OUT.wav TEXT [voice] [style] [pitch] [speed] [pack]\n", argv[0]);
    return 2;
  }
  nac::VoiceSettings s;
  if (argc > 3) s.voice = argv[3];
  if (argc > 4) s.style = argv[4];
  if (argc > 5) s.pitch = std::atof(argv[5]);
  if (argc > 6) s.speed = std::atof(argv[6]);
  nac::SamplePack pack;
  if (argc > 7) {
    std::string error;
    if (!pack.Load(argv[7], &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  }
  const auto audio = nac::Synthesize(argv[2], s, 1, &pack);
  FILE* f = std::fopen(argv[1], "wb");
  if (!f) return 1;
  const uint32_t rate = nac::kSampleRate, bytes = audio.size() * 2;
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
  std::fwrite("data", 1, 4, f); u32(bytes);
  for (float v : audio) {
    const float c = v > 1 ? 1 : v < -1 ? -1 : v;
    u16(static_cast<uint16_t>(static_cast<int16_t>(c * 32767)));
  }
  std::fclose(f);
  return 0;
}
