#include "stdafx.h"

#include "manual_metronome_wasapi.h"
#include "foobar2000/SDK/foobar2000.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mmdeviceapi.h>
#include <mutex>
#include <string>
#include <thread>
#include <wrl/client.h>
#include <audioclient.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

namespace smart_tempo::manual_metronome {
namespace {

using Microsoft::WRL::ComPtr;

constexpr double kMinimumBpm = 40.0;
constexpr double kMaximumBpm = 220.0;
constexpr double kClickDurationSeconds = 0.035;

enum class sample_format {
    unsupported,
    float32,
    pcm16,
    pcm24,
    pcm32,
};

struct render_format {
    sample_format sample = sample_format::unsupported;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t blockAlign = 0;
};

bool is_wave_subtype(const GUID& value, WORD formatTag) {
    static constexpr BYTE waveGuidTail[8] = {
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    return value.Data1 == formatTag && value.Data2 == 0x0000 &&
        value.Data3 == 0x0010 &&
        std::memcmp(value.Data4, waveGuidTail, sizeof(waveGuidTail)) == 0;
}

render_format classify_format(const WAVEFORMATEX* format) {
    render_format out;
    if (format == nullptr || format->nSamplesPerSec == 0 ||
        format->nChannels == 0 || format->nBlockAlign == 0) {
        return out;
    }

    WORD tag = format->wFormatTag;
    GUID subtype{};
    if (tag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        subtype = extensible->SubFormat;
        if (is_wave_subtype(subtype, WAVE_FORMAT_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (is_wave_subtype(subtype, WAVE_FORMAT_PCM)) {
            tag = WAVE_FORMAT_PCM;
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        out.sample = sample_format::float32;
    } else if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) {
        out.sample = sample_format::pcm16;
    } else if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 24) {
        out.sample = sample_format::pcm24;
    } else if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 32) {
        out.sample = sample_format::pcm32;
    }

    out.sampleRate = format->nSamplesPerSec;
    out.channels = format->nChannels;
    out.blockAlign = format->nBlockAlign;
    return out;
}

void write_sample(
    BYTE* frame, const render_format& format, uint16_t channel,
    double value) {
    value = std::clamp(value, -1.0, 1.0);
    switch (format.sample) {
    case sample_format::float32: {
        auto* samples = reinterpret_cast<float*>(frame);
        samples[channel] = static_cast<float>(value);
        break;
    }
    case sample_format::pcm16: {
        auto* samples = reinterpret_cast<int16_t*>(frame);
        samples[channel] = static_cast<int16_t>(
            std::lround(value * 32767.0));
        break;
    }
    case sample_format::pcm24: {
        const int32_t sample = static_cast<int32_t>(
            std::lround(value * 8388607.0));
        BYTE* target = frame + static_cast<size_t>(channel) * 3;
        target[0] = static_cast<BYTE>(sample & 0xff);
        target[1] = static_cast<BYTE>((sample >> 8) & 0xff);
        target[2] = static_cast<BYTE>((sample >> 16) & 0xff);
        break;
    }
    case sample_format::pcm32: {
        auto* samples = reinterpret_cast<int32_t*>(frame);
        samples[channel] = static_cast<int32_t>(
            std::llround(value * 2147483647.0));
        break;
    }
    default:
        break;
    }
}

double click_sample(
    size_t position, uint32_t sampleRate, bool accent, int volumePercent) {
    const double time = static_cast<double>(position) / sampleRate;
    if (time >= kClickDurationSeconds) return 0.0;
    constexpr double twoPi = 6.28318530717958647692;
    const double frequency = accent ? 1880.0 : 1320.0;
    const double attack = std::min(1.0, time / 0.0015);
    const double envelope = attack * std::exp(-time * 92.0);
    const double transient = std::sin(twoPi * frequency * time) +
        0.28 * std::sin(twoPi * frequency * 2.07 * time);
    const double accentGain = accent ? 1.0 : 0.72;
    const double volume = std::clamp(volumePercent, 0, 100) / 100.0;
    return std::clamp(transient * envelope * accentGain * volume, -1.0, 1.0);
}

bool valid_bpm(double bpm) {
    return std::isfinite(bpm) && bpm >= kMinimumBpm && bpm <= kMaximumBpm;
}

bool valid_meter(int meter) {
    return meter == 1 || meter == 2 || meter == 3 || meter == 4 || meter == 6;
}

std::wstring endpoint_id(IMMDevice* device) {
    if (device == nullptr) return {};
    LPWSTR rawId = nullptr;
    if (FAILED(device->GetId(&rawId)) || rawId == nullptr) return {};
    std::wstring result(rawId);
    ::CoTaskMemFree(rawId);
    return result;
}

bool default_endpoint_matches(const std::wstring& expectedId) {
    if (expectedId.empty()) return false;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return false;
    ComPtr<IMMDevice> current;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current);
    return SUCCEEDED(hr) && endpoint_id(current.Get()) == expectedId;
}

} // namespace

struct wasapi_metronome_renderer::implementation {
    struct command_state {
        bool active = false;
        double bpm = 120.0;
        int meter = 4;
        int volume = 60;
        double phaseNudgeSeconds = 0.0;
        uint64_t syncSerial = 0;
        uint64_t stateSerial = 0;
    };

