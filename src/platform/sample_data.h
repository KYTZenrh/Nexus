#pragma once
#include "platform.h"

namespace nexus {

// Clearly labelled sample entries, only ever shown in preview/test builds (never in a release
// build). Shared by every target so they all show identical content.
std::vector<Game> sampleGames();
TrophyList sampleTrophies(const Game& g);

}  // namespace nexus
