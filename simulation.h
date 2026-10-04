#ifndef SIMULATION_H
#define SIMULATION_H

#include <cstdint>
#include <vector>

struct ActivePat {
    int sx, sy;  // fi source
    int dx, dy;  // spike destination
    uint8_t typ; // 0=ln, 1=exp
    uint8_t cond;// for exp: condition result (0=exp(fi), 1=Complex(-1,0))
};

extern std::vector<ActivePat> g_pats;

// Colour of one grid cell (matches the first four floats of renderer.h's Pixel).
struct CellRGBA { float r = 0, g = 0, b = 0, a = 0; };

void parsing();
void conduct();
void rebuildPatterns();
// Incremental variant: only anchors near the edited rectangle are re-scanned.
void rebuildPatternsAround(int x0, int y0, int x1, int y1);
// Fills out[0..W*H) with cell colours, recomputing only cells whose value changed.
// `changed` receives the indices (y*W+x) that were written this call.
void syncGridColors(CellRGBA* out, std::vector<int>& changed, int W, int H, float wireAlpha);

void reco_ln(int x, int y, int dir);
void reco_exp(int x, int y, int dir);

void line_cvs(CoordI a, CoordI b, uint8_t color);
void copy_cv(CoordI p1, CoordI p2);
void paste_cv(CoordI center);

uint32_t magic_c2col(const Complex& c);

#endif // SIMULATION_H
