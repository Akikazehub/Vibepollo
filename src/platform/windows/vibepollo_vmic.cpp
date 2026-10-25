/**
 * @file src/platform/windows/vibepollo_vmic.cpp
 * @brief vibepollo_vmic_t implementation.
 */

#include "vibepollo_vmic.h"

#include "src/config.h"
#include "src/logging.h"
#include "utf_utils.h"

using namespace std::literals;

namespace platf::audio {

  int vibepollo_vmic_t::init() {
    auto backend = std::make_unique<mic_write_wasapi_t>();

    // mic_sink (when set) is the primary search pattern so an explicit render
    // endpoint choice wins; the canonical Steam names always follow so the
    // backend can auto-discover the device when mic_sink is a shorter alias.
    if (!config::audio.mic_sink.empty()) {
      backend->autodetect_patterns.push_back(utf_utils::from_utf8(config::audio.mic_sink));
    }
    backend->autodetect_patterns.push_back(L"Steam Streaming Microphone");
    backend->autodetect_patterns.push_back(L"Speakers (Steam Streaming Microphone)");

    if (backend->init() != 0) {
      log_missing_driver_once();
      return -1;
    }

    speaker_backend = std::move(backend);
    return 0;
  }

  int vibepollo_vmic_t::write_pcm(const float *samples, std::uint32_t frame_count) {
    if (!speaker_backend) return -1;
    return speaker_backend->write_pcm(samples, frame_count);
  }

  void vibepollo_vmic_t::log_missing_driver_once() {
    if (!missing_driver_logged) {
      missing_driver_logged = true;
      BOOST_LOG(warning) << "[mic] Steam Streaming Microphone render endpoint not found — "
                            "is Steam running on this machine? Mic passthrough disabled."sv;
    }
  }

}  // namespace platf::audio
