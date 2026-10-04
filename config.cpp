#include "config.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <algorithm>

bool loadConfig(const char* path, Config& cfg) {
    std::ifstream fin(path);
    if (!fin) {
        fprintf(stderr, "[config] Failed to open %s, using defaults\n", path);
        return false;
    }

    fin >> cfg.gridW >> cfg.gridH >> cfg.rayRadius >> cfg.winW >> cfg.winH;

    cfg.gridW = std::max(1, std::min(cfg.gridW, 1024));
    cfg.gridH = std::max(1, std::min(cfg.gridH, 1024));
    cfg.rayRadius = std::max(1, cfg.rayRadius);
    cfg.winW = std::max(100, cfg.winW);
    cfg.winH = std::max(100, cfg.winH);

    // font name line
    std::string line;
    std::getline(fin, line); // consume rest of first line
    std::getline(fin, cfg.fontName);

    // wire alpha
    std::getline(fin, line);
    cfg.wireAlpha = std::stof(line);

    // text color
    float tr, tg, tb, ta;
    fin >> tr >> tg >> tb >> ta;
    cfg.textR = tr / 255.0f;
    cfg.textG = tg / 255.0f;
    cfg.textB = tb / 255.0f;
    cfg.textA = ta / 255.0f;

    // sun color
    float sr, sg, sb;
    fin >> sr >> sg >> sb;
    cfg.sunR = sr / 255.0f * 10.0f;
    cfg.sunG = sg / 255.0f * 10.0f;
    cfg.sunB = sb / 255.0f * 10.0f;

    // wall color
    float wr, wg, wb;
    fin >> wr >> wg >> wb;
    cfg.wallR = wr / 255.0f * 0.5f;
    cfg.wallG = wg / 255.0f * 0.5f;
    cfg.wallB = wb / 255.0f * 0.5f;

    // emit brightness
    fin >> cfg.emitBrightness;
    fin.close();
    return true;
}
