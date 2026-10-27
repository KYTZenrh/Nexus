#pragma once
#include <vector>

#include "../platform/platform.h"

namespace nexus {

constexpr int kSfxRate = 48000;

// Mono samples in [-1, 1] at kSfxRate for one sound effect.
std::vector<float> synthSfx(Sfx id);

}  // namespace nexus
