/**
 * @file tests/unit/test_live_resize.cpp
 * @brief Size policy, debounce, and 0x5506/0x5507 parsing.
 */
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include "../tests_common.h"
#include <src/live_resize.h>
#include <src/nvhttp.h>

using namespace std::chrono_literals;

namespace {
  live_resize::size_request_t size_of(int requested_w, int requested_h, int width, int height) {
    live_resize::size_request_t size;
    size.requested_width = requested_w;
    size.requested_height = requested_h;
    size.width = width;
    size.height = height;
    return size;
  }

  std::string bytes_hex(const std::uint8_t *data, std::size_t size) {
    static constexpr char k_hex[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
      out[i * 2] = k_hex[data[i] >> 4];
      out[i * 2 + 1] = k_hex[data[i] & 0x0f];
    }
    return out;
  }
}  // namespace

TEST(LiveResize, ResolutionRequestPacketIsLittleEndian) {
  const auto packet = live_resize::encode_resolution_request_packet(1280, 720);
  const std::uint8_t expected[] = {
    0x06, 0x55, 0x0c, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x05, 0x00, 0x00,
    0xd0, 0x02, 0x00, 0x00
  };
  ASSERT_EQ(packet.size(), sizeof(expected));
  EXPECT_EQ(0, std::memcmp(packet.data(), expected, sizeof(expected)));

  const auto body = std::string_view(reinterpret_cast<const char *>(packet.data() + 4), 12);
  const auto parsed = live_resize::parse_resolution_payload(body);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->width, 1280);
  EXPECT_EQ(parsed->height, 720);
}

TEST(LiveResize, ResolutionNotifyPacketIsLittleEndian) {
  const auto packet = live_resize::encode_resolution_notify_packet(1280, 720);
  const std::uint8_t expected[] = {
    0x07, 0x55, 0x08, 0x00,
    0x00, 0x05, 0x00, 0x00,
    0xd0, 0x02, 0x00, 0x00
  };
  ASSERT_EQ(packet.size(), sizeof(expected));
  EXPECT_EQ(0, std::memcmp(packet.data(), expected, sizeof(expected)));
  EXPECT_EQ(bytes_hex(packet.data(), packet.size()), "0755080000050000d0020000");
}

TEST(LiveResize, ParseResolutionPayloadRejectsShortAndNonResolution) {
  std::uint8_t short_body[11] = {};
  EXPECT_FALSE(live_resize::parse_resolution_payload(std::string_view(reinterpret_cast<char *>(short_body), sizeof(short_body))));

  auto fps = live_resize::encode_resolution_payload(1280, 720);
  fps[0] = 1;
  EXPECT_FALSE(live_resize::parse_resolution_payload(std::string_view(reinterpret_cast<char *>(fps.data()), fps.size())));

  auto extra = live_resize::encode_resolution_payload(640, 480);
  std::string padded(reinterpret_cast<char *>(extra.data()), extra.size());
  padded.append("tail");
  const auto parsed = live_resize::parse_resolution_payload(padded);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->width, 640);
  EXPECT_EQ(parsed->height, 480);

  auto negative = live_resize::encode_resolution_payload(-1, 720);
  const auto neg = live_resize::parse_resolution_payload(std::string_view(reinterpret_cast<char *>(negative.data()), negative.size()));
  ASSERT_TRUE(neg);
  EXPECT_EQ(neg->width, -1);
  EXPECT_FALSE(live_resize::clamp_encode_size(neg->width, neg->height, 0));
}

