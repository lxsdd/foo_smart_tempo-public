#include "stdafx.h"

#include "manual_metronome.h"
#include "manual_metronome_wasapi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <commctrl.h>
#include <mmsystem.h>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

namespace smart_tempo::manual_metronome {
namespace {

constexpr double kMinimumBpm = 40.0;
constexpr double kMaximumBpm = 220.0;
constexpr int kSampleRate = 44100;
constexpr double kClickDurationSeconds = 0.035;
constexpr int kVolumeSteps = 100;
constexpr double kMaximumPhaseNudgeSeconds = 0.250;

struct volume_slider_state {
    HWND owner = nullptr;
    int wheelDeltaRemainder = 0;
};

std::vector<int16_t> make_click_samples(double frequency, double gain) {
    const int sampleCount = static_cast<int>(
        std::lround(kClickDurationSeconds * static_cast<double>(kSampleRate)));
    std::vector<int16_t> samples;
    samples.reserve(sampleCount);

    constexpr double twoPi = 6.28318530717958647692;
    for (int sample = 0; sample < sampleCount; ++sample) {
        const double time = static_cast<double>(sample) / kSampleRate;
        const double attack = std::min(1.0, time / 0.0015);
        const double envelope = attack * std::exp(-time * 92.0);
        const double transient = std::sin(twoPi * frequency * time) +
                                 0.28 * std::sin(twoPi * frequency * 2.07 * time);
        const double normalized = std::clamp(transient * envelope * gain, -1.0, 1.0);
        samples.push_back(
            static_cast<int16_t>(std::lround(normalized * 28000.0)));
    }
    return samples;
}

void append_u16(std::vector<uint8_t>& target, uint16_t value) {
    target.push_back(static_cast<uint8_t>(value & 0xff));
    target.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void append_u32(std::vector<uint8_t>& target, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        target.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
    }
}

std::vector<uint8_t> make_wave_image(const std::vector<int16_t>& samples) {
    const uint32_t dataBytes = static_cast<uint32_t>(
        samples.size() * sizeof(int16_t));
    std::vector<uint8_t> wave;
    wave.reserve(44 + dataBytes);
    wave.insert(wave.end(), {'R', 'I', 'F', 'F'});
    append_u32(wave, 36 + dataBytes);
    wave.insert(wave.end(), {'W', 'A', 'V', 'E'});
    wave.insert(wave.end(), {'f', 'm', 't', ' '});
    append_u32(wave, 16);
    append_u16(wave, WAVE_FORMAT_PCM);
    append_u16(wave, 1);
    append_u32(wave, kSampleRate);
    append_u32(wave, kSampleRate * sizeof(int16_t));
    append_u16(wave, sizeof(int16_t));
    append_u16(wave, 16);
    wave.insert(wave.end(), {'d', 'a', 't', 'a'});
    append_u32(wave, dataBytes);
    const auto* bytes = reinterpret_cast<const uint8_t*>(samples.data());
    wave.insert(wave.end(), bytes, bytes + dataBytes);
    return wave;
}

double monotonic_clock_seconds() {
    static const double inverseFrequency = [] {
        LARGE_INTEGER frequency{};
        return ::QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0
            ? 1.0 / static_cast<double>(frequency.QuadPart)
            : 0.0;
    }();
    LARGE_INTEGER counter{};
    if (inverseFrequency <= 0.0 || !::QueryPerformanceCounter(&counter)) {
        return static_cast<double>(::GetTickCount64()) / 1000.0;
    }
    return static_cast<double>(counter.QuadPart) * inverseFrequency;
}

long long beat_at_time(double clockSeconds, double anchor, double period) {
    if (!(period > 0.0) || !std::isfinite(clockSeconds) ||
        !std::isfinite(anchor)) {
        return 0;
    }
    return static_cast<long long>(
        std::floor((clockSeconds - anchor) / period + 1e-7));
}

long long positive_mod(long long value, int modulus) {
    if (modulus <= 1) return 0;
    const long long result = value % modulus;
    return result < 0 ? result + modulus : result;
}

bool valid_meter(int beatsPerBar) {
    return beatsPerBar == 1 || beatsPerBar == 2 || beatsPerBar == 3 ||
           beatsPerBar == 4 || beatsPerBar == 6;
}

constexpr UINT_PTR kVolumeSliderSubclassId = 0x46535456;
constexpr UINT_PTR kCueButtonSubclassId = 0x46535443;

LRESULT CALLBACK cue_button_subclass(
    HWND button, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR ownerValue) {
    // A Win32 double-click sequence uses WM_LBUTTONDBLCLK instead of a second
    // WM_LBUTTONDOWN. Treat both as the same physical cue press edge.
    const bool mousePress =
        message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK;
    const bool initialSpacePress = message == WM_KEYDOWN && wParam == VK_SPACE &&
        (static_cast<uint32_t>(lParam) & (1u << 30)) == 0;
    if (mousePress || initialSpacePress) {
        if (mousePress) {
            // Own the complete pointer press. Passing WM_LBUTTONDOWN into the
            // native button state machine after the cue creates a second,
            // release-edge interaction path that is observably later than the
            // direct candidate-list Space command.
            ::SetFocus(button);
            ::SetCapture(button);
            ::SendMessage(button, BM_SETSTATE, TRUE, 0);
        }
        const HWND owner = reinterpret_cast<HWND>(ownerValue);
        if (::IsWindow(owner)) {
            ::SendMessage(
                owner, WM_COMMAND,
                MAKEWPARAM(::GetDlgCtrlID(button), BN_PUSHED),
                reinterpret_cast<LPARAM>(button));
        }
        if (mousePress) return 0;
    }
    if (message == WM_LBUTTONUP) {
        if (::GetCapture() == button) ::ReleaseCapture();
        ::SendMessage(button, BM_SETSTATE, FALSE, 0);
        return 0;
    }
    if (message == WM_CAPTURECHANGED) {
        ::SendMessage(button, BM_SETSTATE, FALSE, 0);
    }
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(button, cue_button_subclass, subclassId);
    }
    return ::DefSubclassProc(button, message, wParam, lParam);
}

LRESULT CALLBACK volume_slider_subclass(
    HWND slider, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR stateValue) {
    auto* state = reinterpret_cast<volume_slider_state*>(stateValue);
    const HWND owner = state != nullptr ? state->owner : nullptr;
    const auto notify_owner = [slider, owner](UINT request, int position) {
        if (!::IsWindow(owner)) return;
        ::SendMessage(owner, WM_HSCROLL,
                      MAKEWPARAM(request, position),
                      reinterpret_cast<LPARAM>(slider));
    };
    const auto update_position = [slider, &notify_owner](
                                     int position, UINT request) {
        const int minimum = static_cast<int>(
            ::SendMessage(slider, TBM_GETRANGEMIN, 0, 0));
        const int maximum = static_cast<int>(
            ::SendMessage(slider, TBM_GETRANGEMAX, 0, 0));
        const int bounded = std::clamp(position, minimum, maximum);
        ::SendMessage(slider, TBM_SETPOS, TRUE, bounded);
        notify_owner(request, bounded);
        return bounded;
    };
    const auto position_from_pointer = [slider](LPARAM pointer) {
        RECT channel{};
        ::SendMessage(slider, TBM_GETCHANNELRECT, 0,
                      reinterpret_cast<LPARAM>(&channel));
        if (channel.right <= channel.left) {
            ::GetClientRect(slider, &channel);
        }
        const int left = static_cast<int>(channel.left);
        const int right = std::max(left + 1, static_cast<int>(channel.right));
        const int x = std::clamp(
            static_cast<int>(static_cast<short>(LOWORD(pointer))), left, right);
        const int minimum = static_cast<int>(
            ::SendMessage(slider, TBM_GETRANGEMIN, 0, 0));
        const int maximum = static_cast<int>(
            ::SendMessage(slider, TBM_GETRANGEMAX, 0, 0));
        const double fraction =
            static_cast<double>(x - left) / static_cast<double>(right - left);
        return minimum + static_cast<int>(
            std::lround(fraction * static_cast<double>(maximum - minimum)));
    };
    const auto update_from_pointer = [&](LPARAM pointer, UINT request) {
        return update_position(position_from_pointer(pointer), request);
    };

    // Own pointer tracking end-to-end. Passing the initial press to the native
    // trackbar as well would apply a second page-step to the same interaction.
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        ::SetFocus(slider);
        ::SetCapture(slider);
        update_from_pointer(lParam, TB_THUMBTRACK);
        return 0;
    }
    if (message == WM_MOUSEMOVE && ::GetCapture() == slider) {
        update_from_pointer(lParam, TB_THUMBTRACK);
        return 0;
    }
    if (message == WM_LBUTTONUP && ::GetCapture() == slider) {
        const int position = update_from_pointer(lParam, TB_THUMBPOSITION);
        ::ReleaseCapture();
        notify_owner(TB_ENDTRACK, position);
        return 0;
    }
    if (message == WM_KEYDOWN) {
        const int current = static_cast<int>(
            ::SendMessage(slider, TBM_GETPOS, 0, 0));
        const int line = static_cast<int>(
            ::SendMessage(slider, TBM_GETLINESIZE, 0, 0));
        const int page = static_cast<int>(
            ::SendMessage(slider, TBM_GETPAGESIZE, 0, 0));
        int target = current;
        switch (wParam) {
        case VK_UP:
        case VK_RIGHT:
            target += line;
            break;
        case VK_DOWN:
        case VK_LEFT:
            target -= line;
            break;
        case VK_PRIOR:
            target += page;
            break;
        case VK_NEXT:
            target -= page;
            break;
        case VK_HOME:
            target = static_cast<int>(
                ::SendMessage(slider, TBM_GETRANGEMIN, 0, 0));
            break;
        case VK_END:
            target = static_cast<int>(
                ::SendMessage(slider, TBM_GETRANGEMAX, 0, 0));
            break;
        default:
            return ::DefSubclassProc(slider, message, wParam, lParam);
        }
        const int position = update_position(target, TB_THUMBPOSITION);
        notify_owner(TB_ENDTRACK, position);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && state != nullptr) {
        state->wheelDeltaRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
        const int steps = state->wheelDeltaRemainder / WHEEL_DELTA;
        state->wheelDeltaRemainder -= steps * WHEEL_DELTA;
        if (steps != 0) {
            const int current = static_cast<int>(
                ::SendMessage(slider, TBM_GETPOS, 0, 0));
            const int line = static_cast<int>(
                ::SendMessage(slider, TBM_GETLINESIZE, 0, 0));
            const int position = update_position(
                current + steps * line, TB_THUMBPOSITION);
            notify_owner(TB_ENDTRACK, position);
        }
        return 0;
    }
    if (message == WM_CANCELMODE && ::GetCapture() == slider) {
        ::ReleaseCapture();
        return 0;
    }
    if (message == WM_CAPTURECHANGED) {
        return 0;
    }
    if (message == WM_NCDESTROY) {
        if (::GetCapture() == slider) ::ReleaseCapture();
        ::RemoveWindowSubclass(slider, volume_slider_subclass, subclassId);
        delete state;
    }
    return ::DefSubclassProc(slider, message, wParam, lParam);
}

} // namespace

