/**
 * @file src/mic_redirect.h
 * @brief Platform-independent helpers for client microphone redirection.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
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
