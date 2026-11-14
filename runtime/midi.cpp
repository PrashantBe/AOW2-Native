#include "midi.hpp"

#include <algorithm>
#include <cmath>

namespace midi {

namespace {
constexpr double Pi = 3.14159265358979323846;

struct Reader {
    const std::vector<std::uint8_t>& data;
    std::size_t at = 0;
    bool ok = true;
    std::uint32_t big(int bytes) {
        std::uint32_t value = 0;
        for (int i = 0; i < bytes; ++i) value = value << 8 | byte();
        return value;
    }
    std::uint8_t byte() {
        if (at >= data.size()) {
            ok = false;
            return 0;
        }
        return data[at++];
    }
    std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto b = byte();
            value = value << 7 | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return value;
    }
};
} // namespace

std::shared_ptr<const Song> parse(const std::vector<std::uint8_t>& data) {
    Reader in{data};
    if (in.big(4) != 0x4D546864) return nullptr; // "MThd"
    const auto headerLength = in.big(4);
    in.big(2);
    const auto tracks = in.big(2);
    const auto division = in.big(2);
    in.at += headerLength - 6;
    if (!in.ok || division == 0 || (division & 0x8000)) return nullptr; // SMPTE timing is not used by the game.

    auto song = std::make_shared<Song>();
    song->division = static_cast<int>(division);
    for (std::uint32_t track = 0; track < tracks && in.ok; ++track) {
        if (in.big(4) != 0x4D54726B) return nullptr; // "MTrk"
        const auto end = in.at + in.big(4);
        std::uint32_t tick = 0;
        std::uint8_t running = 0;
        while (in.ok && in.at < end) {
            tick += in.vlq();
            std::uint8_t status = in.byte();
            if (status < 0x80) {
                status = running;
                --in.at;
            } else if (status < 0xF0) {
                running = status;
            }
            if (status == 0xFF) {
                const auto type = in.byte();
                const auto length = in.vlq();
                if (type == 0x51 && length == 3) {
                    song->tempos.emplace_back(tick, in.big(3));
                } else {
                    in.at += length;
                }
                if (type == 0x2F) break;
            } else if (status == 0xF0 || status == 0xF7) {
                in.at += in.vlq();
            } else if (status >= 0x80) {
                const auto a = in.byte();
                const auto high = status & 0xF0;
                const std::uint8_t b = high == 0xC0 || high == 0xD0 ? 0 : in.byte();
                song->events.push_back({tick, status, a, b});
            } else {
                return nullptr;
            }
            song->length = std::max(song->length, tick);
        }
        in.at = end;
    }
    if (!in.ok) return nullptr;
    std::stable_sort(song->events.begin(), song->events.end(),
                     [](const Event& left, const Event& right) { return left.tick < right.tick; });
    std::stable_sort(song->tempos.begin(), song->tempos.end());
    return song;
}

void Synth::play(std::shared_ptr<const Song> song, int loops) {
    std::lock_guard<std::mutex> guard(mutex_);
    song_ = std::move(song);
    loops_ = loops;
    reset();
}

void Synth::stop() {
    std::lock_guard<std::mutex> guard(mutex_);
    song_.reset();
    for (auto& voice : voices_) voice.held = false;
}

void Synth::setVolume(int percent) {
    std::lock_guard<std::mutex> guard(mutex_);
    volume_ = std::clamp(percent, 0, 100) / 100.0f;
}

bool Synth::playing() {
    std::lock_guard<std::mutex> guard(mutex_);
    return song_ != nullptr;
}

void Synth::reset() {
    next_ = 0;
    tempoIndex_ = 0;
    tickClock_ = 0;
    tick_ = 0;
    for (auto& channel : channels_) channel = Channel();
    for (auto& voice : voices_) voice = Voice();
}

double Synth::samplesPerTick() const {
    std::uint32_t tempo = 500000;
    if (song_ && tempoIndex_ > 0) tempo = song_->tempos[tempoIndex_ - 1].second;
    return rate_ * (tempo / 1e6) / song_->division;
}