TEST(LiveResize, ClampEncodeSize) {
  const auto odd = live_resize::clamp_encode_size(1919, 1081, 0);
  ASSERT_TRUE(odd);
  EXPECT_EQ(odd->width, 1918);
  EXPECT_EQ(odd->height, 1080);

  const auto exact = live_resize::clamp_encode_size(64, 64, 0);
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->width, 64);
  EXPECT_EQ(exact->height, 64);

  const auto rounded = live_resize::clamp_encode_size(65, 65, 0);
  ASSERT_TRUE(rounded);
  EXPECT_EQ(rounded->width, 64);
  EXPECT_EQ(rounded->height, 64);

  EXPECT_FALSE(live_resize::clamp_encode_size(63, 100, 0));
  EXPECT_FALSE(live_resize::clamp_encode_size(0, 720, 0));
  EXPECT_FALSE(live_resize::clamp_encode_size(-1920, 1080, 0));
  EXPECT_FALSE(live_resize::clamp_encode_size(100000, 64, 0));

  const auto h264 = live_resize::clamp_encode_size(7680, 4320, 0);
  ASSERT_TRUE(h264);
  EXPECT_EQ(h264->width, 4096);
  EXPECT_EQ(h264->height, 2304);

  const auto wide = live_resize::clamp_encode_size(5000, 1000, 0);
  ASSERT_TRUE(wide);
  EXPECT_EQ(wide->width, 4096);
  EXPECT_EQ(wide->height, 818);

  const auto capped = live_resize::clamp_encode_size(4096, 4096, 0);
  ASSERT_TRUE(capped);
  EXPECT_EQ(capped->width, 4096);
  EXPECT_EQ(capped->height, 4096);

  const auto hevc = live_resize::clamp_encode_size(5000, 5000, 1);
  ASSERT_TRUE(hevc);
  EXPECT_EQ(hevc->width, 5000);
  EXPECT_EQ(hevc->height, 5000);

  const auto av1 = live_resize::clamp_encode_size(9000, 1000, 2);
  ASSERT_TRUE(av1);
  EXPECT_EQ(av1->width, 8192);
  EXPECT_EQ(av1->height, 910);

  const auto unknown = live_resize::clamp_encode_size(7680, 4320, 3);
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown->width, 4096);
  EXPECT_EQ(unknown->height, 2304);
}

TEST(LiveResize, ScaleBitrateFromBaseline) {
  EXPECT_EQ(live_resize::scale_bitrate(20000, 1920, 1080, 1280, 720), 8888);
  EXPECT_EQ(live_resize::scale_bitrate(20000, 1920, 1080, 960, 540), 5000);
  const int stepped = live_resize::scale_bitrate(20000, 1920, 1080, 1280, 720);
  const int chained = live_resize::scale_bitrate(stepped, 1280, 720, 960, 540);
  EXPECT_EQ(chained, 4999);
  EXPECT_NE(chained, live_resize::scale_bitrate(20000, 1920, 1080, 960, 540));

  EXPECT_EQ(live_resize::scale_bitrate(20000, 1920, 1080, 64, 64), live_resize::min_bitrate_kbps);
  EXPECT_EQ(live_resize::scale_bitrate(400000, 64, 64, 8192, 8192), live_resize::max_bitrate_kbps);
  EXPECT_EQ(live_resize::scale_bitrate(0, 1920, 1080, 1280, 720), 0);
  EXPECT_EQ(live_resize::scale_bitrate(20000, 0, 1080, 1280, 720), 20000);
}

TEST(LiveResize, DebounceLeadingThenQuiet) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::time_point {};
  live_resize::debouncer_t debouncer;

  EXPECT_EQ(debouncer.push(size_of(1920, 1080, 1920, 1080), t0), live_resize::push_result_e::apply);
  EXPECT_EQ(debouncer.push(size_of(1920, 1080, 1920, 1080), t0 + 100ms), live_resize::push_result_e::drop);
  EXPECT_EQ(debouncer.push(size_of(1280, 720, 1280, 720), t0 + 100ms), live_resize::push_result_e::hold);
  EXPECT_EQ(debouncer.push(size_of(1280, 720, 1280, 720), t0 + 200ms), live_resize::push_result_e::hold);
  EXPECT_FALSE(debouncer.poll(t0 + 349ms));
  const auto settled = debouncer.poll(t0 + 350ms);
  ASSERT_TRUE(settled);
  EXPECT_EQ(settled->width, 1280);
  EXPECT_EQ(settled->height, 720);

  EXPECT_EQ(debouncer.push(size_of(1280, 720, 1280, 720), t0 + 350ms + 249ms), live_resize::push_result_e::drop);
  EXPECT_EQ(debouncer.push(size_of(1280, 720, 1280, 720), t0 + 350ms + live_resize::debounce_window), live_resize::push_result_e::apply);
}

