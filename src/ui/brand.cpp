#include "brand.h"

namespace nexus {

static const Col kAccent = hex(0x5ab8ff);

// The Nexus mark: a diamond whose left half is dimmer than its right, with a small node cut
// out of the middle.
static void diamond(Gfx& g, float cx, float cy, float r, Col base, float leftDim, Col cut) {
    g.triangle(cx, cy - r, cx + r, cy, cx, cy + r, base);
    g.triangle(cx, cy - r, cx - r, cy, cx, cy + r, withA(base, leftDim));
    if (cut.a > 0) g.triangle(cx, cy - r * 0.36f, cx + r * 0.36f, cy, cx, cy + r * 0.36f, cut),
        g.triangle(cx, cy - r * 0.36f, cx - r * 0.36f, cy, cx, cy + r * 0.36f, cut);
}

void drawAppIcon(Gfx& g, float s) {
    g.alpha = 1;
    g.rect(0, 0, s, s, hex(0x142036), hex(0x070a11));
    g.radial(s * 0.28f, s * 0.18f, s * 0.95f, withA(kAccent, 0.26f));
    g.radial(s * 0.92f, s * 1.0f, s * 0.7f, withA(kAccent, 0.10f));

    float cx = s * 0.5f, cy = s * 0.42f;
    g.ring(cx, cy, s * 0.30f, s * 0.008f, withA(kAccent, 0.55f));
    g.ring(cx, cy, s * 0.37f, s * 0.004f, withA(kAccent, 0.25f));
    g.radial(cx, cy, s * 0.26f, withA(kAccent, 0.28f));
    diamond(g, cx, cy, s * 0.19f, kAccent, 0.55f, hex(0x0c1424));

    g.textSpaced("NEXUS", Bold, (int)(s * 0.145f), cx, s * 0.69f, hex(0xeef1f6), s * 0.028f, Align::Center);
    g.textSpaced("KYT STATION DASH", Medium, (int)(s * 0.04f), cx, s * 0.875f, kAccent, s * 0.0125f,
                 Align::Center);
}

void drawBackdrop(Gfx& g, float w, float h) {
    g.alpha = 1;
    g.rect(0, 0, w, h, hex(0x0c1220), hex(0x05070b));
    g.radial(w * 0.78f, h * 0.30f, h * 1.1f, withA(kAccent, 0.16f));
    g.radial(w * 0.10f, h * 1.05f, h * 0.9f, withA(kAccent, 0.08f));

    float cx = w * 0.74f, cy = h * 0.5f;
    g.ring(cx, cy, h * 0.40f, 4.0f, withA(kAccent, 0.18f));
    g.ring(cx, cy, h * 0.50f, 2.0f, withA(kAccent, 0.10f));
    diamond(g, cx, cy, h * 0.27f, withA(kAccent, 0.30f), 0.55f, hex(0x090e18));

    g.textSpaced("NEXUS", Bold, (int)(h * 0.17f), w * 0.07f, h * 0.60f, hex(0xeef1f6), h * 0.032f);
    g.textSpaced("KYT STATION DASH", Medium, (int)(h * 0.036f), w * 0.075f, h * 0.80f, kAccent,
                 h * 0.011f);
}

}  // namespace nexus
