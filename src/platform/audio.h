/**
 * @file src/platform/audio.h
 * @brief Audio device boundary, independent of video and host services.
 */
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platf {
  struct sink_t {
    // Play on host PC
    std::string host;

    // On macOS and Windows, it is not possible to create a virtual sink
    // Therefore, it is optional
    struct null_t {
      std::string stereo;
      std::string surround51;
      std::string surround71;
    };

    std::optional<null_t> null;
  };

  enum class capture_e : int {
    ok,  ///< Success
    reinit,  ///< Need to reinitialize
    timeout,  ///< Timeout
    interrupted,  ///< Capture was interrupted
    error  ///< Error
  };

  class mic_t {
  public:
    virtual capture_e sample(std::vector<float> &frame_buffer) = 0;

    virtual ~mic_t() = default;
  };

  /**
   * @brief Saved default capture endpoint ids so the mic uplink can restore them later.
   */
  struct capture_snapshot_t {
    std::string console_id;
    std::string comms_id;
    std::string multimedia_id;
  };

  class audio_control_t {
  public:
    virtual int set_sink(const std::string &sink) = 0;

    // Select a loopback endpoint without changing system routing, when supported.
    virtual int set_capture_sink([[maybe_unused]] const std::string &sink) {
      return -1;
    }

    virtual std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous, bool host_audio_enabled) = 0;

    /**
     * @brief Check if the audio sink is available in the system.
     * @param sink Sink to be checked.
     * @returns True if available, false otherwise.
     */
    virtual bool is_sink_available(const std::string &sink) = 0;

    virtual std::optional<sink_t> sink_info() = 0;

    /**
     * @brief Restores the host audio sink after streaming stops.
     * @note Most platforms restore a single sink. Windows overrides this to
     *       restore the endpoint captured for each audio role.
     */
    virtual int restore_sink(const std::string &sink) {
      return set_sink(sink);
    }

    /**
     * @brief Resets the default audio device away from virtual streaming speakers.
     * Implementations may continue trying in the background to restore the
     * preferred device after moving the default away from virtual speakers.
     * @param preferred_device Device identifier (platform-specific, e.g. on Windows the
     *        endpoint device_id) of the device that should be restored as default. If
     *        empty, implementations fall back to letting the OS pick whatever non-virtual
     *        device is currently present.
     */
    virtual void reset_default_device([[maybe_unused]] const std::string &preferred_device = {}) {}

    /**
     * @brief Initialize the client-microphone redirect backend (Steam Streaming Microphone).
     * @return 0 on success, -1 when unavailable (Steam not running / endpoint missing).
     */
    virtual int init_mic_redirect_device() {
      return -1;
    }

    /**
     * @brief Release the mic redirect backend.
     */
    virtual void release_mic_redirect_device() {}

    /**
     * @brief Write mono float32 PCM frames into the mic redirect backend.
     */
    virtual int write_mic_pcm(const float * /*samples*/, std::uint32_t /*frame_count*/) {
      return -1;
    }

    /**
     * @brief True if the mic redirect endpoint could be opened right now.
     */
    virtual bool mic_redirect_available() {
      return false;
    }

    /**
     * @brief Snapshot the current default capture endpoints (per role) for later restore.
     */
    virtual capture_snapshot_t snapshot_capture_defaults() {
      return {};
    }

    /**
     * @brief Make a capture endpoint the default for every role, by friendly name.
     */
    virtual void switch_capture_to(const std::string & /*device_name*/) {}

    /**
     * @brief Restore default capture endpoints from an earlier snapshot.
     */
    virtual void restore_capture_from(const capture_snapshot_t & /*snapshot*/) {}

    /**
     * @brief Friendly name of the current default capture endpoint (console role).
     */
    virtual std::string get_current_default_capture_name() {
      return {};
    }

    virtual ~audio_control_t() = default;
  };

  /**
   * @brief Check for an active microphone redirect endpoint without initializing it.
   */
  bool mic_redirect_available();

}  // namespace platf