struct controller::audio_bank {
    std::array<std::vector<uint8_t>, kVolumeSteps + 1> normal;
    std::array<std::vector<uint8_t>, kVolumeSteps + 1> accent;
    wasapi_metronome_renderer wasapi;
    bool wasapiAttempted = false;

    audio_bank() {
        for (int step = 0; step <= kVolumeSteps; ++step) {
            const double gain = static_cast<double>(step) / kVolumeSteps;
            normal[step] = make_wave_image(
                make_click_samples(1320.0, gain * 0.72));
            accent[step] = make_wave_image(
                make_click_samples(1880.0, gain));
        }
    }

    ~audio_bank() { close(); }

    bool open(HWND owner) {
        if (!::IsWindow(owner)) return false;
        if (!wasapiAttempted) {
            wasapiAttempted = true;
            const bool opened = wasapi.prepare();
            FB2K_console_formatter() <<
                "foo_smart_tempo: [Manual Metronome] backend=\"" <<
                (opened ? wasapi.backend_name().c_str()
                        : "Memory WAV fallback") <<
                "\", shared_mode=" << (opened ? 1 : 0) <<
                ", fallback_available=1";
        }
        return true;
    }

    void stop() {
        wasapi.stop();
        ::PlaySoundW(nullptr, nullptr, 0);
    }

