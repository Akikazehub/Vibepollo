/**
 * @file src/platform/windows/mic_write.h
 * @brief WASAPI render backend for the Steam Streaming Microphone.
 *
 * mic_redirect_backend_t is the abstract sink the audio control layer writes
 * decoded client-mic PCM into; mic_write_wasapi_t is the WASAPI implementation.
 *
 * Format note: IPolicyConfig::SetDeviceFormat must be fed a
 * KSDATAFORMAT_SUBTYPE_PCM waveformat to normalize the Steam endpoint, while
 * IAudioClient::Initialize must request KSDATAFORMAT_SUBTYPE_IEEE_FLOAT for
 * the render stream. Using IEEE_FLOAT for both makes Initialize fail with
 * AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008) on the Steam driver.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// WinSock2.h must precede any windows.h pulled in by the WASAPI headers below,
// otherwise the toolchain emits "#warning Please include winsock2.h before
// windows.h", which is fatal under -Werror.
#include <WinSock2.h>

#include <Audioclient.h>
#include <mmdeviceapi.h>

#include "src/utility.h"

namespace platf::audio {

  template<class T>
  void release_com(T *p) {
    p->Release();
  }

  /**
   * @brief Abstract base for mic redirect backends.
   */
  class mic_redirect_backend_t {
  public:
    virtual int init() = 0;
    virtual int write_pcm(const float *samples, std::uint32_t frame_count) = 0;
    virtual const char *backend_id() const = 0;
    virtual ~mic_redirect_backend_t() = default;
  };

  /**
   * @brief WASAPI render backend that plays decoded PCM into the Steam Streaming Microphone.
   *
   * init() finds the Steam render endpoint (re-enabling it when Windows left it
   * disabled/unplugged), normalizes its device format via IPolicyConfig, then
   * opens a shared-mode IEEE_FLOAT render stream.
   *
   * write_pcm() queues mono float32 frames; the render thread duplicates each
   * frame to both channels and drains the queue into WASAPI.
   */
  class mic_write_wasapi_t: public mic_redirect_backend_t {
  public:
    int init() override;
    int write_pcm(const float *samples, std::uint32_t frame_count) override;
    const char *backend_id() const override { return "mic_write_wasapi"; }
    ~mic_write_wasapi_t() override;

    // Set before calling init(); matched case-insensitively as substrings of
    // endpoint friendly names.
    std::vector<std::wstring> autodetect_patterns;

  private:
    bool find_target_device(std::wstring &out_device_id);
    bool try_enable_endpoint(const std::wstring &device_id);
    void normalize_device_formats(const std::wstring &device_id);
    bool initialize_device(const std::wstring &device_id);
    void render_loop();

    util::safe_ptr<IMMDeviceEnumerator, release_com<IMMDeviceEnumerator>> device_enum;
    util::safe_ptr<IAudioClient, release_com<IAudioClient>> audio_client;
    IAudioRenderClient *audio_render = nullptr;
    UINT32 buffer_frame_count = 0;
    std::string target_device_name;
    bool first_write_logged = false;
    bool render_dead_logged = false;
    util::safe_ptr_v2<void, BOOL, CloseHandle> render_event;
    std::mutex queue_mutex;
    std::deque<float> pending_frames;
    std::thread render_thread;
    std::atomic<bool> stop_render_thread {false};
    std::atomic<bool> render_dead {false};
  };

}  // namespace platf::audio
