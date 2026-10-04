/**
 * @file tests/unit/test_config_defaults.cpp
 * @brief Defaults specific to this local build.
 */
#include "../tests_common.h"

#include <src/config.h>

TEST(ConfigDefaultsTest, MicrophoneStreamingEnabledByDefault) {
  EXPECT_TRUE(config::audio.stream_mic);
}

TEST(ConfigDefaultsTest, WindowsMicBackendIsSteamStreamingMicrophone) {
  EXPECT_EQ(config::audio.mic_backend, "steam_streaming_microphone");
}
