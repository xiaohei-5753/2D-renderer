#ifndef CONFIG_H
#define CONFIG_H

#include <string>

struct Config {
    int gridW = 128;
    int gridH = 128;
    int rayRadius = 32;
    int winW = 1280;
    int winH = 1280;

    std::string fontName = "Consolas";

    float wireAlpha = 0.5f;

    float textR = 1.0f, textG = 1.0f, textB = 1.0f, textA = 1.0f;
    float sunR = 0.0f, sunG = 0.0f, sunB = 0.0f;
    float wallR = 0.5f, wallG = 0.5f, wallB = 0.5f;
    float emitBrightness = 1.0f;
};

bool loadConfig(const char* path, Config& cfg);

#endif // CONFIG_H
