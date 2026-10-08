#pragma once

#include "foobar2000/SDK/foobar2000.h"

namespace smart_tempo::manual_metronome {

void initialize_volume_slider(HWND slider, HWND owner, int initialPercent);
void install_cue_button(HWND button, HWND owner);

class controller {
public:
    static controller& instance();

    bool prepare(HWND owner);
    bool enable(HWND owner, double bpm, int beatsPerBar, int volumePercent);
    void disable(HWND owner);
    void release(HWND owner);
    bool is_active_for(HWND owner) const;

    void set_bpm(HWND owner, double bpm);
    double bpm() const;

    void sync_now(HWND owner);
    void nudge_seconds(HWND owner, double seconds);
    double phase_nudge_milliseconds() const;

    void set_meter(HWND owner, int beatsPerBar);
    int meter() const;
    void set_volume(HWND owner, int percent);
    int volume() const;

    void tick(HWND owner);

private:
    controller();
    ~controller();
    controller(const controller&) = delete;
    controller& operator=(const controller&) = delete;

    void start_at_clock(double clockSeconds, bool emitFirstClick);
    void play_click(bool accent, bool restartCurrent = false);

    HWND m_owner = nullptr;
    bool m_enabled = false;
    double m_bpm = 0.0;
    double m_anchorClockSeconds = 0.0;
    double m_lastClockSeconds = -1.0;
    long long m_lastBeat = 0;
    double m_phaseNudgeSeconds = 0.0;
    int m_meter = 4;
    int m_volume = 60;
    bool m_timerPeriodActive = false;
    bool m_usingWasapi = false;
    bool m_reportedRuntimeFallback = false;

    struct audio_bank;
    audio_bank* m_audio = nullptr;
};

} // namespace smart_tempo::manual_metronome
