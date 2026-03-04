//
// Display.cc — GLFW + OpenGL framebuffer renderer (Mode 3 only)
//

#include "system/display.h"
#include "util/logger.h"

#include <cstring>
#include <sstream>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// ── Minimal OpenGL loader (only the functions we need; no GLAD) ───────
#include <GL/gl.h>
#ifdef _WIN32
#ifndef GL_SIZEIPTR_DEFINED
typedef ptrdiff_t GLsizeiptr;
#define GL_SIZEIPTR_DEFINED
#endif
#ifndef GL_CHAR_DEFINED
typedef char GLchar;
#define GL_CHAR_DEFINED
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
#endif

typedef void   (APIENTRY *PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void   (APIENTRY *PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef void   (APIENTRY *PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void   (APIENTRY *PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void   (APIENTRY *PFNGLCOMPILESHADERPROC)(GLuint);
typedef GLuint (APIENTRY *PFNGLCREATEPROGRAMPROC)();
typedef GLuint (APIENTRY *PFNGLCREATESHADERPROC)(GLenum);
typedef void   (APIENTRY *PFNGLDELETEPROGRAMPROC)(GLuint);
typedef void   (APIENTRY *PFNGLDELETESHADERPROC)(GLuint);
typedef void   (APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void   (APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (APIENTRY *PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLint  (APIENTRY *PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar*);
typedef void   (APIENTRY *PFNGLLINKPROGRAMPROC)(GLuint);
typedef void   (APIENTRY *PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar*const*, const GLint*);
typedef void   (APIENTRY *PFNGLUSEPROGRAMPROC)(GLuint);
typedef void   (APIENTRY *PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void   (APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void   (APIENTRY *PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void   (APIENTRY *PFNGLDELETEVERTEXARRAYSPROC)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint*);

#define GL_ARRAY_BUFFER           0x8892
#define GL_STATIC_DRAW            0x88E4
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_VERTEX_SHADER          0x8B31
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_INFO_LOG_LENGTH        0x8B84
#define GL_TEXTURE0               0x84C0

static PFNGLATTACHSHADERPROC            glAttachShader_            = nullptr;
static PFNGLBINDVERTEXARRAYPROC         glBindVertexArray_          = nullptr;
static PFNGLBINDBUFFERPROC              glBindBuffer_              = nullptr;
static PFNGLBUFFERDATAPROC              glBufferData_              = nullptr;
static PFNGLCOMPILESHADERPROC           glCompileShader_            = nullptr;
static PFNGLCREATEPROGRAMPROC           glCreateProgram_            = nullptr;
static PFNGLCREATESHADERPROC            glCreateShader_            = nullptr;
static PFNGLDELETEPROGRAMPROC           glDeleteProgram_           = nullptr;
static PFNGLDELETESHADERPROC            glDeleteShader_            = nullptr;
static PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_  = nullptr;
static PFNGLGENBUFFERSPROC               glGenBuffers_              = nullptr;
static PFNGLGENVERTEXARRAYSPROC         glGenVertexArrays_         = nullptr;
static PFNGLGETPROGRAMIVPROC            glGetProgramiv_             = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC       glGetProgramInfoLog_       = nullptr;
static PFNGLGETSHADERIVPROC             glGetShaderiv_             = nullptr;
static PFNGLGETSHADERINFOLOGPROC        glGetShaderInfoLog_        = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC      glGetUniformLocation_      = nullptr;
static PFNGLLINKPROGRAMPROC             glLinkProgram_             = nullptr;
static PFNGLSHADERSOURCEPROC             glShaderSource_            = nullptr;
static PFNGLUSEPROGRAMPROC              glUseProgram_              = nullptr;
static PFNGLUNIFORM1IPROC               glUniform1i_               = nullptr;
static PFNGLVERTEXATTRIBPOINTERPROC     glVertexAttribPointer_     = nullptr;
static PFNGLACTIVETEXTUREPROC           glActiveTexture_           = nullptr;
static PFNGLDELETEVERTEXARRAYSPROC      glDeleteVertexArrays_      = nullptr;
static PFNGLDELETEBUFFERSPROC           glDeleteBuffers_           = nullptr;

static bool loadGLFunctions() {
#define LOAD(name) name##_ = reinterpret_cast<decltype(name##_)>(glfwGetProcAddress(#name)); \
    if (!name##_) { Logger::log("[Display] Failed to load " #name, LogLevel::ERR); return false; }
    LOAD(glAttachShader)
    LOAD(glBindVertexArray)
    LOAD(glBindBuffer)
    LOAD(glBufferData)
    LOAD(glCompileShader)
    LOAD(glCreateProgram)
    LOAD(glCreateShader)
    LOAD(glDeleteProgram)
    LOAD(glDeleteShader)
    LOAD(glEnableVertexAttribArray)
    LOAD(glGenBuffers)
    LOAD(glGenVertexArrays)
    LOAD(glGetProgramiv)
    LOAD(glGetProgramInfoLog)
    LOAD(glGetShaderiv)
    LOAD(glGetShaderInfoLog)
    LOAD(glGetUniformLocation)
    LOAD(glLinkProgram)
    LOAD(glShaderSource)
    LOAD(glUseProgram)
    LOAD(glUniform1i)
    LOAD(glVertexAttribPointer)
    LOAD(glActiveTexture)
    LOAD(glDeleteVertexArrays)
    LOAD(glDeleteBuffers)
#undef LOAD
    return true;
}

// ── Shader sources ─────────────────────────────────────────────────────
static const char* VERT_SRC = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

static const char* FRAG_SRC = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    fragColor = texture(uTex, vUV);
}
)";

// ── Helpers ────────────────────────────────────────────────────────────
static GLuint compileShaderStage(GLenum type, const char* src) {
    GLuint s = glCreateShader_(type);
    glShaderSource_(s, 1, &src, nullptr);
    glCompileShader_(s);
    GLint ok = 0;
    glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog_(s, sizeof(buf), nullptr, buf);
        std::stringstream ss;
        ss << "[Display] Shader compile error: " << buf;
        Logger::log(ss.str(), LogLevel::ERR);
    }
    return s;
}

// ── Display implementation ─────────────────────────────────────────────

Display::Display() {
    std::memset(state.buffers, 0, sizeof(state.buffers));
}

Display::~Display() {
    shutdown();
}

bool Display::init() {
    if (!glfwInit()) {
        Logger::log("[Display] glfwInit failed", LogLevel::ERR);
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    state.window = glfwCreateWindow(DisplayState::GBA_WIDTH  * DisplayState::SCALE,
                                    DisplayState::GBA_HEIGHT * DisplayState::SCALE,
                                    "Axolotl", nullptr, nullptr);
    if (!state.window) {
        Logger::log("[Display] Failed to create GLFW window", LogLevel::ERR);
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(state.window);
    glfwSwapInterval(1);

    if (!loadGLFunctions()) {
        Logger::log("[Display] Failed to load OpenGL functions", LogLevel::ERR);
        glfwDestroyWindow(state.window);
        glfwTerminate();
        state.window = nullptr;
        return false;
    }

    compileShaders();
    createTexture();
    buildQuadVAO();

    int fbW, fbH;
    glfwGetFramebufferSize(state.window, &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    Logger::log("[Display] Initialised — 720×480 (Mode 3, 3× scale)", LogLevel::INFO);
    return true;
}

bool Display::tick() {
    if (!state.window) return false;

    if (glfwWindowShouldClose(state.window)) {
        state.closed = true;
        return false;
    }

    glfwPollEvents();

    if (state.frameDirty.exchange(false)) {
        int readIdx = state.frontIndex.load(std::memory_order_acquire);
        glBindTexture(GL_TEXTURE_2D, state.texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        DisplayState::GBA_WIDTH, DisplayState::GBA_HEIGHT,
                        GL_RGB, GL_UNSIGNED_BYTE,
                        state.buffers[readIdx]);
    }

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram_(state.shader);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state.texture);
    glUniform1i_(glGetUniformLocation_(state.shader, "uTex"), 0);
    glBindVertexArray_(state.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray_(0);

    glfwSwapBuffers(state.window);
    return true;
}

void Display::shutdown() {
    if (!state.window) return;

    if (state.shader)  { glDeleteProgram_(state.shader);         state.shader  = 0; }
    if (state.vao)     { glDeleteVertexArrays_(1, &state.vao);   state.vao     = 0; }
    if (state.vbo)     { glDeleteBuffers_(1, &state.vbo);        state.vbo     = 0; }
    if (state.texture) { glDeleteTextures(1, &state.texture);   state.texture = 0; }

    glfwDestroyWindow(state.window);
    state.window = nullptr;
    glfwTerminate();

    Logger::log("[Display] Shut down", LogLevel::INFO);
}

void Display::submitFrame(const uint8_t* vram, uint32_t vramSize) {
    int writeIdx = 1 - state.frontIndex.load(std::memory_order_acquire);
    uint8_t* dst = state.buffers[writeIdx];

    constexpr uint32_t RGB888_SIZE = DisplayState::GBA_WIDTH * DisplayState::GBA_HEIGHT * 3;

    if (vramSize == RGB888_SIZE) {
        std::memcpy(dst, vram, RGB888_SIZE);
    } else {
        for (int y = 0; y < DisplayState::GBA_HEIGHT; ++y) {
            for (int x = 0; x < DisplayState::GBA_WIDTH; ++x) {
                int srcOff = (y * DisplayState::GBA_WIDTH + x) * 2;
                uint16_t pixel = static_cast<uint16_t>(vram[srcOff])
                               | (static_cast<uint16_t>(vram[srcOff + 1]) << 8);
                int dstOff = (y * DisplayState::GBA_WIDTH + x) * 3;
                dst[dstOff + 0] = (pixel & 0x1F) << 3;
                dst[dstOff + 1] = ((pixel >> 5)  & 0x1F) << 3;
                dst[dstOff + 2] = ((pixel >> 10) & 0x1F) << 3;
            }
        }
    }

    state.frontIndex.store(writeIdx, std::memory_order_release);
    state.frameDirty.store(true,     std::memory_order_release);
}

bool Display::shouldClose() const {
    return state.closed.load(std::memory_order_relaxed);
}

void Display::requestClose() {
    state.closed.store(true, std::memory_order_relaxed);
}

// ── Private helpers ────────────────────────────────────────────────────

void Display::createTexture() {
    glGenTextures(1, &state.texture);
    glBindTexture(GL_TEXTURE_2D, state.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, DisplayState::GBA_WIDTH, DisplayState::GBA_HEIGHT, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, state.buffers[0]);
}

void Display::buildQuadVAO() {
    float vertices[] = {
        // pos        // uv
        -1.f, -1.f,   0.f, 1.f,
         1.f, -1.f,   1.f, 1.f,
        -1.f,  1.f,   0.f, 0.f,
         1.f,  1.f,   1.f, 0.f,
    };

    glGenVertexArrays_(1, &state.vao);
    glGenBuffers_(1, &state.vbo);

    glBindVertexArray_(state.vao);
    glBindBuffer_(GL_ARRAY_BUFFER, state.vbo);
    glBufferData_(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray_(0);
    glVertexAttribPointer_(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray_(1);

    glBindVertexArray_(0);
}

void Display::compileShaders() {
    GLuint vert = compileShaderStage(GL_VERTEX_SHADER,   VERT_SRC);
    GLuint frag = compileShaderStage(GL_FRAGMENT_SHADER, FRAG_SRC);

    state.shader = glCreateProgram_();
    glAttachShader_(state.shader, vert);
    glAttachShader_(state.shader, frag);
    glLinkProgram_(state.shader);

    GLint ok = 0;
    glGetProgramiv_(state.shader, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog_(state.shader, sizeof(buf), nullptr, buf);
        std::stringstream ss;
        ss << "[Display] Shader link error: " << buf;
        Logger::log(ss.str(), LogLevel::ERR);
    }

    glDeleteShader_(vert);
    glDeleteShader_(frag);
}
