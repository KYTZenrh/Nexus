#include "sample_data.h"

#include <cstdio>

namespace nexus {

std::vector<Game> sampleGames() {
    struct S { const char* t; uint32_t a, b; };
    static const S s[] = {
        {"Neon Circuit", 0x1d6fa5, 0x0a1f3a}, {"Iron Tide", 0x7a3b2e, 0x241410},
        {"Hollow Peaks™ Remastered", 0x3f7a62, 0x0f2a24},
        {"Sunken Atlas: The Complete Deep Waters Collection", 0x2c5b8a, 0x0c1a30},
        {"Last Signal", 0x8a6a2c, 0x2b1f0c}, {"Ember Court", 0x9a3350, 0x2c0f1c},
        {"Velvet Drift", 0x5a4a9a, 0x1b1536}, {"Orbit Keep", 0x4a7f8f, 0x112630},
    };
    std::vector<Game> v;
    int i = 1;
    for (auto& x : s) {
        Game g;
        char id[16];
        std::snprintf(id, sizeof id, "SAMP%05d", i++);
        g.titleId = id;
        g.title = x.t;
        g.sample = true;
        g.colorA = x.a;
        g.colorB = x.b;
        v.push_back(g);
    }
    return v;
}

TrophyList sampleTrophies(const Game& g) {
    TrophyList t;
    if (!g.sample) {
        t.reason = "No trophy data source yet.";
        return t;
    }
    t.available = true;
    t.sample = true;
    if (g.title == "Orbit Keep") return t;  // one sample app without trophies (shows 0 of 0)
    // Deterministic per game so the preview is stable.
    unsigned seed = 0;
    for (char c : g.titleId) seed = seed * 31 + (unsigned char)c;
    struct R { const char* n; const char* h; Grade g; };
    static const R pool[] = {
        {"First Steps", "Complete the opening chapter.", Grade::Bronze},
        {"Quick Study", "Finish the tutorial without skipping any prompts.", Grade::Bronze},
        {"Pack Rat", "Collect 50 items across the world.", Grade::Bronze},
        {"On the Map", "Discover every region in the first act.", Grade::Bronze},
        {"Close Call", "Win an encounter with less than 5% health remaining.", Grade::Bronze},
        {"Well Equipped", "Fully upgrade any one piece of equipment.", Grade::Silver},
        {"No Stone Unturned", "Find all hidden locations in the second act.", Grade::Silver},
        {"Perfect Timing", "Complete a timed challenge with a gold rating.", Grade::Silver},
        {"Old Rivals", "Defeat the rival champion on the hardest difficulty.", Grade::Gold},
        {"Against the Odds", "Finish the story without using any checkpoints.", Grade::Gold},
        {"The Whole Story", "Earn every other trophy in the game.", Grade::Platinum},
    };
    for (size_t i = 0; i < sizeof(pool) / sizeof(pool[0]); i++) {
        Trophy tr;
        tr.name = pool[i].n;
        tr.how = pool[i].h;
        tr.grade = pool[i].g;
        tr.earned = ((seed >> (i % 16)) & 1) && pool[i].g != Grade::Platinum;
        if (g.titleId == "SAMP00006") tr.earned = true;  // one fully completed sample game
        t.items.push_back(tr);
    }
    return t;
}

}  // namespace nexus
