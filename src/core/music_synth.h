#pragma once
#include <vector>

namespace nexus {

constexpr int kMusicRate = 24000;

// A seamless ambient loop (mono, kMusicRate): slow pad chords with sparse, soft bell notes
// and a little echo. Generated in code, so there is no audio file to ship or license.
std::vector<float> synthMusic();

}  // namespace nexus
