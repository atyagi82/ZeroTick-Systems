#include "zerotick/Log.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace zerotick {
namespace {

struct LoggerState {
    std::mutex mutex;
    std::ofstream file;
    std::string path;
    bool initialized = false;
    bool writeFailureReported = false;
};

LoggerState& loggerState() {
    static LoggerState state;
    return state;
}

std::string timestampNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime = {};
#if defined(_WIN32)
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
    std::ostringstream formatted;
    formatted << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
              << '.' << std::setfill('0') << std::setw(3) << milliseconds;
    return formatted.str();
}

void reportWriteFailure(LoggerState& state) {
    if (!state.writeFailureReported) {
        std::cerr << "Logging error: unable to write " << state.path << '\n';
        state.writeFailureReported = true;
    }
}

void writeFileLine(LoggerState& state, const std::string& message) {
    if (!state.initialized) {
        state.path = "logs.txt";
        state.file.open(state.path.c_str(), std::ios::out | std::ios::app);
        state.initialized = state.file.is_open();
        if (!state.initialized) {
            reportWriteFailure(state);
            return;
        }
    }
    if (!state.file) {
        reportWriteFailure(state);
        return;
    }
    state.file << timestampNow() << ' ' << message << '\n';
    state.file.flush();
    if (!state.file) {
        reportWriteFailure(state);
    }
}

} // namespace

void initializeLogFile(const std::string& path) {
    LoggerState& state = loggerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.initialized) {
        if (state.path != path) {
            throw std::logic_error("Log file was already initialized");
        }
        return;
    }
    state.path = path;
    state.file.open(path.c_str(), std::ios::out | std::ios::app);
    if (!state.file.is_open()) {
        throw std::runtime_error("Unable to open log file: " + path);
    }
    state.initialized = true;
    writeFileLine(state, "[session] started");
}

void logLine(const std::string& message) {
    LoggerState& state = loggerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    std::cout << message << std::endl;
    writeFileLine(state, message);
}

} // namespace zerotick
