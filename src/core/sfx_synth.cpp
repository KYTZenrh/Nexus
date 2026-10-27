#include "sfx_synth.h"

#include <cmath>
#include <cstdint>

namespace nexus {

namespace {

const float kTwoPi = 6.2831853f;

struct Buf {
    std::vector<float> d;
    explicit Buf(float sec) : d((size_t)(sec * kSfxRate), 0.0f) {}
};

enum class Env { Decay, Swell };

// Sine tone with optional pitch glide. Decay: instant attack, exponential fall.
// Swell: rises and falls over the duration (used for whooshes).
void tone(Buf& b, float start, float f0, float dur, float amp, float decay, float f1 = -1.0f,
          Env env = Env::Decay) {
    size_t i0 = (size_t)(start * kSfxRate);
    size_t n = (size_t)(dur * kSfxRate);
    float phase = 0.0f;
    for (size_t i = 0; i < n && i0 + i < b.d.size(); i++) {
        float t = (float)i / kSfxRate;
        float u = (float)i / (float)n;
        float f = f1 < 0 ? f0 : f0 + (f1 - f0) * u;
        phase += kTwoPi * f / kSfxRate;
        float e;
        if (env == Env::Swell)
            e = std::pow(std::sin(3.14159265f * u), 1.2f);
        else
            e = std::exp(-decay * t);
        float attack = std::fmin(1.0f, t / 0.003f);        // 3 ms: no click on start
        float release = std::fmin(1.0f, (dur - t) / 0.006f);  // 6 ms: no click on end
        b.d[i0 + i] += amp * e * attack * (release < 0 ? 0 : release) * std::sin(phase);
    }
}

// Struck-metal bell: a few inharmonic partials that die at different speeds.
void bell(Buf& b, float start, float f, float amp, float decay) {
    static const float ratio[4] = {1.0f, 2.01f, 2.76f, 4.07f};
    static const float gain[4] = {1.0f, 0.45f, 0.28f, 0.14f};
    static const float fall[4] = {1.0f, 1.5f, 2.2f, 3.2f};
    size_t i0 = (size_t)(start * kSfxRate);
    for (size_t i = 0; i0 + i < b.d.size(); i++) {
        float t = (float)i / kSfxRate;
        float attack = std::fmin(1.0f, t / 0.002f);
        float s = 0;
        for (int p = 0; p < 4; p++)
            s += gain[p] * std::exp(-decay * fall[p] * t) * std::sin(kTwoPi * f * ratio[p] * t);
        b.d[i0 + i] += amp * attack * s;
    }
}

// Filtered noise; the filter opens up over the duration for a rising "air" feel.
void whoosh(Buf& b, float start, float dur, float amp) {
    size_t i0 = (size_t)(start * kSfxRate);
    size_t n = (size_t)(dur * kSfxRate);
    uint32_t rng = 0x1234567u;
    float lp = 0;
    for (size_t i = 0; i < n && i0 + i < b.d.size(); i++) {
        rng = rng * 1664525u + 1013904223u;
        float w = ((rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        float u = (float)i / (float)n;
        float a = 0.02f + 0.45f * u * u;
        lp += a * (w - lp);
        float e = std::pow(std::sin(3.14159265f * u), 1.5f);
        b.d[i0 + i] += amp * e * lp * 3.0f;
    }
}

void finish(Buf& b) {
    for (auto& s : b.d) s = std::tanh(s * 1.2f) * 0.85f;  // gentle limiter
}

}  // namespace

std::vector<float> synthSfx(Sfx id) {
    switch (id) {
        case Sfx::Move: {
            Buf b(0.07f);
            tone(b, 0, 1500, 0.05f, 0.30f, 55, 1050);
            finish(b);
            return b.d;
        }
        case Sfx::Tab: {
            Buf b(0.18f);
            tone(b, 0, 520, 0.10f, 0.24f, 16, 780);
            tone(b, 0.035f, 780, 0.10f, 0.12f, 18, 1040);
            finish(b);
            return b.d;
        }
        case Sfx::Confirm: {
            Buf b(0.30f);
            tone(b, 0, 660, 0.10f, 0.26f, 14);
            tone(b, 0.07f, 990, 0.20f, 0.26f, 11);
            finish(b);
            return b.d;
        }
        case Sfx::Back: {
            Buf b(0.18f);
            tone(b, 0, 700, 0.12f, 0.26f, 15, 440);
            finish(b);
            return b.d;
        }
        case Sfx::Launch: {
            Buf b(1.5f);
            tone(b, 0, 170, 0.70f, 0.20f, 0, 1500, Env::Swell);
            tone(b, 0, 340, 0.70f, 0.09f, 0, 3000, Env::Swell);
            whoosh(b, 0.02f, 0.72f, 0.14f);
            bell(b, 0.58f, 1318.5f, 0.16f, 5.0f);
            finish(b);
            return b.d;
        }
        case Sfx::Error: {
            Buf b(0.34f);
            tone(b, 0, 210, 0.13f, 0.30f, 9);
            tone(b, 0, 420, 0.13f, 0.08f, 9);
            tone(b, 0.16f, 190, 0.15f, 0.30f, 9);
            tone(b, 0.16f, 380, 0.15f, 0.08f, 9);
            finish(b);
            return b.d;
        }
        case Sfx::Filter: {
            Buf b(0.12f);
            tone(b, 0, 1000, 0.06f, 0.24f, 45);
            tone(b, 0.022f, 1500, 0.07f, 0.11f, 40);
            finish(b);
            return b.d;
        }
        case Sfx::Setting: {
            Buf b(0.14f);
            tone(b, 0, 900, 0.09f, 0.28f, 24);
            tone(b, 0, 1800, 0.09f, 0.06f, 24);
            finish(b);
            return b.d;
        }
        case Sfx::Bronze: {
            Buf b(0.9f);
            bell(b, 0, 523.25f, 0.30f, 6.5f);
            bell(b, 0, 784.0f, 0.08f, 7.5f);
            finish(b);
            return b.d;
        }
        case Sfx::Silver: {
            Buf b(1.1f);
            bell(b, 0, 659.25f, 0.28f, 5.5f);
            bell(b, 0.09f, 987.77f, 0.28f, 5.5f);
            finish(b);
            return b.d;
        }
        case Sfx::Gold: {
            Buf b(1.4f);
            bell(b, 0, 523.25f, 0.26f, 4.8f);
            bell(b, 0.08f, 659.25f, 0.26f, 4.8f);
            bell(b, 0.16f, 783.99f, 0.26f, 4.5f);
            bell(b, 0.25f, 1046.5f, 0.22f, 3.8f);
            finish(b);
            return b.d;
        }
        case Sfx::Platinum: {
            Buf b(2.4f);
            static const float notes[6] = {523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f, 1568.0f};
            for (int i = 0; i < 6; i++) bell(b, i * 0.075f, notes[i], 0.20f, 3.6f);
            float t = 0.55f;
            bell(b, t, 1046.5f, 0.17f, 2.2f);
            bell(b, t, 1318.5f, 0.15f, 2.2f);
            bell(b, t, 1568.0f, 0.15f, 2.2f);
            for (int i = 0; i < 4; i++) tone(b, 0.62f + i * 0.09f, 3136.0f + i * 260.0f, 0.12f, 0.05f, 22);
            finish(b);
            return b.d;
        }
        case Sfx::Locked: {
            Buf b(0.12f);
            tone(b, 0, 170, 0.08f, 0.32f, 38, 130);
            finish(b);
            return b.d;
        }
        default: return {};
    }
}

}  // namespace nexus
