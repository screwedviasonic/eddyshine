/**
 * @file src/platform/windows/rig_target.h
 * @brief Read the window Rig asked Eddyshine to encode.
 *
 * Contract: docs live with Rig as docs/eddyshine-integration.md.
 * This header stays dependency-free so display setup can call it
 * before Boost.Logging is required.
 */
#pragma once

#include <cstdint>
#include <string>

namespace rig {
  struct target_t {
    bool present = false;
    bool window = false;
    std::uint32_t app_id = 0;
    std::uint32_t pid = 0;
    std::uint64_t hwnd = 0;
    int width = 0;
    int height = 0;
    std::uint64_t generation = 0;
  };

  target_t read_target();
  bool window_requested();
  void write_status(const char *capture, std::uint64_t hwnd, std::uint64_t generation, const std::string &reason);
}  // namespace rig
