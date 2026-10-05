#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace vtc {

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

// Local, thread-safe log. Policy (see docs/ARCHITECTURE.md, "Privacidade"):
//  - never log pixel data;
//  - never log patient identifiers (names, IDs, birth dates) at any level;
//  - file paths may contain patient names, so they are logged only at Debug
//    level, which is disabled by default.
class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    [[nodiscard]] LogLevel level() const;
    // Opens (append) the log file. Rotates when it exceeds maxBytes.
    bool openFile(const std::filesystem::path& file, std::uintmax_t maxBytes = 5u * 1024u * 1024u);
    void setEchoToStderr(bool echo);
    [[nodiscard]] std::filesystem::path filePath() const;

    void write(LogLevel level, std::string_view category, std::string_view message);

private:
    Logger() = default;
    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    std::ofstream file_;
    std::filesystem::path path_;
    bool echo_ = false;
};

void logDebug(std::string_view category, std::string_view message);
void logInfo(std::string_view category, std::string_view message);
void logWarning(std::string_view category, std::string_view message);
void logError(std::string_view category, std::string_view message);

const char* toString(LogLevel level);

}  // namespace vtc
