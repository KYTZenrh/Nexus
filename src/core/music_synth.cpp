#include "music_synth.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nexus {

namespace {

const float kTwoPi = 6.2831853f;
const float kLoopSec = 48.0f;

float smoothstep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Every note is written modulo the buffer length, so a note's release tail wraps around to
// the start of the loop and the seam between the end and the beginning is inaudible.
void padNote(std::vector<float>& b, float start, float f, float dur, float amp, float phase) {
    const size_t n = b.size();
    const float attack = 3.6f, release = 5.5f;
    size_t i0 = (size_t)(start * kMusicRate);
    size_t len = (size_t)(dur * kMusicRate);
    static const float detune[3] = {0.9985f, 1.0f, 1.0017f};
    for (size_t i = 0; i < len; i++) {
        float t = (float)i / kMusicRate;
        float env = smoothstep(t / attack) * smoothstep((dur - t) / release);
        float trem = 1.0f + 0.12f * std::sin(kTwoPi * 0.11f * t + phase);
        float s = 0;
        for (float d : detune) s += 0.33f * std::sin(kTwoPi * f * d * t + phase);
        s += 0.07f * std::sin(kTwoPi * f * 2.0f * t + phase);  // a touch of octave for air
        b[(i0 + i) % n] += amp * env * trem * s;
    }
}

void softBell(std::vector<float>& b, float start, float f, float amp) {
    const size_t n = b.size();
    static const float ratio[3] = {1.0f, 2.76f, 5.4f};
    static const float gain[3] = {1.0f, 0.30f, 0.10f};
    static const float fall[3] = {1.4f, 2.4f, 4.0f};
    size_t i0 = (size_t)(start * kMusicRate);
    size_t len = (size_t)(5.0f * kMusicRate);
    for (size_t i = 0; i < len; i++) {
        float t = (float)i / kMusicRate;
        float attack = std::min(1.0f, t / 0.01f);
        float s = 0;
        for (int p = 0; p < 3; p++) s += gain[p] * std::exp(-fall[p] * t) * std::sin(kTwoPi * f * ratio[p] * t);
        b[(i0 + i) % n] += amp * attack * s;
    }
}

}  // namespace

std::vector<float> synthMusic() {
    std::vector<float> b((size_t)(kLoopSec * kMusicRate), 0.0f);

    // Am9 - Fmaj7 - Cmaj7 - G6/9, twelve seconds each.
    struct Chord {
        float notes[6];
    };
    static const Chord chords[4] = {
        {{110.0f, 164.81f, 220.0f, 261.63f, 329.63f, 493.88f}},
        {{87.31f, 130.81f, 174.61f, 220.0f, 261.63f, 329.63f}},
        {{130.81f, 196.0f, 261.63f, 329.63f, 392.0f, 493.88f}},
        {{98.0f, 146.83f, 196.0f, 246.94f, 329.63f, 440.0f}},
    };
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 6; k++) {
            float amp = k == 0 ? 0.55f : 0.36f;  // the bass note carries a little more weight
            padNote(b, c * 12.0f + k * 0.35f, chords[c].notes[k], 12.0f + 5.5f, amp, 1.3f * k + c);
        }

    // Sparse bell notes from an A minor pentatonic scale, placed by a fixed pseudo-random sequence.
    static const float scale[5] = {440.0f, 523.25f, 587.33f, 659.25f, 783.99f};
    uint32_t rng = 20261025u;
    for (int i = 0; i < 12; i++) {
        rng = rng * 1664525u + 1013904223u;
        float t = i * 4.0f + 0.8f + ((rng >> 12) & 0xFF) / 255.0f * 2.2f;
        rng = rng * 1664525u + 1013904223u;
        float f = scale[(rng >> 10) % 5] * (((rng >> 20) & 3) == 0 ? 2.0f : 1.0f);
        softBell(b, t, f, 0.05f);
    }

    // A little echo (two circular delays) for space.
    const size_t n = b.size();
    const size_t d1 = (size_t)(0.43f * kMusicRate), d2 = (size_t)(0.71f * kMusicRate);
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < n; i++) b[i] += 0.30f * b[(i + n - d1) % n] + 0.20f * b[(i + n - d2) % n];

    float peak = 0;
    for (float v : b) peak = std::max(peak, std::fabs(v));
    float k = peak > 0 ? 0.5f / peak : 1.0f;
    for (float& v : b) v *= k;
    return b;
}

}  // namespace nexus
