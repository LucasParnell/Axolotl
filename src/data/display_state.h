//
// display_state.h — plain data for the display subsystem
//

#pragma once

#include <atomic>
#include <cstdint>

struct GLFWwindow;

struct DisplayState {
    static constexpr int GBA_WIDTH  = 240;
    static constexpr int GBA_HEIGHT = 160;
    static constexpr int SCALE      = 3;

    GLFWwindow* window = nullptr;

    // OpenGL handles
    unsigned int texture = 0;
    unsigned int vao     = 0;
    unsigned int vbo     = 0;
    unsigned int shader  = 0;

    // Double-buffered pixel data (RGB888, 240×160)
    uint8_t buffers[2][GBA_WIDTH * GBA_HEIGHT * 3]{};
    std::atomic<int>  frontIndex{0};
    std::atomic<bool> frameDirty{false};

    std::atomic<bool> closed{false};
};
