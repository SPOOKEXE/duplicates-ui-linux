#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

enum class LogLevel { Info, Warn, Error };

const char* logLevelName(LogLevel level);

struct LogLine {
    LogLevel level = LogLevel::Info;
    std::string stamp;  // 14:02:33, local time
    std::string text;
};

// The running commentary of a scan or an apply, shared by every thread that has
// something to say. One instance lives in AppState, so the Log tab shows the
// whole story of a run rather than only the part one subsystem happened to own.
//
// A rolling window, because an apply over a hundred thousand files would
// otherwise be its own memory problem. Whatever falls off the front is counted,
// so the tab can say so rather than quietly lying about what happened. Nothing
// here is the permanent record: that is the run manifest.
class Log {
public:
    void add(LogLevel level, std::string text);
    void info(std::string text) { add(LogLevel::Info, std::move(text)); }
    void warn(std::string text) { add(LogLevel::Warn, std::move(text)); }
    void error(std::string text) { add(LogLevel::Error, std::move(text)); }

    std::vector<LogLine> lines() const;
    void clear();

    // Lines pushed off the front of the window since the last clear.
    size_t dropped() const;
    size_t count(LogLevel level) const;

private:
    mutable std::mutex mutex_;
    std::deque<LogLine> lines_;
    size_t dropped_ = 0;
    size_t counts_[3] = {0, 0, 0};
};