TEST(LiveResize, DebounceKeepsLatestAndDropsRepeat) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::time_point {};
  live_resize::debouncer_t debouncer;

  EXPECT_EQ(debouncer.push(size_of(1920, 1080, 1920, 1080), t0), live_resize::push_result_e::apply);
  EXPECT_EQ(debouncer.push(size_of(1600, 900, 1600, 900), t0 + 10ms), live_resize::push_result_e::hold);
  EXPECT_EQ(debouncer.push(size_of(1280, 800, 1280, 800), t0 + 20ms), live_resize::push_result_e::hold);
  EXPECT_FALSE(debouncer.poll(t0 + 20ms));
  const auto latest = debouncer.poll(t0 + 20ms + live_resize::debounce_window);
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->width, 1280);
  EXPECT_EQ(latest->height, 800);

  live_resize::debouncer_t repeat;
  EXPECT_EQ(repeat.push(size_of(1920, 1080, 1920, 1080), t0), live_resize::push_result_e::apply);
  EXPECT_EQ(repeat.push(size_of(1280, 720, 1280, 720), t0 + 10ms), live_resize::push_result_e::hold);
  EXPECT_EQ(repeat.push(size_of(1920, 1080, 1920, 1080), t0 + 20ms), live_resize::push_result_e::hold);
  EXPECT_FALSE(repeat.poll(t0 + 20ms + live_resize::debounce_window));
}

TEST(LiveResize, DecideResize) {
  using clock = std::chrono::steady_clock;
  const auto now = clock::time_point {} + 2s;
  const auto recent = now - 100ms;
  const auto expired = now - live_resize::settle_timeout;

  const auto not_window = live_resize::decide_resize(1920, 1080, 1280, 720, true, false, recent, now, 1920, 1080, 20000, 1920, 1080);
  EXPECT_EQ(not_window.action, live_resize::adopt_e::letterbox);
  EXPECT_EQ(not_window.width, 1920);
  EXPECT_EQ(not_window.height, 1080);

  const auto ignored = live_resize::decide_resize(1280, 720, 1280, 720, false, true, recent, now, 1920, 1080, 20000, 1920, 1080);
  EXPECT_EQ(ignored.action, live_resize::adopt_e::letterbox);

  const auto adopt = live_resize::decide_resize(1279, 721, 1280, 720, true, true, recent, now, 1920, 1080, 20000, 1920, 1080);
  EXPECT_EQ(adopt.action, live_resize::adopt_e::adopt);
  EXPECT_EQ(adopt.width, 1280);
  EXPECT_EQ(adopt.height, 720);
  EXPECT_EQ(adopt.bitrate_kbps, 8888);

  const auto unchanged = live_resize::decide_resize(1280, 720, 1280, 720, true, true, recent, now, 1280, 720, 20000, 1920, 1080);
  EXPECT_EQ(unchanged.action, live_resize::adopt_e::unchanged);
  EXPECT_EQ(unchanged.width, 1280);

  const auto waiting = live_resize::decide_resize(1200, 700, 1280, 720, true, true, recent, now, 1920, 1080, 20000, 1920, 1080);
  EXPECT_EQ(waiting.action, live_resize::adopt_e::wait);

  const auto late = live_resize::decide_resize(1200, 700, 1280, 720, true, true, expired, now, 1920, 1080, 20000, 1920, 1080);
  EXPECT_EQ(late.action, live_resize::adopt_e::letterbox);
  EXPECT_EQ(late.width, 1920);
  EXPECT_EQ(late.height, 1080);
  EXPECT_EQ(late.bitrate_kbps, 20000);
}

TEST(LiveResize, ParseResizeFile) {
  const auto parsed = live_resize::parse_resize_file("width=1280\nheight=720\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->width, 1280);
  EXPECT_EQ(parsed->height, 720);

  const auto crlf = live_resize::parse_resize_file("# comment\r\nwidth=800\r\nheight=600\r\n");
  ASSERT_TRUE(crlf);
  EXPECT_EQ(crlf->width, 800);
  EXPECT_EQ(crlf->height, 600);

  EXPECT_FALSE(live_resize::parse_resize_file("width=1280\n"));
  EXPECT_FALSE(live_resize::parse_resize_file(""));
  EXPECT_FALSE(live_resize::parse_resize_file("width=nope\nheight=720\n"));

  EXPECT_STREQ(live_resize::source_name(live_resize::resize_source_e::control), "0x5506");
  EXPECT_STREQ(live_resize::source_name(live_resize::resize_source_e::resize_file), "resize.txt");
}

TEST(LiveResize, AdvertisesClientResolutionChange) {
  EXPECT_EQ(live_resize::capability_name, "ClientResolutionChange");
  EXPECT_EQ(live_resize::client_resolution_change_capability(), 1);

  boost::property_tree::ptree tree;
  nvhttp::advertise_live_resize(tree);
  EXPECT_EQ(tree.get<int>("root.ClientResolutionChange"), 1);

  std::ostringstream xml;
  boost::property_tree::write_xml(xml, tree);
  EXPECT_NE(xml.str().find("<ClientResolutionChange>1</ClientResolutionChange>"), std::string::npos);
}
