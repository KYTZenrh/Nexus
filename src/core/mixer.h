#pragma once
// Software mixer shared by every target: sound effect voices plus the looping background
// music, mixed to interleaved stereo float at kSfxRate. The platform only has to hand the
// output to its audio device.
#include <atomic>
#include <mutex>
#include <vector>

#include "../platform/platform.h"

namespace nexus {

class Mixer {
public:
    void loadSfx();                       // synthesizes every sound effect (fast)
    void setMusic(std::vector<float> m);  // generated loop at kMusicRate; safe from any thread
    void play(Sfx s, float volume, float pitch);
    void setMusicVolume(float v) { musicTarget_.store(v); }
    void mix(float* out, int frames);     // called from the audio thread

private:
    struct Voice {
        const std::vector<float>* buf = nullptr;
        double pos = 0;
        float vol = 0;
        float pitch = 1;
    };
    static constexpr int kVoices = 16;
    std::mutex lock_;
    Voice voices_[kVoices];
    std::vector<float> bank_[(int)Sfx::Count];
    std::vector<float> music_;
    std::atomic<bool> musicReady_{false};
    std::atomic<float> musicTarget_{0.0f};
    float musicCur_ = 0.0f;
    double musicPos_ = 0.0;
};

}  // namespace nexus