    ~implementation() { close(); }

    bool prepare() {
        close();
        m_renderEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_commandEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_renderEvent == nullptr || m_commandEvent == nullptr ||
            m_stopEvent == nullptr) {
            close_handle(m_renderEvent);
            close_handle(m_commandEvent);
            close_handle(m_stopEvent);
            return false;
        }
        m_stopRequested.store(false, std::memory_order_release);
        m_initComplete = false;
        m_initSucceeded = false;
        try {
            m_thread = std::thread([this] { thread_main(); });
        } catch (...) {
            close_handle(m_renderEvent);
            close_handle(m_commandEvent);
            close_handle(m_stopEvent);
            return false;
        }

        std::unique_lock lock(m_initMutex);
        const bool signalled = m_initCv.wait_for(
            lock, std::chrono::milliseconds(2000),
            [this] { return m_initComplete; });
        if (!signalled || !m_initSucceeded) {
            lock.unlock();
            close();
            return false;
        }
        return true;
    }

    void close() {
        m_stopRequested.store(true, std::memory_order_release);
        if (m_stopEvent != nullptr) ::SetEvent(m_stopEvent);
        if (m_thread.joinable()) m_thread.join();
        m_ready.store(false, std::memory_order_release);
        m_syncAppliedCv.notify_all();
        close_handle(m_renderEvent);
        close_handle(m_commandEvent);
        close_handle(m_stopEvent);
    }

    bool ready() const {
        return m_ready.load(std::memory_order_acquire);
    }

    std::string backend_name() const {
        std::lock_guard lock(m_labelMutex);
        return m_backendName;
    }

    void stop() {
        mutate([](command_state& state) { state.active = false; });
    }

    void sync(double bpm, int meter, int volume) {
        if (!ready() || !valid_bpm(bpm) || !valid_meter(meter)) return;
        uint64_t requestedSerial = 0;
        {
            std::lock_guard lock(m_stateMutex);
            auto& state = m_state;
            state.active = true;
            state.bpm = bpm;
            state.meter = meter;
            state.volume = std::clamp(volume, 0, 100);
            state.phaseNudgeSeconds = 0.0;
            requestedSerial = ++state.syncSerial;
            ++state.stateSerial;
        }
        if (m_commandEvent != nullptr) ::SetEvent(m_commandEvent);

        // A cue is a transport command, not a coalescible parameter update.
        // Keep the UI handoff bounded, but do not accept another press before
        // this beat one has actually been submitted and the stream restarted.
        std::unique_lock lock(m_syncAppliedMutex);
        const bool applied = m_syncAppliedCv.wait_for(
            lock, std::chrono::milliseconds(50), [this, requestedSerial] {
                return m_appliedSyncSerial.load(std::memory_order_acquire) >=
                        requestedSerial ||
                    !m_ready.load(std::memory_order_acquire);
            });
        if (!applied && !m_reportedSyncApplyTimeout.exchange(
                            true, std::memory_order_acq_rel)) {
            FB2K_console_formatter()
                << "foo_smart_tempo: [Manual Metronome] WASAPI cue apply "
                << "exceeded 50 ms; input was received but render restart "
                << "was late";
        }
    }

    void set_bpm(double bpm) {
        if (!valid_bpm(bpm)) return;
        mutate([&](command_state& state) { state.bpm = bpm; });
    }

    void set_meter(int meter) {
        if (!valid_meter(meter)) return;
        mutate([&](command_state& state) { state.meter = meter; });
    }

    void set_volume(int volume) {
        mutate([&](command_state& state) {
            state.volume = std::clamp(volume, 0, 100);
        });
    }

