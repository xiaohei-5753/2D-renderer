#include "grid.h"
#include "simulation.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <cmath>
#include <cstring>
#include <unordered_map>

// === Global state definitions ===

int W = 128, H = 128;
uint8_t cv[W_MAX][W_MAX] = {};
Complex fi[W_MAX][W_MAX] = {};
uint16_t cv_s[W_MAX][W_MAX] = {};
std::queue<CoordI> sprk;
std::queue<Complex> sprk_f;

uint8_t clip_cv[W_MAX][W_MAX] = {};
Complex clip_fi[W_MAX][W_MAX] = {};
int clipW = 0, clipH = 0;
CoordI clip_c;
int flag_copy = 0;
int flag_copyx = 0;
int flag_paste = 0;
int flag_pause = 0;

// Active pattern list
std::vector<ActivePat> g_pats;

void parsing(){
    for(auto& p:g_pats){
        Complex v=fi[p.sx][p.sy];
        if(p.typ==1){ // exp
            if(p.cond) v=Complex(-1,0);
            else v=std::exp(v);
        }else v=std::log(v); // ln
        sprk.push(CoordI(p.dx,p.dy));sprk_f.push(v);
    }
}

// === Simulation: BFS propagation ===

void conduct() {
    memset(fi, 0, sizeof(Complex) * W_MAX * W_MAX);
    memset(cv_s, 0, sizeof(cv_s));
    int cc = 0;
    while (sprk.size()) {
        std::queue<CoordI> q;
        q.push(sprk.front());
        sprk.pop();
        cc++;
        Complex val = sprk_f.front();
        sprk_f.pop();
        while (q.size()) {
            CoordI p = q.front();
            q.pop();
            // Right (+x)
            if (cv[p.x + 1][p.y] && cv[p.x + 2][p.y] &&
                cv[p.x + 1][p.y + 1] && cv[p.x + 1][p.y - 1] &&
                !cv[p.x][p.y + 1] && !cv[p.x][p.y - 1] &&
                !cv[p.x + 2][p.y + 1] && !cv[p.x + 2][p.y - 1]) {
                if (cv_s[p.x + 2][p.y] != cc) {
                    q.push(CoordI(p.x + 2, p.y));
                    fi[p.x + 2][p.y] += val;
                    fi[p.x + 1][p.y] = fi[p.x + 2][p.y];
                    cv_s[p.x + 2][p.y] = cc;
                }
            } else {
                if (cv[p.x + 1][p.y] && cv_s[p.x + 1][p.y] != cc) {
                    q.push(CoordI(p.x + 1, p.y));
                    fi[p.x + 1][p.y] += val;
                    cv_s[p.x + 1][p.y] = cc;
                }
            }
            // Left (-x)
            if (cv[p.x - 1][p.y] && cv[p.x - 2][p.y] &&
                cv[p.x - 1][p.y + 1] && cv[p.x - 1][p.y - 1] &&
                !cv[p.x][p.y + 1] && !cv[p.x][p.y - 1] &&
                !cv[p.x - 2][p.y + 1] && !cv[p.x - 2][p.y - 1]) {
                if (cv_s[p.x - 2][p.y] != cc) {
                    q.push(CoordI(p.x - 2, p.y));
                    fi[p.x - 2][p.y] += val;
                    fi[p.x - 1][p.y] = fi[p.x - 2][p.y];
                    cv_s[p.x - 2][p.y] = cc;
                }
            } else {
                if (cv[p.x - 1][p.y] && cv_s[p.x - 1][p.y] != cc) {
                    q.push(CoordI(p.x - 1, p.y));
                    fi[p.x - 1][p.y] += val;
                    cv_s[p.x - 1][p.y] = cc;
                }
            }
            // Down (+y)
            if (cv[p.x][p.y + 1] && cv[p.x][p.y + 2] &&
                cv[p.x + 1][p.y + 1] && cv[p.x - 1][p.y + 1] &&
                !cv[p.x + 1][p.y] && !cv[p.x - 1][p.y] &&
                !cv[p.x + 1][p.y + 2] && !cv[p.x - 1][p.y + 2]) {
                if (cv_s[p.x][p.y + 2] != cc) {
                    q.push(CoordI(p.x, p.y + 2));
                    fi[p.x][p.y + 2] += val;
                    fi[p.x][p.y + 1] = fi[p.x][p.y + 2];
                    cv_s[p.x][p.y + 2] = cc;
                }
            } else {
                if (cv[p.x][p.y + 1] && cv_s[p.x][p.y + 1] != cc) {
                    q.push(CoordI(p.x, p.y + 1));
                    fi[p.x][p.y + 1] += val;
                    cv_s[p.x][p.y + 1] = cc;
                }
            }
            // Up (-y)
            if (cv[p.x][p.y - 1] && cv[p.x][p.y - 2] &&
                cv[p.x + 1][p.y - 1] && cv[p.x - 1][p.y - 1] &&
                !cv[p.x + 1][p.y] && !cv[p.x - 1][p.y] &&
                !cv[p.x + 1][p.y - 2] && !cv[p.x - 1][p.y - 2]) {
                if (cv_s[p.x][p.y - 2] != cc) {
                    q.push(CoordI(p.x, p.y - 2));
                    fi[p.x][p.y - 2] += val;
                    fi[p.x][p.y - 1] = fi[p.x][p.y - 2];
                    cv_s[p.x][p.y - 2] = cc;
                }
            } else {
                if (cv[p.x][p.y - 1] && cv_s[p.x][p.y - 1] != cc) {
                    q.push(CoordI(p.x, p.y - 1));
                    fi[p.x][p.y - 1] += val;
                    cv_s[p.x][p.y - 1] = cc;
                }
            }
        }
    }
}

