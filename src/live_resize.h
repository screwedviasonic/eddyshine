/**
 * @file src/live_resize.h
 * @brief Client-driven live resize (Foundation 0x5506 / 0x5507).
 *
 * Wire format, little-endian, matching AlkaidLab/foundation-sunshine PR #424
 * and the qiin2333/moonlight-common-c parser:
 *
 *   0x5506 client -> host, plaintext before encryption:
 *     uint16 type = 0x5506
 *     uint16 payloadLength = 12
 *     int32  param_type = 0          (SS_DYNAMIC_PARAM_TYPE_RESOLUTION)
 *     int32  width
 *     int32  height
 *   The control handler sees only the 12-byte body (param_type, width, height).
 *
 *   0x5507 host -> client, plaintext before encryption:
 *     uint16 type = 0x5507
 *     uint16 payloadLength = 8
 *     uint32 width
 *     uint32 height
 *
 * Stock Moonlight does not send 0x5506 and must not be sent 0x5507.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace live_resize {
  inline constexpr int resolution_param_type = 0;
  inline constexpr std::uint16_t dynamic_param_type = 0x5506;
  inline constexpr std::uint16_t resolution_notify_type = 0x5507;
  inline constexpr std::string_view capability_name = "ClientResolutionChange";
  inline constexpr int size_tolerance_px = 2;
  inline constexpr std::chrono::milliseconds debounce_window {250};
  inline constexpr std::chrono::milliseconds settle_timeout {800};
  // Long enough for the control thread to emit 0x5507 before the next IDR.
  // The control loop blocks in ENet for up to 150 ms between sends.
  inline constexpr std::chrono::milliseconds notify_lead {200};
  inline constexpr int min_bitrate_kbps = 500;
  inline constexpr int max_bitrate_kbps = 500000;

  inline int client_resolution_change_capability() {
    return 1;
  }

  struct limits_t {
    int min_width;
    int min_height;
    int max_width;
    int max_height;
  };

  // video_format matches video::config_t::videoFormat: 0 H.264, 1 HEVC, 2 AV1.
  // Caps follow NVENC on Ada (RTX 4070): H.264 4096, HEVC/AV1 8192. AMF, QSV,
  // and software stay inside the same box so a request cannot exceed the GPU path.
  inline limits_t encoder_limits(int video_format) {
    constexpr int k_min = 64;
    if (video_format == 1 || video_format == 2) {
      return {k_min, k_min, 8192, 8192};
    }
    return {k_min, k_min, 4096, 4096};
  }

  struct encode_size_t {
    int width = 0;
    int height = 0;
  };

  struct parsed_resolution_t {
    int width = 0;
    int height = 0;
  };

  inline void put_u32_le(std::uint8_t *out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xffu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
  }

  inline std::uint32_t get_u32_le(const std::uint8_t *in) {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
  }

  inline std::int32_t get_i32_le(const std::uint8_t *in) {
    return static_cast<std::int32_t>(get_u32_le(in));
  }

  // 12-byte 0x5506 body: int32 type=0, int32 width, int32 height.
  inline std::array<std::uint8_t, 12> encode_resolution_payload(int width, int height) {
    std::array<std::uint8_t, 12> out {};
    put_u32_le(out.data(), static_cast<std::uint32_t>(resolution_param_type));
    put_u32_le(out.data() + 4, static_cast<std::uint32_t>(width));
    put_u32_le(out.data() + 8, static_cast<std::uint32_t>(height));
    return out;
  }

  // Full 0x5506 plaintext, including the 4-byte control header.
  inline std::array<std::uint8_t, 16> encode_resolution_request_packet(int width, int height) {
    std::array<std::uint8_t, 16> out {};
    out[0] = static_cast<std::uint8_t>(dynamic_param_type & 0xffu);
    out[1] = static_cast<std::uint8_t>((dynamic_param_type >> 8) & 0xffu);
    out[2] = 12;
    out[3] = 0;
    auto body = encode_resolution_payload(width, height);
    for (std::size_t i = 0; i < body.size(); ++i) {
      out[4 + i] = body[i];
    }
    return out;
  }

  // Full 0x5507 plaintext, including the 4-byte control header.
  inline std::array<std::uint8_t, 12> encode_resolution_notify_packet(std::uint32_t width, std::uint32_t height) {
    std::array<std::uint8_t, 12> out {};
    out[0] = static_cast<std::uint8_t>(resolution_notify_type & 0xffu);
    out[1] = static_cast<std::uint8_t>((resolution_notify_type >> 8) & 0xffu);
    out[2] = 8;
    out[3] = 0;
    put_u32_le(out.data() + 4, width);
    put_u32_le(out.data() + 8, height);
    return out;
  }

  // `payload` is the decrypted body, header already stripped.
  inline std::optional<parsed_resolution_t> parse_resolution_payload(std::string_view payload) {
    if (payload.size() < 12) {
      return std::nullopt;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(payload.data());
    const auto param_type = get_i32_le(bytes);
    if (param_type != resolution_param_type) {
      return std::nullopt;
    }
    parsed_resolution_t parsed;
    parsed.width = get_i32_le(bytes + 4);
    parsed.height = get_i32_le(bytes + 8);
    return parsed;
  }

  inline std::optional<encode_size_t> clamp_encode_size(int width, int height, int video_format) {
    if (width <= 0 || height <= 0) {
      return std::nullopt;
    }
    const auto limits = encoder_limits(video_format);
    int w = width & ~1;
    int h = height & ~1;
    if (w < limits.min_width || h < limits.min_height) {
      return std::nullopt;
    }
    if (w > limits.max_width) {
      h = static_cast<int>((static_cast<std::int64_t>(h) * limits.max_width) / w);
      w = limits.max_width;
    }
    if (h > limits.max_height) {
      w = static_cast<int>((static_cast<std::int64_t>(w) * limits.max_height) / h);
      h = limits.max_height;
    }
    w &= ~1;
    h &= ~1;
    if (w < limits.min_width || h < limits.min_height || w > limits.max_width || h > limits.max_height) {
      return std::nullopt;
    }
    return encode_size_t {w, h};
  }

  inline int scale_bitrate(int base_kbps, int base_width, int base_height, int new_width, int new_height) {
    if (base_kbps <= 0 || base_width <= 0 || base_height <= 0 || new_width <= 0 || new_height <= 0) {
      return base_kbps;
    }
    const auto scaled = static_cast<std::int64_t>(base_kbps) * new_width * new_height /
                        (static_cast<std::int64_t>(base_width) * base_height);
    if (scaled < min_bitrate_kbps) {
      return min_bitrate_kbps;
    }
    if (scaled > max_bitrate_kbps) {
      return max_bitrate_kbps;
    }
    return static_cast<int>(scaled);
  }

  inline bool near_size(int width, int height, int target_width, int target_height, int tolerance = size_tolerance_px) {
    return std::abs(width - target_width) <= tolerance && std::abs(height - target_height) <= tolerance;
  }

  enum class push_result_e {
    drop,
    apply,
    hold,
  };

  struct size_request_t {
    int requested_width = 0;
    int requested_height = 0;
    int width = 0;
    int height = 0;
  };

  // Leading edge applies the first size immediately. Later sizes inside
  // debounce_window are held and the latest one is applied after a quiet gap.
  class debouncer_t {
  public:
    push_result_e push(const size_request_t &size, std::chrono::steady_clock::time_point now) {
      if (pending && pending->width == size.width && pending->height == size.height) {
        return push_result_e::hold;
      }
      if (!pending && applied && size.width == last_width && size.height == last_height && (now - last_apply) < debounce_window) {
        return push_result_e::drop;
      }
      const bool leading = !applied || (now - last_apply) >= debounce_window;
      if (leading && !pending) {
        last_apply = now;
        last_input = now;
        last_width = size.width;
        last_height = size.height;
        applied = true;
        return push_result_e::apply;
      }
      pending = size;
      last_input = now;
      return push_result_e::hold;
    }

    std::optional<size_request_t> poll(std::chrono::steady_clock::time_point now) {
      if (!pending) {
        return std::nullopt;
      }
      if ((now - last_input) < debounce_window) {
        return std::nullopt;
      }
      auto out = *pending;
      pending.reset();
      if (applied && out.width == last_width && out.height == last_height) {
        return std::nullopt;
      }
      last_apply = now;
      last_input = now;
      last_width = out.width;
      last_height = out.height;
      applied = true;
      return out;
    }

  private:
    std::optional<size_request_t> pending;
    std::chrono::steady_clock::time_point last_apply {};
    std::chrono::steady_clock::time_point last_input {};
    int last_width = 0;
    int last_height = 0;
    bool applied = false;
  };

  enum class adopt_e {
    wait,
    adopt,
    unchanged,
    letterbox,
  };

  struct adopt_decision_t {
    adopt_e action = adopt_e::letterbox;
    int width = 0;
    int height = 0;
    int bitrate_kbps = 0;
  };

  enum class resize_source_e {
    control,
    resize_file,
  };

  inline const char *source_name(resize_source_e source) {
    return source == resize_source_e::resize_file ? "resize.txt" : "0x5506";
  }

  struct request_t {
    int requested_width = 0;
    int requested_height = 0;
    int clamped_width = 0;
    int clamped_height = 0;
    int applied_width = 0;
    int applied_height = 0;
    bool window_followed = false;
    resize_source_e source = resize_source_e::control;
    std::chrono::steady_clock::time_point requested_at {};
  };

  struct notify_t {
    int width = 0;
    int height = 0;
  };

  // Capture size is the window WGC just produced. Target is the clamped request.
  // A window that does not land on the target keeps the current encode size.
  inline adopt_decision_t decide_resize(
    int capture_width,
    int capture_height,
    int target_width,
    int target_height,
    bool window_followed,
    bool window_capture,
    std::chrono::steady_clock::time_point requested_at,
    std::chrono::steady_clock::time_point now,
    int current_width,
    int current_height,
    int base_bitrate_kbps,
    int base_width,
    int base_height
  ) {
    adopt_decision_t decision;
    decision.bitrate_kbps = base_bitrate_kbps;
    if (!window_capture || !window_followed || target_width <= 0 || target_height <= 0) {
      decision.action = adopt_e::letterbox;
      decision.width = current_width;
      decision.height = current_height;
      return decision;
    }
    if (near_size(capture_width, capture_height, target_width, target_height)) {
      if (current_width == target_width && current_height == target_height) {
        decision.action = adopt_e::unchanged;
        decision.width = current_width;
        decision.height = current_height;
        return decision;
      }
      decision.action = adopt_e::adopt;
      decision.width = target_width;
      decision.height = target_height;
      decision.bitrate_kbps = scale_bitrate(base_bitrate_kbps, base_width, base_height, target_width, target_height);
      return decision;
    }
    if ((now - requested_at) < settle_timeout) {
      decision.action = adopt_e::wait;
      decision.width = current_width;
      decision.height = current_height;
      return decision;
    }
    decision.action = adopt_e::letterbox;
    decision.width = current_width;
    decision.height = current_height;
    return decision;
  }

  struct file_resize_t {
    int width = 0;
    int height = 0;
  };

  inline std::optional<file_resize_t> parse_resize_file(std::string_view text) {
    int width = 0;
    int height = 0;
    bool have_width = false;
    bool have_height = false;
    std::size_t i = 0;
    while (i < text.size()) {
      auto end = text.find('\n', i);
      if (end == std::string_view::npos) {
        end = text.size();
      }
      auto line = text.substr(i, end - i);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      i = end < text.size() ? end + 1 : text.size();
      if (line.empty() || line.front() == '#') {
        continue;
      }
      const auto split = line.find('=');
      if (split == std::string_view::npos) {
        continue;
      }
      const auto key = line.substr(0, split);
      const auto value = line.substr(split + 1);
      try {
        const int parsed = std::stoi(std::string {value});
        if (key == "width") {
          width = parsed;
          have_width = true;
        } else if (key == "height") {
          height = parsed;
          have_height = true;
        }
      } catch (...) {
      }
    }
    if (!have_width || !have_height) {
      return std::nullopt;
    }
    return file_resize_t {width, height};
  }

  struct window_resize_result_t {
    bool attempted = false;
    bool followed = false;
    int applied_width = 0;
    int applied_height = 0;
    std::string reason;
  };

  window_resize_result_t resize_captured_window(int width, int height);
  void clear_observed_frame_size();
  void note_observed_frame_size(int width, int height);
  bool observed_frame_size(int &width, int &height);
  void request_capture_reinit();
  bool consume_capture_reinit();
  std::optional<file_resize_t> take_resize_file();
}  // namespace live_resize