    void close() {
        stop();
        wasapi.close();
        wasapiAttempted = false;
    }

    bool timeline_ready() const { return wasapi.ready(); }

    bool sync_timeline(double bpm, int meter, int volumePercent) {
        if (!wasapi.ready()) return false;
        wasapi.sync(bpm, meter, volumePercent);
        return wasapi.ready();
    }

    void set_bpm(double bpm) { wasapi.set_bpm(bpm); }
    void set_meter(int meter) { wasapi.set_meter(meter); }
    void set_volume(int volumePercent) { wasapi.set_volume(volumePercent); }
    void nudge_seconds(double seconds) { wasapi.nudge_seconds(seconds); }

    bool play(bool useAccent, int volumePercent, bool restartCurrent) {
        if (restartCurrent) stop();
        const int step = std::clamp(volumePercent, 0, kVolumeSteps);
        const auto& wave = useAccent ? accent[step] : normal[step];
        if (wave.empty()) return false;
        return ::PlaySoundW(
            reinterpret_cast<LPCWSTR>(wave.data()), nullptr,
            SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != FALSE;
    }
};

void initialize_volume_slider(HWND slider, HWND owner, int initialPercent) {
    if (!::IsWindow(slider) || !::IsWindow(owner)) return;
    ::SendMessage(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    ::SendMessage(slider, TBM_SETPAGESIZE, 0, 10);
    ::SendMessage(slider, TBM_SETLINESIZE, 0, 1);
    ::SendMessage(slider, TBM_SETPOS, TRUE,
                  std::clamp(initialPercent, 0, 100));
    DWORD_PTR existingValue = 0;
    if (::GetWindowSubclass(
            slider, volume_slider_subclass, kVolumeSliderSubclassId,
            &existingValue)) {
        auto* state = reinterpret_cast<volume_slider_state*>(existingValue);
        if (state != nullptr) {
            state->owner = owner;
            state->wheelDeltaRemainder = 0;
        }
        return;
    }
    auto* state = new volume_slider_state{owner, 0};
    if (!::SetWindowSubclass(
            slider, volume_slider_subclass, kVolumeSliderSubclassId,
            reinterpret_cast<DWORD_PTR>(state))) {
        delete state;
    }
}

void install_cue_button(HWND button, HWND owner) {
    if (!::IsWindow(button) || !::IsWindow(owner)) return;
    ::SetWindowSubclass(
        button, cue_button_subclass, kCueButtonSubclassId,
        reinterpret_cast<DWORD_PTR>(owner));
}

controller& controller::instance() {
    static controller value;
    return value;
}

controller::controller() : m_audio(new audio_bank()) {}

controller::~controller() {
    if (m_timerPeriodActive) ::timeEndPeriod(1);
    delete m_audio;
}

bool controller::prepare(HWND owner) {
    return m_audio != nullptr && m_audio->open(owner);
}

bool controller::enable(
    HWND owner, double bpmValue, int beatsPerBar, int volumePercent) {
    if (!::IsWindow(owner) || !std::isfinite(bpmValue) ||
        bpmValue < kMinimumBpm || bpmValue > kMaximumBpm ||
        !valid_meter(beatsPerBar) || m_audio == nullptr ||
        !m_audio->open(owner)) {
        return false;
    }
    m_owner = owner;
    m_enabled = true;
    m_bpm = bpmValue;
    m_anchorClockSeconds = 0.0;
    m_lastClockSeconds = -1.0;
    m_lastBeat = 0;
    m_phaseNudgeSeconds = 0.0;
    m_meter = beatsPerBar;
    m_volume = std::clamp(volumePercent, 0, 100);
    m_usingWasapi = false;
    m_reportedRuntimeFallback = false;
    if (!m_timerPeriodActive && ::timeBeginPeriod(1) == TIMERR_NOERROR) {
        m_timerPeriodActive = true;
    }
    start_at_clock(monotonic_clock_seconds(), true);
    return true;
}

void controller::disable(HWND owner) {
    if (owner != m_owner) return;
    m_enabled = false;
    m_usingWasapi = false;
    if (m_timerPeriodActive) {
        ::timeEndPeriod(1);
        m_timerPeriodActive = false;
    }
    if (m_audio != nullptr) m_audio->stop();
}

void controller::release(HWND owner) {
    if (owner != m_owner) return;
    m_enabled = false;
    m_usingWasapi = false;
    m_owner = nullptr;
    m_lastClockSeconds = -1.0;
    if (m_timerPeriodActive) {
        ::timeEndPeriod(1);
        m_timerPeriodActive = false;
    }
    if (m_audio != nullptr) m_audio->close();
}

bool controller::is_active_for(HWND owner) const {
    return m_enabled && owner != nullptr && owner == m_owner;
}

void controller::set_bpm(HWND owner, double bpmValue) {
    if (owner != m_owner || !std::isfinite(bpmValue) ||
        bpmValue < kMinimumBpm || bpmValue > kMaximumBpm) {
        return;
    }
    if (std::abs(m_bpm - bpmValue) < 1e-9) return;
    m_bpm = bpmValue;
    if (m_audio != nullptr) m_audio->set_bpm(m_bpm);

    const double clockSeconds = monotonic_clock_seconds();
    const double period = 60.0 / m_bpm;
    m_lastBeat = beat_at_time(
        clockSeconds, m_anchorClockSeconds, period);
    m_lastClockSeconds = clockSeconds;
}

double controller::bpm() const { return m_bpm; }

void controller::sync_now(HWND owner) {
    if (owner != m_owner || !m_enabled) return;
    // Start/Sync is a DJ-cue-style restart: the immediate click is beat one
    // and an independent monotonic clock continues from that audible reference.
    m_phaseNudgeSeconds = 0.0;
    start_at_clock(monotonic_clock_seconds(), true);
}

void controller::nudge_seconds(HWND owner, double seconds) {
    if (owner != m_owner || !m_enabled || !std::isfinite(seconds)) return;
    const double nudged = std::clamp(
        m_phaseNudgeSeconds + seconds,
        -kMaximumPhaseNudgeSeconds,
        kMaximumPhaseNudgeSeconds);
    m_anchorClockSeconds += nudged - m_phaseNudgeSeconds;
    m_phaseNudgeSeconds = nudged;
    if (m_audio != nullptr && m_usingWasapi) {
        m_audio->nudge_seconds(seconds);
    }
    const double clockSeconds = monotonic_clock_seconds();
    const double period = 60.0 / m_bpm;
    // Moving a grid must never replay a beat that was already emitted.
    m_lastBeat = std::max(
        m_lastBeat,
        beat_at_time(clockSeconds, m_anchorClockSeconds, period));
    m_lastClockSeconds = clockSeconds;
}

double controller::phase_nudge_milliseconds() const {
    return m_phaseNudgeSeconds * 1000.0;
}

void controller::set_meter(HWND owner, int beatsPerBar) {
    if (owner != m_owner) return;
    if (!valid_meter(beatsPerBar)) return;
    m_meter = beatsPerBar;
    if (m_audio != nullptr) m_audio->set_meter(m_meter);
}

int controller::meter() const { return m_meter; }

void controller::set_volume(HWND owner, int percent) {
    if (owner != m_owner) return;
    m_volume = std::clamp(percent, 0, 100);
    if (m_audio != nullptr) m_audio->set_volume(m_volume);
}

int controller::volume() const { return m_volume; }

void controller::start_at_clock(double clockSeconds, bool emitFirstClick) {
    m_anchorClockSeconds = clockSeconds + m_phaseNudgeSeconds;
    m_lastClockSeconds = clockSeconds;
    m_lastBeat = 0;
    if (emitFirstClick && m_audio != nullptr &&
        m_audio->sync_timeline(m_bpm, m_meter, m_volume)) {
        m_usingWasapi = true;
        return;
    }
    m_usingWasapi = false;
    if (emitFirstClick) play_click(m_meter > 1, true);
}

void controller::tick(HWND owner) {
    if (owner != m_owner || !m_enabled || !std::isfinite(m_bpm) ||
        m_bpm < kMinimumBpm || m_bpm > kMaximumBpm) {
        return;
    }

    const double clockSeconds = monotonic_clock_seconds();
    if (m_usingWasapi && m_audio != nullptr) {
        if (m_audio->timeline_ready()) return;
        m_usingWasapi = false;
        if (!m_reportedRuntimeFallback) {
            m_reportedRuntimeFallback = true;
            FB2K_console_formatter() <<
                "foo_smart_tempo: [Manual Metronome] WASAPI stream lost; "
                "restarting beat one with Memory WAV fallback";
        }
        m_phaseNudgeSeconds = 0.0;
        start_at_clock(clockSeconds, true);
        return;
    }
    const double period = 60.0 / m_bpm;
    if (m_lastClockSeconds < 0.0) {
        start_at_clock(clockSeconds, true);
        return;
    }

    const long long beat = beat_at_time(
        clockSeconds, m_anchorClockSeconds, period);
    if (beat > m_lastBeat) {
        // Never emit a burst after a delayed UI tick; the next audible beat is enough.
        m_lastBeat = beat;
        const bool accent = m_meter > 1 &&
            positive_mod(beat, m_meter) == 0;
        play_click(accent);
    }
    m_lastClockSeconds = clockSeconds;
}

void controller::play_click(bool accent, bool restartCurrent) {
    if (m_volume <= 0 || m_audio == nullptr) return;
    m_audio->play(accent, m_volume, restartCurrent);
}

} // namespace smart_tempo::manual_metronome
