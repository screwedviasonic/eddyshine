/**
 * @file src/live_resize.cpp
 * @brief Window resize and capture-size handoff for live resize.
 */
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
#endif

#include "live_resize.h"

#ifdef _WIN32
  #include <Windows.h>

  #include "src/platform/windows/rig_target.h"
#endif

#include <atomic>

namespace {
  std::atomic<int> g_observed_w {0};
  std::atomic<int> g_observed_h {0};
  std::atomic<bool> g_observed_valid {false};
  std::atomic<bool> g_reinit {false};
}  // namespace

namespace live_resize {
  void clear_observed_frame_size() {
    g_observed_valid.store(false);
    g_observed_w.store(0);
    g_observed_h.store(0);
  }

  void note_observed_frame_size(int width, int height) {
    if (width <= 0 || height <= 0) {
      return;
    }
    g_observed_w.store(width);
    g_observed_h.store(height);
    g_observed_valid.store(true);
  }

  bool observed_frame_size(int &width, int &height) {
    if (!g_observed_valid.load()) {
      return false;
    }
    width = g_observed_w.load();
    height = g_observed_h.load();
    return width > 0 && height > 0;
  }

  void request_capture_reinit() {
    g_reinit.store(true);
  }

  bool consume_capture_reinit() {
    return g_reinit.exchange(false);
  }

  std::optional<file_resize_t> take_resize_file() {
#ifdef _WIN32
    const auto consumed = rig::consume_resize_file();
    if (!consumed) {
      return std::nullopt;
    }
    return file_resize_t {consumed->width, consumed->height};
#else
    return std::nullopt;
#endif
  }

  window_resize_result_t resize_captured_window(int width, int height) {
    window_resize_result_t result;
#ifndef _WIN32
    (void) width;
    (void) height;
    result.reason = "window resize is implemented on Windows only";
    return result;
#else
    const auto target = rig::read_target();
    if (!target.present || !target.window || target.hwnd == 0) {
      result.reason = "no window-capture hwnd";
      return result;
    }
    auto hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(target.hwnd));
    if (!IsWindow(hwnd)) {
      result.reason = "hwnd is not a window";
      return result;
    }
    result.attempted = true;
    if (IsIconic(hwnd)) {
      RECT iconic {};
      if (GetClientRect(hwnd, &iconic)) {
        result.applied_width = iconic.right - iconic.left;
        result.applied_height = iconic.bottom - iconic.top;
      }
      result.reason = "window-minimized";
      return result;
    }

    RECT rect {0, 0, width, height};
    const auto style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
    const auto ex_style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    const BOOL has_menu = GetMenu(hwnd) != nullptr;
    auto user32 = GetModuleHandleA("user32.dll");
    using get_dpi_fn = UINT(WINAPI *)(HWND);
    using adjust_dpi_fn = BOOL(WINAPI *)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto get_dpi = user32 ? reinterpret_cast<get_dpi_fn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
    auto adjust_dpi = user32 ? reinterpret_cast<adjust_dpi_fn>(GetProcAddress(user32, "AdjustWindowRectExForDpi")) : nullptr;
    if (get_dpi && adjust_dpi) {
      adjust_dpi(&rect, style, has_menu, ex_style, get_dpi(hwnd));
    } else {
      AdjustWindowRectEx(&rect, style, has_menu, ex_style);
    }
    const int outer_w = rect.right - rect.left;
    const int outer_h = rect.bottom - rect.top;
    if (outer_w <= 0 || outer_h <= 0 || !SetWindowPos(hwnd, nullptr, 0, 0, outer_w, outer_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
      result.reason = "SetWindowPos failed";
      return result;
    }
    RECT client {};
    if (!GetClientRect(hwnd, &client)) {
      result.reason = "GetClientRect failed";
      return result;
    }
    result.applied_width = client.right - client.left;
    result.applied_height = client.bottom - client.top;
    if (!near_size(result.applied_width, result.applied_height, width, height)) {
      result.reason = "window-did-not-follow";
      return result;
    }
    result.followed = true;
    result.reason = "";
    return result;
#endif
  }
}  // namespace live_resize
