/**
 * @file src/platform/windows/mic_write.cpp
 * @brief WASAPI render backend implementation for the Steam Streaming Microphone.
 */

#include "mic_write.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <Audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>

// Must come after mmdeviceapi.h
#include "PolicyConfig.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "utf_utils.h"

using namespace std::literals;

namespace {

  // Local PKEY definition: audio.cpp defines its own copy with INITGUID, and the
  // two translation units must not share one symbol.
  const PROPERTYKEY local_PKEY_Device_FriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
  };

  /**
   * @brief Waveformat handed to IPolicyConfig::SetDeviceFormat.
   *
   * SubFormat must be KSDATAFORMAT_SUBTYPE_PCM here. Handing the same call an
   * IEEE_FLOAT format makes the later WASAPI Initialize fail with
   * AUDCLNT_E_UNSUPPORTED_FORMAT on the Steam Streaming Microphone driver.
   */
  WAVEFORMATEXTENSIBLE make_device_format_waveformat() {
    WAVEFORMATEXTENSIBLE wfx {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = 2;
    wfx.Format.nSamplesPerSec = 48000;
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = 8;
    wfx.Format.nAvgBytesPerSec = 384000;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return wfx;
  }

  /**
   * @brief Waveformat handed to IAudioClient::Initialize (the actual render stream).
   */
  WAVEFORMATEXTENSIBLE make_render_waveformat() {
    WAVEFORMATEXTENSIBLE wfx = make_device_format_waveformat();
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return wfx;
  }

  std::wstring to_lower(std::wstring str) {
    std::transform(str.begin(), str.end(), str.begin(), ::towlower);
    return str;
  }

}  // namespace

namespace platf::audio {

  int mic_write_wasapi_t::init() {
    IMMDeviceEnumerator *raw_enum = nullptr;
    auto hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
      IID_IMMDeviceEnumerator, (void **) &raw_enum);
    if (FAILED(hr) || !raw_enum) {
      BOOST_LOG(warning) << "[mic] mic_write_wasapi: CoCreateInstance(DeviceEnumerator) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return -1;
    }
    device_enum.reset(raw_enum);

    std::wstring device_id;
    if (!find_target_device(device_id)) {
      BOOST_LOG(warning) << "[mic] mic_write_wasapi: Steam Streaming Microphone render endpoint not found"sv;
      return -1;
    }

    // Normalize the endpoint's device format (must run before Initialize).
    normalize_device_formats(device_id);

    if (!initialize_device(device_id)) {
      return -1;
    }

