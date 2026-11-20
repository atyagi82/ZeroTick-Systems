#pragma once

#include <string>

namespace zerotick {

void initializeLogFile(const std::string& path = "logs.txt");
void logLine(const std::string& message);

} // namespace zerotick