void Synth::dispatch(const Event& event) {
    const int channel = event.status & 0x0F;
    switch (event.status & 0xF0) {
    case 0x90:
        if (event.b > 0) {
            noteOn(channel, event.a, event.b);
            break;
        }
        [[fallthrough]];
    case 0x80: noteOff(channel, event.a); break;
    case 0xC0: channels_[channel].program = event.a; break;
    case 0xB0:
        if (event.a == 7) channels_[channel].volume = event.b / 127.0f;
        else if (event.a == 11) channels_[channel].expression = event.b / 127.0f;
        else if (event.a == 120 || event.a == 123) {
            for (auto& voice : voices_) if (voice.channel == channel) voice.held = false;
        } else if (event.a == 121) {
            channels_[channel] = Channel();
        }
        break;
    default: break;
    }
}

void Synth::noteOn(int channel, int note, int velocity) {
    // Take a free voice, or the quietest one.
    Voice* slot = &voices_[0];
    for (auto& voice : voices_) {
        if (voice.channel < 0) {
            slot = &voice;
            break;
        }
        if (voice.level < slot->level) slot = &voice;
    }
    Voice voice;
    voice.channel = channel;
    voice.note = note;
    voice.held = true;
    voice.drum = channel == 9;
    voice.gain = velocity / 127.0f;
    voice.level = 0;
    const auto& state = channels_[channel];
    if (voice.drum) {
        // Kick, snare, hi-hats, cymbals, then toms and other percussion.
        if (note == 35 || note == 36) voice.kind = 0;
        else if (note == 38 || note == 40 || note == 37 || note == 39) voice.kind = 1;
        else if (note == 42 || note == 44) voice.kind = 2;
        else if (note == 46) voice.kind = 3;
        else if (note == 49 || note == 57 || note == 52 || note == 55 || note == 51 || note == 59) voice.kind = 4;
        else voice.kind = 5;
        const double base = voice.kind == 0 ? 150 : voice.kind == 1 ? 190 : 90 + (note - 41) * 12.0;
        voice.step = base / rate_;
        voice.sweep = voice.kind == 0 ? 0.9996 : 0.99985;
        voice.held = false;
    } else {
        voice.kind = state.program / 8; // General MIDI instrument family.
        voice.step = 440.0 * std::pow(2.0, (note - 69) / 12.0) / rate_;
    }
    *slot = voice;
}

void Synth::noteOff(int channel, int note) {
    for (auto& voice : voices_) {
        if (voice.channel == channel && voice.note == note && voice.held) voice.held = false;
    }
}

