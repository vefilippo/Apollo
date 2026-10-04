/**
 * @file tests/unit/test_mic_redirect.cpp
 * @brief Tests for microphone redirection sequencing, session selection and device lifetime.
 */
#include "../tests_common.h"

#include <atomic>
#include <src/mic_redirect.h>
#include <thread>
#include <vector>

using namespace std::literals;
using mic_redirect::packet_disposition_e;

// classify_packet

TEST(MicRedirectClassifyTest, AcceptsAnyPacketWithoutPlayoutCursor) {
  EXPECT_EQ(mic_redirect::classify_packet(false, 0, 12345), packet_disposition_e::accept);
}

TEST(MicRedirectClassifyTest, AcceptsExpectedAndSlightlyAheadPackets) {
  EXPECT_EQ(mic_redirect::classify_packet(true, 10, 10), packet_disposition_e::accept);
  EXPECT_EQ(mic_redirect::classify_packet(true, 10, 15), packet_disposition_e::accept);
}

TEST(MicRedirectClassifyTest, DropsSlightlyLatePacketsAsStale) {
  EXPECT_EQ(mic_redirect::classify_packet(true, 10, 9), packet_disposition_e::stale);
  EXPECT_EQ(mic_redirect::classify_packet(true, 10, 0), packet_disposition_e::stale);
}

TEST(MicRedirectClassifyTest, ResyncsWhenClientRestartsFarBehindCursor) {
  // Reconnected client restarts at 0 while the old cursor is well past it.
  EXPECT_EQ(mic_redirect::classify_packet(true, 5000, 0), packet_disposition_e::resync);
}

TEST(MicRedirectClassifyTest, ResyncsWhenRestartLandsFarAheadOfCursor) {
  // Old cursor above 0x8000: a restart at 0 looks "ahead" by wraparound and would otherwise play
  // concealment until the sequence space wrapped.
  EXPECT_EQ(mic_redirect::classify_packet(true, 40000, 0), packet_disposition_e::resync);
}

TEST(MicRedirectClassifyTest, HandlesWraparoundAsInOrder) {
  EXPECT_EQ(mic_redirect::classify_packet(true, 65535, 0), packet_disposition_e::accept);
  EXPECT_EQ(mic_redirect::classify_packet(true, 0, 65535), packet_disposition_e::stale);
}

// find_newest_if

TEST(MicRedirectFindNewestTest, ReturnsLastMatchingElement) {
  int a = 1, b = 2, c = 3;
  std::vector<int *> items {&a, &b, &c};
  auto *found = mic_redirect::find_newest_if(items, [](int *v) { return *v != 3; });
  EXPECT_EQ(found, &b);
}

TEST(MicRedirectFindNewestTest, ReturnsNullWhenNothingMatches) {
  int a = 1;
  std::vector<int *> items {&a};
  EXPECT_EQ(mic_redirect::find_newest_if(items, [](int *) { return false; }), nullptr);
}

// device_guard_t

namespace {
  struct fake_device_t {
    std::atomic<int> inits {0};
    std::atomic<int> releases {0};
    std::atomic<bool> fail_init {false};
    std::atomic<bool> in_use {false};
    std::atomic<bool> released_while_in_use {false};

    mic_redirect::device_guard_t make_guard() {
      return mic_redirect::device_guard_t {
        [this]() {
          ++inits;
          return fail_init ? -1 : 0;
        },
        [this]() {
          if (in_use) {
            released_while_in_use = true;
          }
          ++releases;
        }
      };
    }
  };
}  // namespace

TEST(MicRedirectDeviceGuardTest, FirstAcquireInitializesDevice) {
  fake_device_t dev;
  auto guard = dev.make_guard();
  EXPECT_EQ(guard.acquire(), 0);
  EXPECT_EQ(dev.inits, 1);
  EXPECT_EQ(guard.users(), 1);
}

TEST(MicRedirectDeviceGuardTest, LaterAcquireReinitializesForFreshPlayoutState) {
  fake_device_t dev;
  auto guard = dev.make_guard();
  ASSERT_EQ(guard.acquire(), 0);
  ASSERT_EQ(guard.acquire(), 0);
  EXPECT_EQ(dev.inits, 2);
  EXPECT_EQ(dev.releases, 1);
  EXPECT_EQ(guard.users(), 2);
}

TEST(MicRedirectDeviceGuardTest, ReleasesDeviceOnlyWhenLastUserLeaves) {
  fake_device_t dev;
  auto guard = dev.make_guard();
  ASSERT_EQ(guard.acquire(), 0);
  ASSERT_EQ(guard.acquire(), 0);
  guard.release();
  EXPECT_EQ(dev.releases, 1);  // only the re-init release so far
  guard.release();
  EXPECT_EQ(dev.releases, 2);
  EXPECT_EQ(guard.users(), 0);
}

TEST(MicRedirectDeviceGuardTest, FailedInitDoesNotCountUser) {
  fake_device_t dev;
  dev.fail_init = true;
  auto guard = dev.make_guard();
  EXPECT_NE(guard.acquire(), 0);
  EXPECT_EQ(guard.users(), 0);
  EXPECT_EQ(guard.with_device([] { return 1; }), -1);
}

TEST(MicRedirectDeviceGuardTest, WithDeviceRejectsWhenInactive) {
  fake_device_t dev;
  auto guard = dev.make_guard();
  bool called = false;
  EXPECT_EQ(guard.with_device([&] {
    called = true;
    return 1;
  }),
            -1);
  EXPECT_FALSE(called);
}

TEST(MicRedirectDeviceGuardTest, WithDeviceRunsWhenActive) {
  fake_device_t dev;
  auto guard = dev.make_guard();
  ASSERT_EQ(guard.acquire(), 0);
  EXPECT_EQ(guard.with_device([] { return 7; }), 7);
}

TEST(MicRedirectDeviceGuardTest, ReleaseNeverOverlapsDeviceUse) {
  fake_device_t dev;
  auto guard = dev.make_guard();
  std::atomic<bool> stop {false};

  std::thread writer([&] {
    while (!stop) {
      guard.with_device([&] {
        dev.in_use = true;
        std::this_thread::sleep_for(50us);
        dev.in_use = false;
        return 0;
      });
    }
  });

  for (int i = 0; i < 300; ++i) {
    guard.acquire();
    guard.release();
  }
  stop = true;
  writer.join();

  EXPECT_FALSE(dev.released_while_in_use);
  EXPECT_EQ(guard.users(), 0);
}

// recovery_limiter_t

TEST(MicRedirectRecoveryLimiterTest, AllowsFirstAttemptThenWaitsForInterval) {
  mic_redirect::recovery_limiter_t limiter {2s};
  const auto t0 = std::chrono::steady_clock::time_point {} + 100s;
  EXPECT_TRUE(limiter.should_attempt(t0));
  EXPECT_FALSE(limiter.should_attempt(t0 + 1s));
  EXPECT_TRUE(limiter.should_attempt(t0 + 2s));
}
