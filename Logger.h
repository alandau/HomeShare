#pragma once

#include "lib/fmt/format.h"

class Logger {
public:
    enum LogLevel {E, W, I, D};
    
    virtual ~Logger() {}
    
    template <class... Args>
    void log(LogLevel level, const Args&... args) {
        if (shouldLog(level)) {
            logString(level, fmt::format(args...));
        }
    }

    template <class... Args>
    void e(const Args&... args) { log(E, args...); }

    template <class... Args>
    void w(const Args&... args) { log(W, args...); }

    template <class... Args>
    void i(const Args&... args) { log(I, args...); }

    template <class... Args>
    void d(const Args&... args) { log(D, args...); }

protected:
    virtual bool shouldLog(LogLevel level) { return true; }
    virtual void logString(LogLevel level, const std::wstring& s) = 0;
};