    void nudge_seconds(double deltaSeconds) {
        if (!std::isfinite(deltaSeconds)) return;
        mutate([&](command_state& state) {
            state.phaseNudgeSeconds = std::clamp(
                state.phaseNudgeSeconds + deltaSeconds, -0.250, 0.250);
        });
    }

private:
    template <typename Callback>
    void mutate(Callback&& callback) {
        {
            std::lock_guard lock(m_stateMutex);
            callback(m_state);
            ++m_state.stateSerial;
        }
        if (m_commandEvent != nullptr) ::SetEvent(m_commandEvent);
    }

    static void close_handle(HANDLE& handle) {
        if (handle != nullptr) {
            ::CloseHandle(handle);
            handle = nullptr;
        }
    }

    void signal_initialization(bool succeeded) {
        {
            std::lock_guard lock(m_initMutex);
            m_initSucceeded = succeeded;
            m_initComplete = true;
        }
        m_initCv.notify_all();
    }

    bool initialize_client(
        ComPtr<IMMDevice>& device, ComPtr<IAudioClient>& client,
        ComPtr<IAudioRenderClient>& renderClient, UINT32& bufferFrames,
        render_format& outputFormat, double& periodMilliseconds,
        std::wstring& endpointId) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = ::CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) return false;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) return false;
        endpointId = endpoint_id(device.Get());
        if (endpointId.empty()) return false;

        WAVEFORMATEX* rawFormat = nullptr;
        hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) return false;
        hr = client->GetMixFormat(&rawFormat);
        if (FAILED(hr) || rawFormat == nullptr) return false;

        outputFormat = classify_format(rawFormat);
        if (outputFormat.sample == sample_format::unsupported) {
            ::CoTaskMemFree(rawFormat);
            return false;
        }

        bool initialized = false;
        UINT32 requestedPeriodFrames = 0;
        ComPtr<IAudioClient3> client3;
        if (SUCCEEDED(client.As(&client3)) && client3 != nullptr) {
            UINT32 defaultPeriod = 0;
            UINT32 fundamentalPeriod = 0;
            UINT32 minimumPeriod = 0;
            UINT32 maximumPeriod = 0;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(
                    rawFormat, &defaultPeriod, &fundamentalPeriod,
                    &minimumPeriod, &maximumPeriod)) &&
                minimumPeriod > 0 &&
                SUCCEEDED(client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimumPeriod,
                    rawFormat, nullptr))) {
                initialized = true;
                requestedPeriodFrames = minimumPeriod;
            }
        }

        if (!initialized) {
            client.Reset();
            client3.Reset();
            hr = device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                constexpr REFERENCE_TIME defaultBufferDuration = 100000;
                hr = client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    defaultBufferDuration, 0, rawFormat, nullptr);
                initialized = SUCCEEDED(hr);
            }
        }

        if (!initialized) {
            ::CoTaskMemFree(rawFormat);
            return false;
        }

        hr = client->SetEventHandle(m_renderEvent);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(hr)) {
            hr = client->GetService(IID_PPV_ARGS(&renderClient));
        }
        ::CoTaskMemFree(rawFormat);
        if (FAILED(hr) || bufferFrames == 0 || renderClient == nullptr) {
            return false;
        }

        const UINT32 periodFrames = requestedPeriodFrames > 0
            ? requestedPeriodFrames
            : bufferFrames;
        periodMilliseconds =
            1000.0 * periodFrames / outputFormat.sampleRate;
        return true;
    }

    command_state snapshot_state() const {
        std::lock_guard lock(m_stateMutex);
        return m_state;
    }

    bool render_frames(
        IAudioRenderClient* renderClient, UINT32 frameCount,
        const render_format& format, uint64_t submittedFrames,
        const command_state& state, double& nextBeatFrame,
        long long& beatIndex, size_t& clickPosition, bool& clickAccent) {
        if (frameCount == 0) return true;
        BYTE* data = nullptr;
        HRESULT hr = renderClient->GetBuffer(frameCount, &data);
        if (FAILED(hr) || data == nullptr) return false;
        std::memset(
            data, 0, static_cast<size_t>(frameCount) * format.blockAlign);

        const size_t clickFrames = static_cast<size_t>(
            std::ceil(kClickDurationSeconds * format.sampleRate));
        for (UINT32 frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            const double absoluteFrame =
                static_cast<double>(submittedFrames + frameIndex);
            if (state.active && absoluteFrame + 0.5 >= nextBeatFrame) {
                ++beatIndex;
                clickPosition = 0;
                clickAccent = state.meter > 1 &&
                    (beatIndex % state.meter) == 0;
                nextBeatFrame += 60.0 * format.sampleRate / state.bpm;
            }

            if (state.active && clickPosition < clickFrames) {
                const double sample = click_sample(
                    clickPosition, format.sampleRate, clickAccent,
                    state.volume);
                BYTE* frame = data +
                    static_cast<size_t>(frameIndex) * format.blockAlign;
                for (uint16_t channel = 0; channel < format.channels; ++channel) {
                    write_sample(frame, format, channel, sample);
                }
                ++clickPosition;
            }
        }

        hr = renderClient->ReleaseBuffer(frameCount, 0);
        return SUCCEEDED(hr);
    }

    bool render_available(
        IAudioClient* client, IAudioRenderClient* renderClient,
        UINT32 bufferFrames, const render_format& format,
        uint64_t& submittedFrames, command_state& appliedState,
        uint64_t& appliedSyncSerial, double& nextBeatFrame,
        long long& beatIndex, size_t& clickPosition, bool& clickAccent) {
        const command_state state = snapshot_state();
        const bool syncRequested = state.syncSerial != appliedSyncSerial;
        const double oldNudge = appliedState.phaseNudgeSeconds;
        const bool nudgeChanged =
            std::abs(state.phaseNudgeSeconds - oldNudge) > 1e-12;
        const bool bpmChanged = std::abs(state.bpm - appliedState.bpm) > 1e-12;

        if (syncRequested && state.active) {
            // A cue must discard every previously queued click. Merely moving
            // the future phase lets an old beat escape before the new beat 1.
            HRESULT hr = client->Stop();
            if (FAILED(hr)) return false;
            hr = client->Reset();
            if (FAILED(hr)) return false;

            submittedFrames = 0;
            appliedState = state;
            appliedSyncSerial = state.syncSerial;
            beatIndex = 0;
            clickPosition = 0;
            clickAccent = state.meter > 1;
            nextBeatFrame = 60.0 * format.sampleRate / state.bpm;
            if (!render_frames(
                    renderClient, bufferFrames, format, submittedFrames,
                    state, nextBeatFrame, beatIndex, clickPosition,
                    clickAccent)) {
                return false;
            }
            submittedFrames = bufferFrames;
            const HRESULT startResult = client->Start();
            if (SUCCEEDED(startResult)) {
                m_appliedSyncSerial.store(
                    appliedSyncSerial, std::memory_order_release);
                m_syncAppliedCv.notify_all();
                return true;
            }
            return false;
        }

        UINT32 padding = 0;
        HRESULT hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr) || padding > bufferFrames) return false;
        const UINT32 available = bufferFrames - padding;
        if (available == 0) return true;
        appliedState = state;

        if (!state.active) {
            clickPosition = std::numeric_limits<size_t>::max();
        } else {
            if (bpmChanged) {
                nextBeatFrame = static_cast<double>(submittedFrames) +
                    60.0 * format.sampleRate / state.bpm;
            }
            if (nudgeChanged) {
                nextBeatFrame +=
                    (state.phaseNudgeSeconds - oldNudge) * format.sampleRate;
            }
        }

        if (!render_frames(
                renderClient, available, format, submittedFrames, state,
                nextBeatFrame, beatIndex, clickPosition, clickAccent)) {
            return false;
        }
        submittedFrames += available;
        return true;
    }

    void thread_main() {
        const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitializeCom = SUCCEEDED(comResult);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            signal_initialization(false);
            if (uninitializeCom) ::CoUninitialize();
            return;
        }

        DWORD mmcssTaskIndex = 0;
        HANDLE mmcssHandle = ::AvSetMmThreadCharacteristicsW(
            L"Pro Audio", &mmcssTaskIndex);
        const auto finishThread = [&] {
            if (mmcssHandle != nullptr) {
                ::AvRevertMmThreadCharacteristics(mmcssHandle);
            }
            if (uninitializeCom) ::CoUninitialize();
        };

        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> renderClient;
        UINT32 bufferFrames = 0;
        render_format format;
        double periodMilliseconds = 0.0;
        std::wstring endpointId;
        if (!initialize_client(
                device, client, renderClient, bufferFrames, format,
                periodMilliseconds, endpointId)) {
            signal_initialization(false);
            finishThread();
            return;
        }

        BYTE* initial = nullptr;
        HRESULT hr = renderClient->GetBuffer(bufferFrames, &initial);
        if (SUCCEEDED(hr) && initial != nullptr) {
            std::memset(
                initial, 0,
                static_cast<size_t>(bufferFrames) * format.blockAlign);
            hr = renderClient->ReleaseBuffer(
                bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) {
            signal_initialization(false);
            finishThread();
            return;
        }

        {
            std::lock_guard lock(m_labelMutex);
            char label[96]{};
            std::snprintf(
                label, sizeof(label), "WASAPI shared %.2f ms",
                periodMilliseconds);
            m_backendName = label;
        }
        m_ready.store(true, std::memory_order_release);
        signal_initialization(true);

        uint64_t submittedFrames = bufferFrames;
        command_state appliedState = snapshot_state();
        uint64_t appliedSyncSerial = appliedState.syncSerial;
        double nextBeatFrame = std::numeric_limits<double>::infinity();
        long long beatIndex = 0;
        size_t clickPosition = std::numeric_limits<size_t>::max();
        bool clickAccent = false;
        HANDLE waits[] = {m_stopEvent, m_commandEvent, m_renderEvent};
        ULONGLONG nextEndpointCheck = ::GetTickCount64() + 1000;

        while (!m_stopRequested.load(std::memory_order_acquire)) {
            const DWORD waitResult = ::WaitForMultipleObjects(
                static_cast<DWORD>(std::size(waits)), waits, FALSE, 1000);
            if (waitResult == WAIT_OBJECT_0) break;
            if (waitResult != WAIT_OBJECT_0 + 1 &&
                waitResult != WAIT_OBJECT_0 + 2 &&
                waitResult != WAIT_TIMEOUT) {
                break;
            }
            // Render events arrive continuously, so endpoint checks must use
            // their own wall-clock cadence instead of relying on wait timeout.
            // Exiting marks the renderer failed; the UI controller then
            // restarts beat one through Memory-WAV on the new default device.
            const ULONGLONG now = ::GetTickCount64();
            if (now >= nextEndpointCheck &&
                !default_endpoint_matches(endpointId)) {
                break;
            }
            if (now >= nextEndpointCheck) nextEndpointCheck = now + 1000;
            if (!render_available(
                    client.Get(), renderClient.Get(), bufferFrames, format,
                    submittedFrames, appliedState, appliedSyncSerial,
                    nextBeatFrame, beatIndex, clickPosition, clickAccent)) {
                break;
            }
        }

        m_ready.store(false, std::memory_order_release);
        m_syncAppliedCv.notify_all();
        client->Stop();
        client->Reset();
        renderClient.Reset();
        client.Reset();
        device.Reset();
        finishThread();
    }

    mutable std::mutex m_stateMutex;
    command_state m_state;
    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_ready{false};
    std::atomic<uint64_t> m_appliedSyncSerial{0};
    std::atomic<bool> m_reportedSyncApplyTimeout{false};
    std::mutex m_syncAppliedMutex;
    std::condition_variable m_syncAppliedCv;
    HANDLE m_renderEvent = nullptr;
    HANDLE m_commandEvent = nullptr;
    HANDLE m_stopEvent = nullptr;

    mutable std::mutex m_initMutex;
    std::condition_variable m_initCv;
    bool m_initComplete = false;
    bool m_initSucceeded = false;

    mutable std::mutex m_labelMutex;
    std::string m_backendName = "Memory WAV fallback";
};

wasapi_metronome_renderer::wasapi_metronome_renderer()
    : m_impl(std::make_unique<implementation>()) {}

wasapi_metronome_renderer::~wasapi_metronome_renderer() = default;

bool wasapi_metronome_renderer::prepare() { return m_impl->prepare(); }
void wasapi_metronome_renderer::close() { m_impl->close(); }
bool wasapi_metronome_renderer::ready() const { return m_impl->ready(); }
std::string wasapi_metronome_renderer::backend_name() const {
    return m_impl->backend_name();
}
void wasapi_metronome_renderer::stop() { m_impl->stop(); }
void wasapi_metronome_renderer::sync(
    double bpm, int meter, int volumePercent) {
    m_impl->sync(bpm, meter, volumePercent);
}
void wasapi_metronome_renderer::set_bpm(double bpm) { m_impl->set_bpm(bpm); }
void wasapi_metronome_renderer::set_meter(int meter) { m_impl->set_meter(meter); }
void wasapi_metronome_renderer::set_volume(int volumePercent) {
    m_impl->set_volume(volumePercent);
}
void wasapi_metronome_renderer::nudge_seconds(double deltaSeconds) {
    m_impl->nudge_seconds(deltaSeconds);
}

} // namespace smart_tempo::manual_metronome