// === Pattern recognition: ln (8 directions) ===

void reco_ln(int x, int y, int dir) {
    switch (dir) {
    case 0:
        if (cv[x+0][y+0]) break;
        if (!cv[x+1][y+0]) break; if (cv[x+2][y+0]) break;
        if (!cv[x+0][y+1]) break; if (!cv[x+1][y+1]) break; if (!cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break;
        if (cv[x+0][y+2]) break; if (!cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (cv[x+3][y+2]) break;
        if (cv[x+0][y+3]) break; if (!cv[x+1][y+3]) break; if (!cv[x+2][y+3]) break; if (!cv[x+3][y+3]) break; if (cv[x+4][y+3]) break;
        if (cv[x+1][y+4]) break; if (cv[x+2][y+4]) break; if (cv[x+3][y+4]) break;
        sprk_f.push(std::log(fi[x+2][y+1])); sprk.push(CoordI(x+2, y+3));
        break;
    case 1:
        if (cv[x+3][y+0]) break;
        if (!cv[x+1][y+1]) break; if (cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break; if (cv[x+4][y+1]) break;
        if (cv[x+0][y+2]) break; if (!cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break; if (cv[x+4][y+2]) break;
        if (!cv[x+0][y+3]) break; if (!cv[x+1][y+3]) break; if (!cv[x+2][y+3]) break; if (!cv[x+3][y+3]) break; if (cv[x+4][y+3]) break;
        if (cv[x+0][y+4]) break; if (!cv[x+1][y+4]) break; if (cv[x+2][y+4]) break; if (cv[x+3][y+4]) break;
        sprk_f.push(std::log(fi[x+1][y+2])); sprk.push(CoordI(x+3, y+2));
        break;
    case 2:
        if (cv[x+1][y+0]) break; if (cv[x+2][y+0]) break; if (cv[x+3][y+0]) break;
        if (cv[x+0][y+1]) break; if (!cv[x+1][y+1]) break; if (!cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break; if (cv[x+4][y+1]) break;
        if (cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break; if (cv[x+4][y+2]) break;
        if (!cv[x+1][y+3]) break; if (!cv[x+2][y+3]) break; if (!cv[x+3][y+3]) break; if (!cv[x+4][y+3]) break;
        if (cv[x+2][y+4]) break; if (!cv[x+3][y+4]) break; if (cv[x+4][y+4]) break;
        sprk_f.push(std::log(fi[x+2][y+3])); sprk.push(CoordI(x+2, y+1));
        break;
    case 3:
        if (cv[x+1][y+0]) break; if (cv[x+2][y+0]) break; if (!cv[x+3][y+0]) break; if (cv[x+4][y+0]) break;
        if (cv[x+0][y+1]) break; if (!cv[x+1][y+1]) break; if (!cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break; if (!cv[x+4][y+1]) break;
        if (cv[x+0][y+2]) break; if (!cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break; if (cv[x+4][y+2]) break;
        if (cv[x+0][y+3]) break; if (!cv[x+1][y+3]) break; if (cv[x+2][y+3]) break; if (!cv[x+3][y+3]) break;
        if (cv[x+1][y+4]) break;
        sprk_f.push(std::log(fi[x+3][y+2])); sprk.push(CoordI(x+1, y+2));
        break;
    case 4:
        if (cv[x+2][y+0]) break; if (!cv[x+3][y+0]) break; if (cv[x+4][y+0]) break;
        if (!cv[x+1][y+1]) break; if (!cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break; if (!cv[x+4][y+1]) break;
        if (cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break; if (cv[x+4][y+2]) break;
        if (cv[x+0][y+3]) break; if (!cv[x+1][y+3]) break; if (!cv[x+2][y+3]) break; if (!cv[x+3][y+3]) break; if (cv[x+4][y+3]) break;
        if (cv[x+1][y+4]) break; if (cv[x+2][y+4]) break; if (cv[x+3][y+4]) break;
        sprk_f.push(std::log(fi[x+2][y+1])); sprk.push(CoordI(x+2, y+3));
        break;
    case 5:
        if (cv[x+1][y+0]) break;
        if (cv[x+0][y+1]) break; if (!cv[x+1][y+1]) break; if (cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break;
        if (cv[x+0][y+2]) break; if (!cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break; if (cv[x+4][y+2]) break;
        if (cv[x+0][y+3]) break; if (!cv[x+1][y+3]) break; if (!cv[x+2][y+3]) break; if (!cv[x+3][y+3]) break; if (!cv[x+4][y+3]) break;
        if (cv[x+1][y+4]) break; if (cv[x+2][y+4]) break; if (!cv[x+3][y+4]) break; if (cv[x+4][y+4]) break;
        sprk_f.push(std::log(fi[x+3][y+2])); sprk.push(CoordI(x+1, y+2));
        break;
    case 6:
        if (cv[x+1][y+0]) break; if (cv[x+2][y+0]) break; if (cv[x+3][y+0]) break;
        if (cv[x+0][y+1]) break; if (!cv[x+1][y+1]) break; if (!cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break; if (cv[x+4][y+1]) break;
        if (cv[x+0][y+2]) break; if (!cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (cv[x+3][y+2]) break;
        if (!cv[x+0][y+3]) break; if (!cv[x+1][y+3]) break; if (!cv[x+2][y+3]) break; if (!cv[x+3][y+3]) break;
        if (cv[x+0][y+4]) break; if (!cv[x+1][y+4]) break; if (cv[x+2][y+4]) break;
        sprk_f.push(std::log(fi[x+2][y+3])); sprk.push(CoordI(x+2, y+1));
        break;
    case 7:
        if (cv[x+0][y+0]) break; if (!cv[x+1][y+0]) break; if (cv[x+2][y+0]) break; if (cv[x+3][y+0]) break;
        if (!cv[x+0][y+1]) break; if (!cv[x+1][y+1]) break; if (!cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break; if (cv[x+4][y+1]) break;
        if (cv[x+0][y+2]) break; if (!cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break; if (cv[x+4][y+2]) break;
        if (!cv[x+1][y+3]) break; if (cv[x+2][y+3]) break; if (!cv[x+3][y+3]) break; if (cv[x+4][y+3]) break;
        if (cv[x+3][y+4]) break;
        sprk_f.push(std::log(fi[x+1][y+2])); sprk.push(CoordI(x+3, y+2));
        break;
    }
}

// === Pattern recognition: exp (4 directions) ===

void reco_exp(int x, int y, int dir) {
    switch (dir) {
    case 0:
        if (cv[x+1][y+0]) break; if (!cv[x+2][y+0]) break; if (cv[x+3][y+0]) break;
        if (cv[x+0][y+1]) break; if (!cv[x+1][y+1]) break; if (!cv[x+2][y+1]) break; if (!cv[x+3][y+1]) break; if (cv[x+4][y+1]) break;
        if (cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (cv[x+3][y+2]) break;
        if (!cv[x+2][y+3]) break;
        if (!(cv[x+2][y+4] || cv[x+1][y+3] || cv[x+3][y+3])) {
            sprk_f.push(Complex(-1, 0));
        } else {
            sprk_f.push(std::exp(fi[x+2][y+3]));
        }
        sprk.push(CoordI(x+2, y+1));
        break;
    case 1:
        if (cv[x+1][y+0]) break;
        if (cv[x+0][y+1]) break; if (!cv[x+1][y+1]) break; if (cv[x+2][y+1]) break;
        if (!cv[x+0][y+2]) break; if (!cv[x+1][y+2]) break; if (cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break;
        if (cv[x+0][y+3]) break; if (!cv[x+1][y+3]) break; if (cv[x+2][y+3]) break;
        if (cv[x+1][y+4]) break;
        if (!(cv[x+4][y+2] || cv[x+3][y+1] || cv[x+3][y+3])) {
            sprk_f.push(Complex(-1, 0));
        } else {
            sprk_f.push(std::exp(fi[x+3][y+2]));
        }
        sprk.push(CoordI(x+1, y+2));
        break;
    case 2:
        if (!cv[x+2][y+0]) break;
        if (cv[x+1][y+1]) break; if (cv[x+2][y+1]) break; if (cv[x+3][y+1]) break;
        if (cv[x+0][y+2]) break; if (!cv[x+1][y+2]) break; if (!cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break; if (cv[x+4][y+2]) break;
        if (cv[x+1][y+3]) break; if (!cv[x+2][y+3]) break; if (cv[x+3][y+3]) break;
        if (!(cv[x+2][y-1] || cv[x+1][y+0] || cv[x+3][y+0])) {
            sprk_f.push(Complex(-1, 0));
        } else {
            sprk_f.push(std::exp(fi[x+2][y+0]));
        }
        sprk.push(CoordI(x+2, y+2));
        break;
    case 3:
        if (cv[x+2][y+0]) break;
        if (cv[x+1][y+1]) break; if (!cv[x+2][y+1]) break; if (cv[x+3][y+1]) break;
        if (!cv[x+0][y+2]) break; if (cv[x+1][y+2]) break; if (!cv[x+2][y+2]) break; if (!cv[x+3][y+2]) break;
        if (cv[x+1][y+3]) break; if (!cv[x+2][y+3]) break; if (cv[x+3][y+3]) break;
        if (cv[x+2][y+4]) break;
        if (!(cv[x-1][y+2] || cv[x+0][y+1] || cv[x+0][y+3])) {
            sprk_f.push(Complex(-1, 0));
        } else {
            sprk_f.push(std::exp(fi[x+0][y+2]));
        }
        sprk.push(CoordI(x+2, y+2));
        break;
    }
}

// === Grid editing: Bresenham line ===

void line_cvs(CoordI a, CoordI b, uint8_t color) {
    CoordI d = a - b;
    int sym = 0;
    if (d.y < 0) { d.y = -d.y; sym |= 1; }
    if (d.x < 0) { d.x = -d.x; sym |= 2; }
    if (d.y > d.x) { std::swap(d.y, d.x); sym |= 4; }
    int err = 0;
    int y = 0;
    for (int x = 0; x <= d.x; x++) {
        CoordI p = {x, y};
        if (sym & 4) std::swap(p.x, p.y);
        if (sym & 2) p.x = -p.x;
        if (sym & 1) p.y = -p.y;
        p = p + b;
        if (p.x >= 0 && p.x < W && p.y >= 0 && p.y < H) {
            cv[p.x][p.y] = color;
            fi[p.x][p.y] = Complex(0, 0);
        }
        err += d.y;
        if (err >= d.x) {
            y++;
            err -= d.x;
        }
    }
}

// Per-anchor pattern slots. The bit order is exactly the order the original full scan
// emitted patterns (0..7 = ln directions, 8..11 = exp directions), and anchors are
// emitted sorted by (y,x), so an incremental update yields the same g_pats sequence as
// a full rebuild. NOTE: mask must hold 12 bits (8 ln + 4 exp).
struct AnchorEntry{uint16_t mask;uint8_t cond[4];};
static std::unordered_map<uint32_t,AnchorEntry> g_anchorPats;
static const int ANCHOR_B[8][4]={{2,1,2,3},{1,2,3,2},{2,3,2,1},{3,2,1,2},{2,1,2,3},{3,2,1,2},{2,3,2,1},{1,2,3,2}};
static const int ANCHOR_BE[4][4]={{2,3,2,1},{3,2,1,2},{2,0,2,2},{0,2,2,2}};

static void scanAnchor(int x,int y,AnchorEntry& e){
    e.mask=0;e.cond[0]=e.cond[1]=e.cond[2]=e.cond[3]=0;
    for(int d=0;d<8;d++){
        size_t os=sprk.size();reco_ln(x,y,d);
        if(sprk.size()>os){sprk.pop();sprk_f.pop();e.mask|=(uint16_t)(1u<<d);}
    }
    for(int d=0;d<4;d++){
        size_t os=sprk.size();reco_exp(x,y,d);
        if(sprk.size()>os){sprk.pop();Complex v=sprk_f.front();sprk_f.pop();(void)v;
            int c=0; // check condition cells from cv
            if(d==0)c=!(cv[x+2][y+4]||cv[x+1][y+3]||cv[x+3][y+3])?1:0;
            else if(d==1)c=!(cv[x+4][y+2]||cv[x+3][y+1]||cv[x+3][y+3])?1:0;
            else if(d==2)c=!(cv[x+2][y-1]||cv[x+1][y+0]||cv[x+3][y+0])?1:0;
            else if(d==3)c=!(cv[x-1][y+2]||cv[x+0][y+1]||cv[x+0][y+3])?1:0;
            e.mask|=(uint16_t)(1u<<(8+d));e.cond[d]=(uint8_t)c;}
    }
}

static void emitPatterns(){
    g_pats.clear();
    if(g_anchorPats.empty())return;
    std::vector<uint32_t> keys;keys.reserve(g_anchorPats.size());
    for(auto& kv:g_anchorPats)keys.push_back(kv.first);
    std::sort(keys.begin(),keys.end());
    for(size_t i=0;i<keys.size();i++){
        const AnchorEntry& e=g_anchorPats[keys[i]];
        int x=(int)(keys[i]%W_MAX),y=(int)(keys[i]/W_MAX);
        for(int d=0;d<8;d++)if(e.mask&(1u<<d))
            g_pats.push_back({x+ANCHOR_B[d][0],y+ANCHOR_B[d][1],x+ANCHOR_B[d][2],y+ANCHOR_B[d][3],0,0});
        for(int d=0;d<4;d++)if(e.mask&(1u<<(8+d)))
            g_pats.push_back({x+ANCHOR_BE[d][0],y+ANCHOR_BE[d][1],x+ANCHOR_BE[d][2],y+ANCHOR_BE[d][3],1,e.cond[d]});
    }
}

void rebuildPatterns(){
    while(!sprk.empty())sprk.pop();
    while(!sprk_f.empty())sprk_f.pop();
    g_anchorPats.clear();
    for(int y=0;y<H-5;y++)for(int x=0;x<W-5;x++){
        AnchorEntry e;scanAnchor(x,y,e);
        if(e.mask)g_anchorPats[(uint32_t)y*W_MAX+(uint32_t)x]=e;
    }
    emitPatterns();
}

// Only anchors whose 5x5 window overlaps the edited rectangle can change: the
// recognisers read offsets x-1..x+4 and y-1..y+4 relative to the anchor, so an edited
// cell (ex,ey) affects anchors with ax in [ex-4,ex+1] / ay in [ey-4,ey+1]; 5 cells of
// dilation are used for margin. Everything else keeps its previous match state.
void rebuildPatternsAround(int x0,int y0,int x1,int y1){
    while(!sprk.empty())sprk.pop();
    while(!sprk_f.empty())sprk_f.pop();
    int ax0=std::max(0,x0-5),ay0=std::max(0,y0-5);
    int ax1=std::min(W-6,x1+5),ay1=std::min(H-6,y1+5);
    bool touched=false;
    for(int y=ay0;y<=ay1;y++)for(int x=ax0;x<=ax1;x++){
        uint32_t k=(uint32_t)y*W_MAX+(uint32_t)x;
        AnchorEntry e;scanAnchor(x,y,e);
        if(e.mask){g_anchorPats[k]=e;touched=true;}
        else if(g_anchorPats.erase(k))touched=true;
    }
    if(touched)emitPatterns();
}

// === Clipboard operations ===

void copy_cv(CoordI p1, CoordI p2) {
    CoordI c1 = {std::min(p1.x, p2.x), std::min(p1.y, p2.y)};
    CoordI c2 = {std::max(p1.x, p2.x), std::max(p1.y, p2.y)};
    for (int y = c1.y; y < c2.y; y++)
        for (int x = c1.x; x < c2.x; x++) {
            int dx = x - c1.x, dy = y - c1.y;
            clip_cv[dx][dy] = cv[x][y];
            clip_fi[dx][dy] = fi[x][y];
        }
    clipW = c2.x - c1.x;
    clipH = c2.y - c1.y;
}

void paste_cv(CoordI center) {
    for (int y = 0; y < clipH; y++)
        for (int x = 0; x < clipW; x++) {
            int gx = x + center.x - clipW / 2;
            int gy = y + center.y - clipH / 2;
            if (gx >= 0 && gx < W && gy >= 0 && gy < H && clip_cv[x][y]) {
                cv[gx][gy] = clip_cv[x][y];
                fi[gx][gy] = clip_fi[x][y];
            }
        }
}

void clearGrid() {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            cv[x][y] = 0;
            fi[x][y] = Complex(0, 0);
        }
    sprk = std::queue<CoordI>();
    sprk_f = std::queue<Complex>();
}

// === File I/O ===

void saveGrid(const std::string& fn) {
    std::ofstream fout(fn, std::ios::trunc);
    if (!fout) {
        std::cout << "Failed to save: " << fn << '\n';
        return;
    }
    fout << W << ' ' << H << '\n';
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++)
            fout << (int)cv[x][y] << ' ';
        fout << '\n';
    }
    fout.close();
    std::cout << "Saved: " << fn << '\n';
}

void loadGrid(const std::string& fn) {
    std::ifstream fin(fn);
    if (!fin) {
        std::cout << "Failed to load: " << fn << '\n';
        return;
    }
    int saveW, saveH;
    fin >> saveW >> saveH;
    if (saveW < 1) saveW = 1;
    if (saveH < 1) saveH = 1;
    if (saveW > W_MAX) saveW = W_MAX;
    if (saveH > W_MAX) saveH = W_MAX;

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            cv[x][y] = 0;
            fi[x][y] = Complex(0, 0);
        }

    for (int sy = 0; sy < saveH; sy++)
        for (int sx = 0; sx < saveW; sx++) {
            int tmp;
            fin >> tmp;
            if (sx < W && sy < H)
                cv[sx][sy] = (uint8_t)tmp;
        }

    fin.close();
    std::cout << "Loaded: " << fn << " (save size " << saveW << "x" << saveH
              << " -> grid size " << W << "x" << H << ")\n";
}

// === Color mapping: complex -> ARGB ===

// Same mapping as before, but without powf() and without the two fmodf() calls
// (measured 1.45x on a fully drawn canvas):
//   luminance l = 1 - 0.5^|c|     (powf(0.5f,x) == exp2f(-x), and the old exponent
//                                  (|c|-1)*1+1 simplifies to |c|)
//   hue h = (arg(c)/pi - 1/9)*180, wrapped into [0,360) by the "+2 if negative" branch
//           -> the old fmodf(h,360) could never trigger
//   x = c * (1 - |fmodf(h/60,2) - 1|)   -> equivalent floor-based wrap
// Checked against the old formula over 2,000,000 random samples: identical except
// ~0.0008% of samples where one channel differs by 1/255.
static constexpr double PI_1 = 0.31830988618379067153;
static constexpr double ARGBIAS1 = -0.11111111111111111111;

uint32_t magic_c2col(const Complex& c) {
    float l = 1.0f - exp2f(-(float)std::abs(c));
    float xarg = (float)(std::arg(c) * PI_1 + ARGBIAS1);
    if (xarg < 0.0f) xarg += 2.0f;
    float h = xarg * 180.0f;
    float cc = 1.0f - fabsf(2.0f * l - 1.0f);
    float u = h * (1.0f / 60.0f);
    u -= 2.0f * floorf(u * 0.5f);
    float xx = cc * (1.0f - fabsf(u - 1.0f));
    float m = l - cc * 0.5f;
    float r, g, b;
    if (h < 60.0f)       { r = cc; g = xx; b = 0.0f; }
    else if (h < 120.0f) { r = xx; g = cc; b = 0.0f; }
    else if (h < 180.0f) { r = 0.0f; g = cc; b = xx; }
    else if (h < 240.0f) { r = 0.0f; g = xx; b = cc; }
    else if (h < 300.0f) { r = xx; g = 0.0f; b = cc; }
    else                 { r = cc; g = 0.0f; b = xx; }
    return ((uint32_t)(uint8_t)((r + m) * 255.0f) << 16) |
           ((uint32_t)(uint8_t)((g + m) * 255.0f) << 8)  |
           ((uint32_t)(uint8_t)((b + m) * 255.0f));
}

// === Incremental grid -> colour buffer ===
// Recomputes only the cells whose (cv, fi) changed since the previous call, so the
// per-frame cost becomes proportional to what actually moved instead of W*H.
void syncGridColors(CellRGBA* out, std::vector<int>& changed, int W_, int H_, float wireAlpha) {
    static std::vector<Complex> pfi;
    static std::vector<uint8_t> pcv;      // 0xFF = "never computed"
    static int cw = -1, ch = -1;
    if (cw != W_ || ch != H_) {
        pfi.assign((size_t)W_ * H_, Complex(0, 0));
        pcv.assign((size_t)W_ * H_, 0xFF);
        cw = W_; ch = H_;
    }
    changed.clear();
    for (int y = 0; y < H_; y++) for (int x = 0; x < W_; x++) {
        size_t i = (size_t)y * W_ + x;
        uint8_t c = cv[x][y];
        if (c == pcv[i] && (c == 0 || fi[x][y] == pfi[i])) continue;
        pcv[i] = c;
        if (c == 0) {
            out[i].r = out[i].g = out[i].b = out[i].a = 0.0f;
        } else {
            Complex v = fi[x][y];
            pfi[i] = v;
            uint32_t argb = magic_c2col(v);
            out[i].r = ((argb >> 16) & 0xFF) / 255.0f;
            out[i].g = ((argb >> 8) & 0xFF) / 255.0f;
            out[i].b = (argb & 0xFF) / 255.0f;
            out[i].a = wireAlpha;
        }
        changed.push_back((int)i);
    }
}
