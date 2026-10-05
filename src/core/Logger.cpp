#include "core/Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace vtc {

namespace {
std::string timestampNow() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::ostringstream os;
    os << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return os.str();
}
}  // namespace

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARNING";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::lock_guard lock(mutex_);
    return level_;
}

bool Logger::openFile(const std::filesystem::path& file, std::uintmax_t maxBytes) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > maxBytes) {
        std::filesystem::path old = file;
        old += ".1";
        std::filesystem::remove(old, ec);
        std::filesystem::rename(file, old, ec);
    }
    file_.close();
    file_.open(file, std::ios::out | std::ios::app);
    path_ = file;
    return file_.is_open();
}

void Logger::setEchoToStderr(bool echo) {
    std::lock_guard lock(mutex_);
    echo_ = echo;
}

std::filesystem::path Logger::filePath() const {
    std::lock_guard lock(mutex_);
    return path_;
}

void Logger::write(LogLevel level, std::string_view category, std::string_view message) {
    std::lock_guard lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(level_)) {
        return;
    }
    std::ostringstream line;
    line << timestampNow() << " [" << toString(level) << "] [" << category << "] " << message << '\n';
    const std::string s = line.str();
    if (file_.is_open()) {
        file_ << s;
        file_.flush();
    }
    if (echo_) {
        std::cerr << s;
    }
}

void logDebug(std::string_view c, std::string_view m) { Logger::instance().write(LogLevel::Debug, c, m); }
void logInfo(std::string_view c, std::string_view m) { Logger::instance().write(LogLevel::Info, c, m); }
void logWarning(std::string_view c, std::string_view m) { Logger::instance().write(LogLevel::Warning, c, m); }
void logError(std::string_view c, std::string_view m) { Logger::instance().write(LogLevel::Error, c, m); }

}  // namespace vtc
