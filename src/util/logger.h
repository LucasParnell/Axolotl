#pragma once

#include <string>
#include <functional>

enum class LogLevel {
    INFO,
    WARNING,
    ERR,
    DEBUG
};

class Logger {
public:
    static void setOnWarning(std::function<void(const std::string&)> f);
    static void setInteractivePrompt(bool enabled,
                                     const std::string& prompt = "gba> ",
                                     bool suppress_info = false);
    static void log(const std::string& message, LogLevel level);
    static void writeToFile(const std::string& path);
};
