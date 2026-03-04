#pragma once

#include "data/display_state.h"

class Display {
public:
    Display();
    ~Display();

    bool init();
    bool tick();
    void shutdown();

    void submitFrame(const uint8_t* vram, uint32_t vramSize);

    bool shouldClose() const;
    void requestClose();

private:
    void createTexture();
    void buildQuadVAO();
    void compileShaders();

    DisplayState state;
};
