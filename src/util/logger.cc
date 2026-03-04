#include "util/logger.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <cstdlib>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
    struct SpinGuard {
        explicit SpinGuard(std::atomic_flag& flag) : flag_(flag) {
            while (flag_.test_and_set(std::memory_order_acquire)) {}
        }
        ~SpinGuard() { flag_.clear(std::memory_order_release); }
        std::atomic_flag& flag_;
    };

    std::atomic_flag logLock = ATOMIC_FLAG_INIT;
    std::vector<std::string> logBuffer;
    std::function<void(const std::string&)> onWarning;
    LogLevel minLevel = LogLevel::INFO;
    bool interactivePromptEnabled = false;
    bool interactiveSuppressInfo = false;
    std::string interactivePrompt = "gba> ";

    int levelRank(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG:   return 0;
            case LogLevel::INFO:    return 1;
            case LogLevel::WARNING: return 2;
            case LogLevel::ERR:     return 3;
            default:                return 1;
        }
    }

    void initMinLevelFromEnv() {
        const char* env = std::getenv("AXOLOTL_LOG_LEVEL");
        if (!env) return;
        std::string_view v(env);
        if (v == "DEBUG" || v == "debug") minLevel = LogLevel::DEBUG;
        else if (v == "INFO" || v == "info") minLevel = LogLevel::INFO;
        else if (v == "WARNING" || v == "warning" || v == "WARN" || v == "warn") minLevel = LogLevel::WARNING;
        else if (v == "ERR" || v == "err" || v == "ERROR" || v == "error") minLevel = LogLevel::ERR;
    }

    std::string toString(LogLevel level) {
        switch (level) {
            case LogLevel::INFO:    return "INFO";
            case LogLevel::WARNING: return "WARNING";
            case LogLevel::ERR:     return "ERR";
            case LogLevel::DEBUG:   return "DEBUG";
            default:                return "UNKNOWN";
        }
    }
}

void Logger::setOnWarning(std::function<void(const std::string&)> f) {
    SpinGuard lock(logLock);
    onWarning = std::move(f);
}

void Logger::setInteractivePrompt(bool enabled, const std::string& prompt, bool suppress_info) {
    SpinGuard lock(logLock);
    interactivePromptEnabled = enabled;
    interactiveSuppressInfo = suppress_info;
    if (!prompt.empty()) interactivePrompt = prompt;
}

void Logger::log(const std::string& message, LogLevel level) {
    static bool init_done = (initMinLevelFromEnv(), true);
    (void)init_done;
    bool interactive_drop_info = false;
    {
        SpinGuard lock(logLock);
        interactive_drop_info = interactivePromptEnabled && interactiveSuppressInfo && (level == LogLevel::INFO);
    }
    if (interactive_drop_info || levelRank(level) < levelRank(minLevel)) return;

    std::string levelStr = toString(level);
    std::string line = "[" + levelStr + "]: " + message;

    std::function<void(const std::string&)> maybeOnWarning;
    bool interactive_mode = false;
    std::string prompt_copy;
    {
        SpinGuard lock(logLock);
        logBuffer.push_back(line);
        if (level == LogLevel::WARNING && onWarning) maybeOnWarning = onWarning;
        interactive_mode = interactivePromptEnabled;
        prompt_copy = interactivePrompt;
    }
    if (maybeOnWarning) maybeOnWarning(message);

#ifdef _WIN32
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    WORD color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    switch (level) {
        case LogLevel::INFO:    color = FOREGROUND_GREEN; break;
        case LogLevel::WARNING: color = FOREGROUND_RED | FOREGROUND_GREEN; break;
        case LogLevel::ERR:     color = FOREGROUND_RED; break;
        case LogLevel::DEBUG:   color = FOREGROUND_BLUE; break;
    }
    if (interactive_mode) std::cout << "\n";
    SetConsoleTextAttribute(hConsole, color);
    std::cout << line << std::endl;
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    (void)prompt_copy;
#else
    if (interactive_mode) std::cout << "\n";
    switch (level) {
        case LogLevel::INFO:    std::cout << "\033[32m" << line << "\033[0m" << std::endl; break;
        case LogLevel::WARNING: std::cout << "\033[33m" << line << "\033[0m" << std::endl; break;
        case LogLevel::ERR:     std::cout << "\033[31m" << line << "\033[0m" << std::endl; break;
        case LogLevel::DEBUG:   std::cout << "\033[34m" << line << "\033[0m" << std::endl; break;
    }
    (void)prompt_copy;
#endif
}

void Logger::writeToFile(const std::string& path) {
    std::vector<std::string> copy;
    {
        SpinGuard lock(logLock);
        copy = std::move(logBuffer);
        logBuffer.clear();
    }
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }
    std::ofstream out(path);
    if (out) {
        for (const auto& line : copy)
            out << line << '\n';
    }
}
