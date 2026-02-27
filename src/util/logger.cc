#include "util/logger.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
    std::mutex logMutex;
    std::vector<std::string> logBuffer;
    std::function<void(const std::string&)> onWarning;

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
    std::lock_guard<std::mutex> lock(logMutex);
    onWarning = std::move(f);
}

void Logger::log(const std::string& message, LogLevel level) {
    std::string levelStr = toString(level);
    std::string line = "[" + levelStr + "]: " + message;

    std::function<void(const std::string&)> maybeOnWarning;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        logBuffer.push_back(line);
        if (level == LogLevel::WARNING && onWarning) maybeOnWarning = onWarning;
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
    SetConsoleTextAttribute(hConsole, color);
    std::cout << line << std::endl;
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#else
    switch (level) {
        case LogLevel::INFO:    std::cout << "\033[32m" << line << "\033[0m" << std::endl; break;
        case LogLevel::WARNING: std::cout << "\033[33m" << line << "\033[0m" << std::endl; break;
        case LogLevel::ERR:     std::cout << "\033[31m" << line << "\033[0m" << std::endl; break;
        case LogLevel::DEBUG:   std::cout << "\033[34m" << line << "\033[0m" << std::endl; break;
    }
#endif
}

void Logger::writeToFile(const std::string& path) {
    std::vector<std::string> copy;
    {
        std::lock_guard<std::mutex> lock(logMutex);
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
