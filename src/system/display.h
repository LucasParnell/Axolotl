#pragma once

#include "data/display_state.h"

class GlDisplay {
public:
    GlDisplay();
    ~GlDisplay();

    bool init();
    bool tick();
    void shutdown();

    void submitFrame(const uint8_t* vram, uint32_t vramSize);

    bool shouldClose() const;
    void requestClose();
    GLFWwindow* GetWindow() const { return state.window; }
    void toggleFullscreen();
    bool isFullscreen() const { return state.fullscreen; }

private:
    void createTexture();
    void buildQuadVAO();
    void compileShaders();
    void updateViewportIfNeeded();

    DisplayState state;
};