    return 0;
  }

  bool mic_write_wasapi_t::find_target_device(std::wstring &out_device_id) {
    auto search_mask = [&](DWORD state_mask, std::wstring &id_out) -> bool {
      IMMDeviceCollection *raw_collection = nullptr;
      auto hr = device_enum->EnumAudioEndpoints(eRender, state_mask, &raw_collection);
      if (FAILED(hr) || !raw_collection) {
        return false;
      }
      util::safe_ptr<IMMDeviceCollection, release_com<IMMDeviceCollection>> collection(raw_collection);

      UINT count = 0;
      collection->GetCount(&count);

      for (UINT i = 0; i < count; ++i) {
        IMMDevice *raw_device = nullptr;
        if (FAILED(collection->Item(i, &raw_device)) || !raw_device) continue;
        util::safe_ptr<IMMDevice, release_com<IMMDevice>> device(raw_device);

        IPropertyStore *raw_prop = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &raw_prop)) || !raw_prop) continue;
        util::safe_ptr<IPropertyStore, release_com<IPropertyStore>> prop(raw_prop);

        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (FAILED(prop->GetValue(local_PKEY_Device_FriendlyName, &pv)) ||
            pv.vt != VT_LPWSTR || !pv.pwszVal) {
          PropVariantClear(&pv);
          continue;
        }
        std::wstring name(pv.pwszVal);
        PropVariantClear(&pv);

        const auto name_lower = to_lower(name);
        bool matched = false;
        for (const auto &pattern: autodetect_patterns) {
          if (name_lower.find(to_lower(pattern)) != std::wstring::npos) {
            matched = true;
            break;
          }
        }
        if (!matched) continue;

        WCHAR *raw_id = nullptr;
        if (SUCCEEDED(device->GetId(&raw_id)) && raw_id) {
          id_out = std::wstring(raw_id);
          CoTaskMemFree(raw_id);
          target_device_name = utf_utils::to_utf8(name);
          return true;
        }
      }
      return false;
    };

    // Fast path: an ACTIVE endpoint is ready to use.
    if (search_mask(DEVICE_STATE_ACTIVE, out_device_id)) {
      BOOST_LOG(info) << "[mic] found Steam mic render endpoint: "sv << target_device_name;
      return true;
    }

    // Self-heal: the render endpoint (the loopback sink we play decoded mic
    // audio into) can be left disabled by Sound settings or a Steam Remote Play
    // toggle, and WASAPI then never enumerates it as ACTIVE. Try the
    // recoverable inactive states and re-enable via IPolicyConfig. NOTPRESENT
    // is excluded: a driver-absent endpoint cannot be revived by a visibility
    // change, and matching its stale entry first would falsely abort the heal.
    std::wstring inactive_id;
    if (search_mask(DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED, inactive_id)) {
      BOOST_LOG(warning) << "[mic] Steam mic render endpoint ["sv << target_device_name
                         << "] is present but inactive — attempting to re-enable it."sv;
      if (try_enable_endpoint(inactive_id)) {
        // Endpoint state propagation through MMDevice is asynchronous; poll for
        // the endpoint to surface as ACTIVE instead of assuming a fixed delay.
        for (int attempt = 0; attempt < 20; ++attempt) {
          if (search_mask(DEVICE_STATE_ACTIVE, out_device_id)) {
            BOOST_LOG(info) << "[mic] re-enabled and found Steam mic render endpoint: "sv << target_device_name;
            return true;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
      BOOST_LOG(warning) << "[mic] could not re-enable the Steam mic render endpoint — enable "
                            "'Speakers (Steam Streaming Microphone)' in Windows Sound settings, or "
                            "toggle Steam Remote Play microphone."sv;
      return false;
    }

    return false;
  }

  bool mic_write_wasapi_t::try_enable_endpoint(const std::wstring &device_id) {
    IPolicyConfig *raw_policy = nullptr;
    auto hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
      IID_IPolicyConfig, (void **) &raw_policy);
    if (FAILED(hr) || !raw_policy) {
      BOOST_LOG(warning) << "[mic] try_enable_endpoint: CoCreateInstance(IPolicyConfig) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    util::safe_ptr<IPolicyConfig, release_com<IPolicyConfig>> policy(raw_policy);

    hr = policy->SetEndpointVisibility(device_id.c_str(), TRUE);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[mic] try_enable_endpoint: SetEndpointVisibility(TRUE) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    return true;
  }

  void mic_write_wasapi_t::normalize_device_formats(const std::wstring &device_id) {
    IPolicyConfig *raw_policy = nullptr;
    auto hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
      IID_IPolicyConfig, (void **) &raw_policy);
    if (FAILED(hr) || !raw_policy) {
      BOOST_LOG(warning) << "[mic] normalize_formats: CoCreateInstance(IPolicyConfig) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return;
    }
    util::safe_ptr<IPolicyConfig, release_com<IPolicyConfig>> policy(raw_policy);

    const auto device_fmt = make_device_format_waveformat();
    WAVEFORMATEXTENSIBLE prev_fmt {};
    auto render_hr = policy->SetDeviceFormat(device_id.c_str(),
      reinterpret_cast<WAVEFORMATEX *>(&device_fmt),
      reinterpret_cast<WAVEFORMATEX *>(&prev_fmt));

    if (SUCCEEDED(render_hr)) {
      BOOST_LOG(info) << "[mic] Changed Steam microphone render device format for ["sv
                      << target_device_name << "] to [pcm, 32-bit, 48000 Hz, 2ch]"sv;
    } else {
      BOOST_LOG(warning) << "[mic] SetDeviceFormat render: 0x"sv
                         << util::hex(render_hr).to_string_view()
                         << " (format may already be correct)"sv;
    }

    // Also normalize the paired capture endpoint so recordings through the
    // Steam Streaming Microphone use the same layout.
    IMMDeviceCollection *raw_captures = nullptr;
    if (FAILED(device_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &raw_captures))) return;
    util::safe_ptr<IMMDeviceCollection, release_com<IMMDeviceCollection>> captures(raw_captures);

    UINT count = 0;
    captures->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      IMMDevice *raw_dev = nullptr;
      if (FAILED(captures->Item(i, &raw_dev)) || !raw_dev) continue;
      util::safe_ptr<IMMDevice, release_com<IMMDevice>> cap_dev(raw_dev);

      IPropertyStore *raw_prop = nullptr;
      if (FAILED(cap_dev->OpenPropertyStore(STGM_READ, &raw_prop)) || !raw_prop) continue;
      util::safe_ptr<IPropertyStore, release_com<IPropertyStore>> prop(raw_prop);

      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (FAILED(prop->GetValue(local_PKEY_Device_FriendlyName, &pv)) ||
          pv.vt != VT_LPWSTR || !pv.pwszVal) {
        PropVariantClear(&pv);
        continue;
      }
      std::wstring cap_name(pv.pwszVal);
      PropVariantClear(&pv);

      const auto cap_lower = to_lower(cap_name);
      bool is_steam_cap = cap_lower.find(L"steam streaming") != std::wstring::npos;
      if (!is_steam_cap) {
        for (const auto &pattern: autodetect_patterns) {
          if (cap_lower.find(to_lower(pattern)) != std::wstring::npos) {
            is_steam_cap = true;
            break;
          }
        }
      }
      if (!is_steam_cap) continue;

      WCHAR *raw_cap_id = nullptr;
      if (FAILED(cap_dev->GetId(&raw_cap_id)) || !raw_cap_id) continue;
      std::wstring cap_id(raw_cap_id);
      CoTaskMemFree(raw_cap_id);

      const auto cap_fmt = make_device_format_waveformat();
      WAVEFORMATEXTENSIBLE prev {};
      auto cap_hr = policy->SetDeviceFormat(cap_id.c_str(),
        reinterpret_cast<WAVEFORMATEX *>(&cap_fmt),
        reinterpret_cast<WAVEFORMATEX *>(&prev));

      if (SUCCEEDED(cap_hr)) {
        BOOST_LOG(info) << "[mic] Changed Steam microphone capture device format for ["sv
                        << utf_utils::to_utf8(cap_name) << "] to [pcm, 32-bit, 48000 Hz, 2ch]"sv;
      } else {
        BOOST_LOG(warning) << "[mic] SetDeviceFormat capture: 0x"sv
                           << util::hex(cap_hr).to_string_view()
                           << " (format may already be correct)"sv;
      }
    }
  }

  bool mic_write_wasapi_t::initialize_device(const std::wstring &device_id) {
    IMMDevice *raw_device = nullptr;
    auto hr = device_enum->GetDevice(device_id.c_str(), &raw_device);
    if (FAILED(hr) || !raw_device) {
      BOOST_LOG(warning) << "[mic] initialize_device: GetDevice failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    util::safe_ptr<IMMDevice, release_com<IMMDevice>> device(raw_device);

    IAudioClient *raw_client = nullptr;
    hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void **) &raw_client);
    if (FAILED(hr) || !raw_client) {
      BOOST_LOG(warning) << "[mic] initialize_device: Activate failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    audio_client.reset(raw_client);

    // Buffer duration in REFERENCE_TIME (100ns units): ms * 10000.
    const REFERENCE_TIME buffer_duration =
      static_cast<REFERENCE_TIME>(config::audio.mic_buffer_ms) * 10000LL;
    const auto render_fmt = make_render_waveformat();
    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer_duration, 0,
      reinterpret_cast<WAVEFORMATEX *>(&render_fmt), nullptr);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[mic] initialize_device: Initialize failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }

    hr = audio_client->GetBufferSize(&buffer_frame_count);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[mic] initialize_device: GetBufferSize failed"sv;
      return false;
    }

    IAudioRenderClient *raw_render = nullptr;
    hr = audio_client->GetService(IID_IAudioRenderClient, (void **) &raw_render);
    if (FAILED(hr) || !raw_render) {
      BOOST_LOG(warning) << "[mic] initialize_device: GetService(IAudioRenderClient) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    audio_render = raw_render;

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!evt) {
      BOOST_LOG(warning) << "[mic] initialize_device: CreateEvent failed"sv;
      return false;
    }
    render_event.reset(evt);
    audio_client->SetEventHandle(render_event.get());

    hr = audio_client->Start();
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[mic] initialize_device: Start failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }

    BOOST_LOG(info) << "[mic] WASAPI render target: "sv << target_device_name
                    << " buf="sv << buffer_frame_count << " frames"sv;

    // Only spawn the render thread on first init. The re-init path inside
    // render_loop runs on the render thread itself; assigning over a joinable
    // std::thread calls std::terminate().
    if (!render_thread.joinable()) {
      stop_render_thread.store(false);
      render_dead.store(false);
      render_thread = std::thread(&mic_write_wasapi_t::render_loop, this);
    }
    return true;
  }

  int mic_write_wasapi_t::write_pcm(const float *samples, std::uint32_t frame_count) {
    if (!render_event || stop_render_thread.load(std::memory_order_acquire) ||
        render_dead.load(std::memory_order_acquire)) {
      if (render_dead.load(std::memory_order_acquire) && !render_dead_logged) {
        render_dead_logged = true;
        BOOST_LOG(warning) << "[mic] write_pcm: render thread dead — mic audio being dropped"sv;
      }
      return -1;
    }
    {
      std::lock_guard<std::mutex> lk(queue_mutex);
      for (std::uint32_t i = 0; i < frame_count; ++i) {
        pending_frames.push_back(std::clamp(samples[i], -1.0f, 1.0f));
      }
      // Cap at one second of mono frames to bound latency.
      if (pending_frames.size() > 48000) {
        const auto trim = pending_frames.size() - 48000;
        pending_frames.erase(pending_frames.begin(), pending_frames.begin() + static_cast<std::ptrdiff_t>(trim));
      }
    }
    if (!first_write_logged) {
      first_write_logged = true;
      BOOST_LOG(info) << "[mic] first PCM write to Steam mic backend"sv;
    }
    SetEvent(render_event.get());
    return 0;
  }

  void mic_write_wasapi_t::render_loop() {
    set_thread_name("mic::render");
    auto coinit_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    if (FAILED(coinit_hr) && coinit_hr != RPC_E_CHANGED_MODE) {
      BOOST_LOG(error) << "[mic] render_loop: CoInitializeEx failed 0x"sv
                       << util::hex(coinit_hr).to_string_view();
      return;
    }
    adjust_thread_priority(thread_priority_e::high);

    while (!stop_render_thread.load(std::memory_order_acquire)) {
      WaitForSingleObject(render_event.get(), 20);
      if (stop_render_thread.load(std::memory_order_acquire)) break;

      UINT32 padding = 0;
      const HRESULT pad_hr = audio_client->GetCurrentPadding(&padding);
      if (FAILED(pad_hr)) {
        BOOST_LOG(warning) << "[mic] GetCurrentPadding failed 0x"sv
                           << util::hex(pad_hr).to_string_view()
                           << " — attempting WASAPI re-init"sv;
        if (audio_client) audio_client->Stop();
        if (audio_render) {
          audio_render->Release();
          audio_render = nullptr;
        }
        audio_client.reset();
        std::wstring device_id;
        if (!find_target_device(device_id) || !initialize_device(device_id)) {
          BOOST_LOG(error) << "[mic] WASAPI re-init failed — render dead"sv;
          render_dead.store(true, std::memory_order_release);
          break;
        }
        BOOST_LOG(info) << "[mic] WASAPI re-init succeeded — resuming render"sv;
        // Drop audio queued before the re-init so it isn't replayed into the
        // freshly-opened WASAPI session.
        {
          std::lock_guard<std::mutex> lk(queue_mutex);
          pending_frames.clear();
        }
        continue;
      }

      const auto avail = buffer_frame_count - padding;
      if (avail == 0) continue;

      UINT32 to_write;
      {
        std::lock_guard<std::mutex> lk(queue_mutex);
        to_write = std::min<std::uint32_t>(avail, static_cast<UINT32>(pending_frames.size()));
      }
      if (to_write == 0) continue;

      BYTE *buf = nullptr;
      if (FAILED(audio_render->GetBuffer(to_write, &buf)) || !buf) continue;

      auto *dst = reinterpret_cast<float *>(buf);
      {
        std::lock_guard<std::mutex> lk(queue_mutex);
        for (UINT32 f = 0; f < to_write; ++f) {
          const float s = pending_frames.front();
          pending_frames.pop_front();
          dst[f * 2] = s;
          dst[f * 2 + 1] = s;
        }
      }
      audio_render->ReleaseBuffer(to_write, 0);
    }

    if (SUCCEEDED(coinit_hr)) CoUninitialize();
  }

  mic_write_wasapi_t::~mic_write_wasapi_t() {
    stop_render_thread.store(true);
    if (render_event) SetEvent(render_event.get());
    if (render_thread.joinable()) render_thread.join();
    if (audio_client) audio_client->Stop();
    if (audio_render) {
      audio_render->Release();
      audio_render = nullptr;
    }
  }

}  // namespace platf::audio
