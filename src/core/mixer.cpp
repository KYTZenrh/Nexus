#include "mixer.h"

#include "music_synth.h"
#include "sfx_synth.h"

namespace nexus {

void Mixer::loadSfx() {
    for (int i = 0; i < (int)Sfx::Count; i++) bank_[i] = synthSfx((Sfx)i);
}

void Mixer::setMusic(std::vector<float> m) {
    std::lock_guard<std::mutex> g(lock_);
    music_ = std::move(m);
    musicPos_ = 0;
    musicReady_.store(!music_.empty(), std::memory_order_release);
}

void Mixer::play(Sfx s, float volume, float pitch) {
    if (volume <= 0.0f || (int)s < 0 || (int)s >= (int)Sfx::Count || bank_[(int)s].empty()) return;
    std::lock_guard<std::mutex> g(lock_);
    Voice* slot = nullptr;
    Voice* oldest = &voices_[0];
    for (auto& v : voices_) {
        if (!v.buf) { slot = &v; break; }
        if (v.pos > oldest->pos) oldest = &v;
    }
    if (!slot) slot = oldest;  // all busy: steal the voice that has played the longest
    slot->buf = &bank_[(int)s];
    slot->pos = 0;
    slot->vol = volume;
    slot->pitch = pitch;
}

void Mixer::mix(float* out, int frames) {
    for (int i = 0; i < frames * 2; i++) out[i] = 0.0f;
    std::lock_guard<std::mutex> g(lock_);
    for (auto& v : voices_) {
        if (!v.buf) continue;
        const std::vector<float>& b = *v.buf;
        for (int i = 0; i < frames; i++) {
            size_t p = (size_t)v.pos;
            if (p + 1 >= b.size()) {
                v.buf = nullptr;
                break;
            }
            float f = (float)(v.pos - (double)p);
            float s = (b[p] + (b[p + 1] - b[p]) * f) * v.vol;
            out[i * 2] += s;
            out[i * 2 + 1] += s;
            v.pos += v.pitch;
        }
    }

    // Background music: a looping buffer at kMusicRate, with the volume eased so changes
    // (and the fade-out during a launch) never click.
    if (musicReady_.load(std::memory_order_acquire) && !music_.empty()) {
        const float target = musicTarget_.load();
        const size_t n = music_.size();
        const double step = (double)kMusicRate / (double)kSfxRate;
        for (int i = 0; i < frames; i++) {
            musicCur_ += (target - musicCur_) * 0.0004f;
            if (musicCur_ > 0.0003f) {
                size_t p = (size_t)musicPos_;
                float f = (float)(musicPos_ - (double)p);
                float s = (music_[p] + (music_[(p + 1) % n] - music_[p]) * f) * musicCur_;
                out[i * 2] += s;
                out[i * 2 + 1] += s;
            }
            musicPos_ += step;
            if (musicPos_ >= (double)n) musicPos_ -= (double)n;
        }
    }
}

}  // namespace nexus
