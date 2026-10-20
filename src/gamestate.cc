#include "gamestate.h"

#include <sys/stat.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace nac {
namespace {

std::atomic<bool> g_in_game{false};
std::once_flag g_started;

std::string LatestLog() {
  const char* state = std::getenv("XDG_STATE_HOME");
  const char* home = std::getenv("HOME");
  std::string base = state && *state ? state : std::string(home ? home : "") + "/.local/state";
  return base + "/mocktail/logs/latest.log";
}

void Watch() {
  const std::string link = LatestLog();
  std::string path;
  std::streamoff offset = 0;
  std::string partial;
  while (true) {
    // latest.log points at this session's log once Nightcap has started it.
    char target[4096];
    const ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
    std::string current = link;
    if (n > 0) {
      target[n] = 0;
      current = target[0] == '/' ? target : link.substr(0, link.rfind('/') + 1) + target;
    }
    if (current != path) {
      path = current;
      offset = 0;
      partial.clear();
      g_in_game = false;
    }
    std::ifstream file(path);
    if (file) {
      file.seekg(0, std::ios::end);
      const std::streamoff size = file.tellg();
      if (size < offset) offset = 0;  // truncated
      if (size > offset) {
        file.seekg(offset);
        std::string chunk(static_cast<size_t>(size - offset), '\0');
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        offset = size;
        partial += chunk;
        size_t start = 0, end;
        while ((end = partial.find('\n', start)) != std::string::npos) {
          const std::string_view line(partial.data() + start, end - start);
          if (line.find("Joining game '") != std::string_view::npos) {
            g_in_game = true;
          } else if (line.find("onGameLeaveBegin()") != std::string_view::npos ||
                     line.find("leave game dispatched") != std::string_view::npos) {
            g_in_game = false;
          }
          start = end + 1;
        }
        partial.erase(0, start);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }
}

}  // namespace

bool InGame() {
  std::call_once(g_started, [] { std::thread(Watch).detach(); });
  return g_in_game.load();
}

}  // namespace nac
