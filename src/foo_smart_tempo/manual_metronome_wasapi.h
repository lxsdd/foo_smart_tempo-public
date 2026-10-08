#pragma once

#include <memory>
#include <string>

namespace smart_tempo::manual_metronome {

// Optional low-latency renderer. It is opened at runtime and never raises the
// component's Windows 7 baseline; callers retain the proven Memory-WAV path.
class wasapi_metronome_renderer {
public:
    wasapi_metronome_renderer();
    ~wasapi_metronome_renderer();

    wasapi_metronome_renderer(const wasapi_metronome_renderer&) = delete;
    wasapi_metronome_renderer& operator=(
        const wasapi_metronome_renderer&) = delete;

    bool prepare();
    void close();
    bool ready() const;
    std::string backend_name() const;

    void stop();
    void sync(double bpm, int meter, int volumePercent);
    void set_bpm(double bpm);
    void set_meter(int meter);
    void set_volume(int volumePercent);
    void nudge_seconds(double deltaSeconds);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

} // namespace smart_tempo::manual_metronome
