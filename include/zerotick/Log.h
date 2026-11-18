#pragma once

#include <iostream>
#include <mutex>
#include <string>

namespace zerotick {

// Keep each multi-threaded status line intact on the console.
inline void logLine(const std::string& message) {
    static std::mutex outputMutex;
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << message << std::endl;
}

} // namespace zerotick
