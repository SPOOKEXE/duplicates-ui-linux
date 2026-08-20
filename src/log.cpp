#include "log.h"

#include <ctime>

namespace {

// Enough to follow a long run without holding a line per file forever.
constexpr size_t kMaxLines = 5000;

std::string clockNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm {};
    localtime_r(&now, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

}  // namespace

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "?";
}

void Log::add(LogLevel level, std::string text) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(LogLine {level, clockNow(), std::move(text)});
    ++counts_[static_cast<int>(level)];
    while (lines_.size() > kMaxLines) {
        lines_.pop_front();
        ++dropped_;
    }
}

std::vector<LogLine> Log::lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {lines_.begin(), lines_.end()};
}

void Log::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    dropped_ = 0;
    counts_[0] = counts_[1] = counts_[2] = 0;
}

size_t Log::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

size_t Log::count(LogLevel level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_[static_cast<int>(level)];
}
