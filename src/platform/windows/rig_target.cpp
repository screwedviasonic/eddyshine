/**
 * @file src/platform/windows/rig_target.cpp
 * @brief `%ProgramData%\Rig\capture.txt` and `capture-status.txt`.
 */
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif

#include "rig_target.h"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
  std::filesystem::path rig_dir() {
    if (const char *env = std::getenv("RIG_DATA_DIR")) {
      if (env[0] != '\0') {
        return std::filesystem::path(env);
      }
    }
    char buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableA("ProgramData", buf, MAX_PATH);
    std::filesystem::path base = (len > 0 && len < MAX_PATH) ? std::filesystem::path(buf) : std::filesystem::path("C:\\ProgramData");
    return base / "Rig";
  }

  void assign_u32(const std::string &text, std::uint32_t &out) {
    try {
      unsigned long value = std::stoul(text);
      out = static_cast<std::uint32_t>(value);
    } catch (...) {
    }
  }

  void assign_u64(const std::string &text, std::uint64_t &out) {
    try {
      out = std::stoull(text);
    } catch (...) {
    }
  }

  void assign_int(const std::string &text, int &out) {
    try {
      out = std::stoi(text);
    } catch (...) {
    }
  }
}  // namespace

namespace rig {
  target_t read_target() {
    target_t out;
    std::ifstream in(rig_dir() / "capture.txt");
    if (!in) {
      return out;
    }
    out.present = true;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const auto split = line.find('=');
      if (split == std::string::npos) {
        continue;
      }
      const std::string key = line.substr(0, split);
      const std::string value = line.substr(split + 1);
      if (key == "capture") {
        out.window = value == "window";
      } else if (key == "app_id") {
        assign_u32(value, out.app_id);
      } else if (key == "pid") {
        assign_u32(value, out.pid);
      } else if (key == "hwnd") {
        assign_u64(value, out.hwnd);
      } else if (key == "width") {
        assign_int(value, out.width);
      } else if (key == "height") {
        assign_int(value, out.height);
      } else if (key == "generation") {
        assign_u64(value, out.generation);
      }
    }
    return out;
  }

  bool window_requested() {
    const target_t target = read_target();
    return target.present && target.window;
  }

  void write_status(const char *capture, std::uint64_t hwnd, std::uint64_t generation, const std::string &reason) {
    const auto dir = rig_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "capture-status.txt";
    const auto tmp = dir / "capture-status.tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      if (!out) {
        return;
      }
      out << "capture=" << capture << "\n"
          << "hwnd=" << hwnd << "\n"
          << "generation=" << generation << "\n"
          << "reason=" << reason << "\n";
    }
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
  }
}  // namespace rig
