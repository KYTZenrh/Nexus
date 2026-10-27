#pragma once
#include "gfx.h"

namespace nexus {

// The app's identity artwork, drawn with the same renderer as the UI so the mark, colours and
// type always match. Exported as PNG by `nexus --make-icon <folder>`.
void drawAppIcon(Gfx& g, float size);                 // icon0.png, square (512 x 512)
void drawBackdrop(Gfx& g, float w, float h);          // pic1.png, 1920 x 1080 home-screen art

}  // namespace nexus
