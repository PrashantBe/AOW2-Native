#pragma once

// A small General MIDI synthesizer for the game's music, so the handheld needs no
// sound fonts or MIDI libraries. It covers what the game's tracks use: notes,
// program changes, channel volume and expression, tempo, and the drum channel.

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace midi {

struct Event {
    std::uint32_t tick;
    std::uint8_t status, a, b;
};

struct Song {
    int division = 480;
    std::vector<Event> events;           // All tracks merged in time order.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tempos; // tick, microseconds per beat
    std::uint32_t length = 0;
};

// Returns nullptr if the data is not a Standard MIDI File.
std::shared_ptr<const Song> parse(const std::vector<std::uint8_t>& data);

// Plays at most one song at a time. Safe to call from any thread.
class Synth {
public:
    explicit Synth(int sampleRate) : rate_(sampleRate) {}
    void play(std::shared_ptr<const Song> song, int loops); // loops < 0 repeats forever.
    void stop();
    void setVolume(int percent);
    bool playing();
    void render(std::int16_t* out, int frames); // Mono.

private:
    struct Voice {
        int channel = -1, note = 0;
        bool held = false, drum = false;
        double phase = 0, step = 0, sweep = 0;
        float gain = 0, level = 0, release = 0;
        int age = 0, kind = 0;
        float low = 0; // One-pole low-pass state.
    };
    struct Channel {
        int program = 0;
        float volume = 100 / 127.0f, expression = 1.0f;
    };
    void reset();
    void dispatch(const Event& event);
    void noteOn(int channel, int note, int velocity);
    void noteOff(int channel, int note);
    float sample(Voice& voice);
    double samplesPerTick() const;

    std::mutex mutex_;
    const int rate_;
    std::shared_ptr<const Song> song_;
    int loops_ = 0;
    std::size_t next_ = 0;
    std::size_t tempoIndex_ = 0;
    double tickClock_ = 0; // Fractional ticks elapsed.
    std::uint32_t tick_ = 0;
    float volume_ = 1.0f;
    Channel channels_[16];
    Voice voices_[32];
    std::uint32_t noise_ = 0x12345678;
};

} // namespace midi
