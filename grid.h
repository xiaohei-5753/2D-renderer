#ifndef GRID_H
#define GRID_H

#include <cstdint>
#include <complex>
#include <queue>

constexpr int W_MAX = 1024;

struct CoordI {
    int x, y;
    CoordI(int x = 0, int y = 0) : x(x), y(y) {}
    CoordI operator+(const CoordI& r) const { return {x + r.x, y + r.y}; }
    CoordI operator-(const CoordI& r) const { return {x - r.x, y - r.y}; }
    bool operator==(const CoordI& r) const { return x == r.x && y == r.y; }
    bool operator!=(const CoordI& r) const { return x != r.x || y != r.y; }
};

using Complex = std::complex<double>;

// Global grid state
extern int W, H;
extern uint8_t cv[W_MAX][W_MAX];       // cell type: 0=empty, 1=wire, 2=signal
extern Complex fi[W_MAX][W_MAX];       // complex field value
extern uint16_t cv_s[W_MAX][W_MAX];    // visited marker for BFS

// Spark queues
extern std::queue<CoordI> sprk;
extern std::queue<Complex> sprk_f;

// Clipboard
extern uint8_t clip_cv[W_MAX][W_MAX];
extern Complex clip_fi[W_MAX][W_MAX];
extern int clipW, clipH;
extern CoordI clip_c;
extern int flag_copy;
extern int flag_copyx;
extern int flag_paste;
extern int flag_pause;

void clearGrid();
void saveGrid(const std::string& fn);
void loadGrid(const std::string& fn);

#endif // GRID_H
