/**
 * @file src/mic_redirect.h
 * @brief Platform-independent helpers for client microphone redirection.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <mutex>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>
#include <utility>

namespace mic_redirect {
  /**
   * @brief How far (in packets, either direction) a packet may be from the playout cursor before
   *        it is treated as a new stream instead of a late or early packet of the current one.
   *        Must exceed the jitter buffer's queue limit so normal reordering never triggers it.
   */
  constexpr std::uint16_t resync_threshold_packets = 100;

  enum class packet_disposition_e {
    accept,  ///< Queue the packet for playout.
    stale,  ///< Arrived after its playout deadline; drop it.
    resync,  ///< Sequence jumped (e.g. the client reconnected); reset playout state, then queue it.
  };

  /**
   * @brief Decide what to do with an incoming microphone packet.
   * @param has_cursor Whether playout has started (a cursor exists).
   * @param expected The sequence number the playout cursor expects next.
   * @param sequence The incoming packet's sequence number.
   * @param threshold Distance beyond which the packet starts a new stream.
   */
  inline packet_disposition_e classify_packet(bool has_cursor, std::uint16_t expected, std::uint16_t sequence, std::uint16_t threshold = resync_threshold_packets) {
    if (!has_cursor) {
      return packet_disposition_e::accept;
    }

    const auto behind = static_cast<std::uint16_t>(expected - sequence);
    if (behind == 0) {
      return packet_disposition_e::accept;
    }

    if (behind < 0x8000) {
      return behind > threshold ? packet_disposition_e::resync : packet_disposition_e::stale;
    }

    const auto ahead = static_cast<std::uint16_t>(sequence - expected);
    return ahead > threshold ? packet_disposition_e::resync : packet_disposition_e::accept;
  }

  /**
   * @brief Encryption capability bit for AES-GCM microphone packets (local protocol extension).
   *        Negotiated alongside SS_ENC_MICROPHONE (0x08, AES-CBC); this build requires it.
   */
  constexpr std::uint32_t encryption_flag_gcm = 0x10;

  constexpr std::size_t gcm_counter_size = 8;
  constexpr std::size_t gcm_tag_size = 16;
  constexpr std::size_t inner_header_size = 6;  ///< sequence (LE16) + timestamp (LE32)

  /**
   * @brief 12-byte AES-GCM nonce for a client-originated microphone packet.
   *
   * Deterministic construction per NIST SP 800-38D 8.2.1: the client's 64-bit packet counter is the
   * invocation field and 'C','M' (client, microphone) is the fixed field, distinct from the fixed
   * fields of every other AES-GCM use of the session key ('H'/'C','C' control, 'V' video).
   */
  inline std::array<std::uint8_t, 12> gcm_iv(std::uint64_t counter) {
    std::array<std::uint8_t, 12> iv {};
    for (std::size_t i = 0; i < gcm_counter_size; ++i) {
      iv[i] = static_cast<std::uint8_t>(counter >> (8 * i));
    }
    iv[10] = 'C';
    iv[11] = 'M';
    return iv;
  }

  struct gcm_payload_t {
    std::uint64_t counter;
    std::string_view tagged_cipher;  ///< 16-byte tag followed by ciphertext
  };

  /**
   * @brief Split an encrypted microphone payload into its counter and tag+ciphertext.
   * @return nullopt if too short to hold counter, tag and the encrypted inner header.
   */
  inline std::optional<gcm_payload_t> split_gcm_payload(std::string_view payload) {
    if (payload.size() < gcm_counter_size + gcm_tag_size + inner_header_size) {
      return std::nullopt;
    }

    std::uint64_t counter = 0;
    for (std::size_t i = 0; i < gcm_counter_size; ++i) {
      counter |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(payload[i])) << (8 * i);
    }

    return gcm_payload_t {counter, payload.substr(gcm_counter_size)};
  }

  /**
   * @brief Check the authenticated inner header against the packet's plaintext header.
   * @return The Opus frame, or nullopt if the outer sequence/timestamp were tampered with.
   */
  inline std::optional<std::string_view> unwrap_inner(const std::vector<std::uint8_t> &plaintext, std::uint16_t sequence, std::uint32_t timestamp) {
    if (plaintext.size() < inner_header_size) {
      return std::nullopt;
    }

    const auto inner_sequence = static_cast<std::uint16_t>(plaintext[0] | (plaintext[1] << 8));
    const auto inner_timestamp = static_cast<std::uint32_t>(plaintext[2]) |
                                 (static_cast<std::uint32_t>(plaintext[3]) << 8) |
                                 (static_cast<std::uint32_t>(plaintext[4]) << 16) |
                                 (static_cast<std::uint32_t>(plaintext[5]) << 24);
    if (inner_sequence != sequence || inner_timestamp != timestamp) {
      return std::nullopt;
    }

    return std::string_view {reinterpret_cast<const char *>(plaintext.data()) + inner_header_size, plaintext.size() - inner_header_size};
  }

  /**
   * @brief Sliding-window replay protection over a 64-bit packet counter (RFC 4303 style).
   */
  class replay_window_t {
  public:
    static constexpr std::uint64_t window_size = 64;

    /**
     * @return true the first time a counter within the window is seen; false for duplicates and
     *         counters older than the window.
     */
    bool accept(std::uint64_t counter) {
      if (!any_seen) {
        any_seen = true;
        highest = counter;
        seen = 1;
        return true;
      }

      if (counter > highest) {
        const auto shift = counter - highest;
        seen = shift >= window_size ? 0 : seen << shift;
        seen |= 1;
        highest = counter;
        return true;
      }

      const auto offset = highest - counter;
      if (offset >= window_size) {
        return false;
      }

      const auto bit = std::uint64_t {1} << offset;
      if (seen & bit) {
        return false;
      }

      seen |= bit;
      return true;
    }

  private:
    bool any_seen = false;
    std::uint64_t highest = 0;
    std::uint64_t seen = 0;  ///< bit i set => counter (highest - i) was accepted
  };

  /**
   * @brief Of the given sequence numbers, the first one at or after @p cursor (wraparound-aware).
   * @param keys A non-empty range of 16-bit sequence numbers.
   */
  template<class Range>
  std::uint16_t earliest_after(const Range &keys, std::uint16_t cursor) {
    auto best = *std::begin(keys);
    for (auto key : keys) {
      if (static_cast<std::uint16_t>(key - cursor) < static_cast<std::uint16_t>(best - cursor)) {
        best = key;
      }
    }
    return best;
  }

  /**
   * @brief Of the given sequence numbers, the one furthest behind @p newest (wraparound-aware).
   * @param keys A non-empty range of 16-bit sequence numbers.
   */
  template<class Range>
  std::uint16_t oldest_before(const Range &keys, std::uint16_t newest) {
    auto best = *std::begin(keys);
    for (auto key : keys) {
      if (static_cast<std::uint16_t>(newest - key) > static_cast<std::uint16_t>(newest - best)) {
        best = key;
      }
    }
    return best;
  }

  /**
   * @brief Return the last (newest) element of a container of pointers matching a predicate.
   */
  template<class Container, class Pred>
  auto find_newest_if(Container &items, Pred pred) -> typename Container::value_type {
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      if (pred(*it)) {
        return *it;
      }
    }

    return nullptr;
  }

  /**
   * @brief Owns the shared host microphone device across concurrent mic sessions.
   *
   * Session count, device initialization, release and use are serialized by one mutex, so a
   * release can never run while a packet is being written, and a session ending cannot tear the
   * device down underneath a session that is starting.
   */
  class device_guard_t {
  public:
    device_guard_t(std::function<int()> init, std::function<void()> release):
        init_fn {std::move(init)},
        release_fn {std::move(release)} {
    }

    device_guard_t(device_guard_t &&other) noexcept:
        init_fn {std::move(other.init_fn)},
        release_fn {std::move(other.release_fn)},
        user_count {other.user_count},
        active {other.active} {
    }

    /**
     * @brief Register a starting mic session. The first session initializes the device; later
     *        sessions re-initialize it so the new session starts from a fresh playout state.
     * @return 0 on success. On failure the session is not counted.
     */
    int acquire() {
      std::lock_guard lock(mutex);

      if (active) {
        release_fn();
        active = false;
      }

      if (init_fn() != 0) {
        return -1;
      }

      active = true;
      ++user_count;
      return 0;
    }

    /**
     * @brief Unregister an ending mic session; the last one releases the device.
     */
    void release() {
      std::lock_guard lock(mutex);

      if (user_count == 0) {
        return;
      }

      if (--user_count == 0 && active) {
        release_fn();
        active = false;
      }
    }

    /**
     * @brief Run @p fn against the active device without blocking.
     * @return fn's result, or -1 if the device is inactive or busy initializing/releasing.
     */
    template<class F>
    int with_device(F &&fn) {
      std::unique_lock lock(mutex, std::try_to_lock);
      if (!lock.owns_lock() || !active) {
        return -1;
      }

      return fn();
    }

    int users() {
      std::lock_guard lock(mutex);
      return user_count;
    }

  private:
    std::function<int()> init_fn;
    std::function<void()> release_fn;
    std::mutex mutex;
    int user_count = 0;
    bool active = false;
  };

  /**
   * @brief Rate-limits a repeated log message; reports how many were suppressed in between.
   */
  class log_limiter_t {
  public:
    explicit log_limiter_t(std::chrono::steady_clock::duration interval):
        interval {interval} {
    }

    /**
     * @param suppressed Set to the number of messages suppressed since the last one logged.
     * @return Whether to log now.
     */
    bool should_log(std::chrono::steady_clock::time_point now, std::size_t &suppressed) {
      std::lock_guard lock(mutex);
      if (last_logged && now - *last_logged < interval) {
        ++suppressed_count;
        return false;
      }

      last_logged = now;
      suppressed = suppressed_count;
      suppressed_count = 0;
      return true;
    }

  private:
    std::chrono::steady_clock::duration interval;
    std::optional<std::chrono::steady_clock::time_point> last_logged;
    std::size_t suppressed_count = 0;
    std::mutex mutex;
  };

  /**
   * @brief Rate-limits device recovery attempts.
   */
  class recovery_limiter_t {
  public:
    explicit recovery_limiter_t(std::chrono::steady_clock::duration min_interval):
        min_interval {min_interval} {
    }

    bool should_attempt(std::chrono::steady_clock::time_point now) {
      if (last_attempt && now - *last_attempt < min_interval) {
        return false;
      }

      last_attempt = now;
      return true;
    }

  private:
    std::chrono::steady_clock::duration min_interval;
    std::optional<std::chrono::steady_clock::time_point> last_attempt;
  };
}  // namespace mic_redirect