float Synth::sample(Voice& voice) {
    const double t = voice.age / static_cast<double>(rate_);
    ++voice.age;
    float value = 0;
    float target = 0;
    if (voice.drum) {
        noise_ = noise_ * 1664525u + 1013904223u;
        const float noise = static_cast<std::int32_t>(noise_) / 2147483648.0f;
        switch (voice.kind) {
        case 0: value = static_cast<float>(std::sin(2 * Pi * voice.phase)); voice.step *= voice.sweep;
                target = static_cast<float>(std::exp(-t * 18)); break;
        case 1: value = 0.6f * noise + 0.4f * static_cast<float>(std::sin(2 * Pi * voice.phase));
                target = static_cast<float>(std::exp(-t * 22)); break;
        case 2: voice.low += (noise - voice.low) * 0.2f; value = noise - voice.low;
                target = static_cast<float>(std::exp(-t * 60)); break;
        case 3: voice.low += (noise - voice.low) * 0.2f; value = noise - voice.low;
                target = static_cast<float>(std::exp(-t * 12)); break;
        case 4: voice.low += (noise - voice.low) * 0.35f; value = noise - 0.5f * voice.low;
                target = 0.6f * static_cast<float>(std::exp(-t * 3.5)); break;
        default: value = static_cast<float>(std::sin(2 * Pi * voice.phase)); voice.step *= voice.sweep;
                 target = static_cast<float>(std::exp(-t * 14)); break;
        }
        voice.level = target;
        if (target < 0.001f) voice.channel = -1;
    } else {
        const double p = voice.phase;
        const float sine = static_cast<float>(std::sin(2 * Pi * p));
        const float saw = static_cast<float>(2 * (p - std::floor(p + 0.5)));
        const float triangle = static_cast<float>(4 * std::abs(p - std::floor(p + 0.75) + 0.25) - 1);
        const float square = p - std::floor(p) < 0.5 ? 0.6f : -0.6f;
        float attack = 0.004f, decay = 0.0f, sustain = 1.0f, release = 0.08f;
        switch (voice.kind) {
        case 0: // Piano
            value = 0.7f * sine + 0.3f * static_cast<float>(std::sin(4 * Pi * p));
            decay = 1.2f; sustain = 0.0f; release = 0.15f; break;
        case 1: case 3: // Chromatic percussion, guitar
            value = triangle; decay = 0.9f; sustain = 0.0f; release = 0.1f; break;
        case 2: case 8: case 9: // Organ, reed, pipe
            value = 0.7f * sine + 0.3f * static_cast<float>(std::sin(6 * Pi * p)); break;
        case 4: // Bass
            value = 0.8f * triangle + 0.2f * sine; decay = 2.0f; sustain = 0.6f; break;
        case 5: case 6: case 11: // Strings, ensemble, pads: a softened saw with a slow attack
            voice.low += (saw - voice.low) * 0.12f;
            value = 1.4f * voice.low; attack = 0.06f; release = 0.25f;
            if (voice.channel >= 0 && channels_[voice.channel].program == 55) { // Orchestra hit
                value = 0.6f * saw + 0.4f * square; attack = 0.002f; decay = 0.35f; sustain = 0.0f;
            }
            break;
        case 7: case 10: // Brass, synth lead
            voice.low += (saw - voice.low) * 0.3f;
            value = voice.low; attack = 0.02f; break;
        default:
            value = triangle; break;
        }
        if (voice.held) {
            const float attackLevel = std::min(1.0f, static_cast<float>(t) / attack);
            target = decay > 0 ? attackLevel * (sustain + (1 - sustain) * static_cast<float>(std::exp(-t / decay * 3)))
                               : attackLevel;
            voice.level = target;
            voice.release = 0;
        } else {
            voice.level *= static_cast<float>(std::exp(-1.0 / (release * rate_) * 5));
        }
        if (voice.level < 0.0005f && (!voice.held || t > 0.05)) voice.channel = -1;
    }
    voice.phase += voice.step;
    if (voice.phase > 1e6) voice.phase -= std::floor(voice.phase);
    return value * voice.level * voice.gain;
}

void Synth::render(std::int16_t* out, int frames) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (int i = 0; i < frames; ++i) {
        if (song_) {
            tickClock_ += 1.0 / samplesPerTick();
            while (tickClock_ >= 1.0) {
                tickClock_ -= 1.0;
                ++tick_;
            }
            while (tempoIndex_ < song_->tempos.size() && song_->tempos[tempoIndex_].first <= tick_) ++tempoIndex_;
            while (next_ < song_->events.size() && song_->events[next_].tick <= tick_) dispatch(song_->events[next_++]);
            if (next_ >= song_->events.size() && tick_ >= song_->length) {
                if (loops_ < 0 || --loops_ > 0) {
                    const auto song = song_;
                    reset();
                    song_ = song;
                } else {
                    song_.reset();
                }
            }
        }
        float mix = 0;
        for (auto& voice : voices_) {
            if (voice.channel < 0) continue;
            const auto& channel = channels_[voice.channel];
            mix += sample(voice) * channel.volume * channel.expression;
        }
        mix *= 0.22f * volume_;
        out[i] = static_cast<std::int16_t>(std::clamp(mix, -1.0f, 1.0f) * 32000);
    }
}

} // namespace midi
