// Minimal Arduino surface for the desktop UI preview. Only what the compiled
// drawing unit and the headers it includes touch is provided.
#pragma once

#include <M5GFX.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define PROGMEM
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

namespace hermes_sim {
unsigned long simMillis();
void simDelay(unsigned long ms);
}  // namespace hermes_sim

// The harness owns the clock so screenshots are deterministic.
#define millis() hermes_sim::simMillis()
#define delay(ms) hermes_sim::simDelay(ms)

using std::max;
using std::min;

// Arduino String modelled on std::string with the members the UI uses.
class String {
public:
    String() = default;
    String(const char* text) : value_(text ? text : "") {}
    String(const std::string& text) : value_(text) {}
    String(char character) : value_(1, character) {}
    String(int number) : value_(std::to_string(number)) {}
    String(unsigned int number) : value_(std::to_string(number)) {}
    String(long number) : value_(std::to_string(number)) {}
    String(unsigned long number) : value_(std::to_string(number)) {}
    String(long long number) : value_(std::to_string(number)) {}
    String(unsigned long long number) : value_(std::to_string(number)) {}

    const char* c_str() const { return value_.c_str(); }
    operator const char*() const { return value_.c_str(); }
    std::size_t length() const { return value_.size(); }
    bool isEmpty() const { return value_.empty(); }
    bool reserve(std::size_t size) { value_.reserve(size); return true; }
    char operator[](std::size_t index) const { return value_[index]; }
    char& operator[](std::size_t index) { return value_[index]; }
    char charAt(std::size_t index) const { return value_[index]; }

    String& operator+=(const String& other) { value_ += other.value_; return *this; }
    String& operator+=(const char* other) { value_ += other ? other : ""; return *this; }
    String& operator+=(char character) { value_ += character; return *this; }
    friend String operator+(const String& a, const String& b) { return String(a.value_ + b.value_); }
    friend String operator+(const String& a, const char* b) { return String(a.value_ + (b ? b : "")); }
    friend String operator+(const char* a, const String& b) { return String(std::string(a ? a : "") + b.value_); }
    friend String operator+(const String& a, char b) { return String(a.value_ + b); }
    bool operator==(const String& other) const { return value_ == other.value_; }
    bool operator!=(const String& other) const { return value_ != other.value_; }
    bool operator==(const char* other) const { return value_ == (other ? other : ""); }
    bool operator!=(const char* other) const { return !(*this == other); }

    String substring(std::size_t from) const
    {
        return from >= value_.size() ? String() : String(value_.substr(from));
    }
    String substring(std::size_t from, std::size_t to) const
    {
        if (from >= value_.size() || to <= from) return String();
        return String(value_.substr(from, to - from));
    }
    int indexOf(char character, std::size_t from = 0) const
    {
        const auto position = value_.find(character, from);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    int indexOf(const char* text, std::size_t from = 0) const
    {
        const auto position = value_.find(text ? text : "", from);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    int indexOf(const String& text, std::size_t from = 0) const { return indexOf(text.c_str(), from); }
    int lastIndexOf(char character) const
    {
        const auto position = value_.rfind(character);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    bool startsWith(const char* prefix) const { return value_.rfind(prefix ? prefix : "", 0) == 0; }
    bool startsWith(const String& prefix) const { return startsWith(prefix.c_str()); }
    bool endsWith(const char* suffix) const
    {
        const std::string tail(suffix ? suffix : "");
        return value_.size() >= tail.size() &&
               value_.compare(value_.size() - tail.size(), tail.size(), tail) == 0;
    }
    bool endsWith(const String& suffix) const { return endsWith(suffix.c_str()); }
    void remove(std::size_t index) { if (index < value_.size()) value_.erase(index); }
    void remove(std::size_t index, std::size_t count) { if (index < value_.size()) value_.erase(index, count); }
    void trim()
    {
        const auto first = value_.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) { value_.clear(); return; }
        const auto last = value_.find_last_not_of(" \t\r\n");
        value_ = value_.substr(first, last - first + 1);
    }
    void toLowerCase() { for (auto& c : value_) c = static_cast<char>(tolower(c)); }
    void toUpperCase() { for (auto& c : value_) c = static_cast<char>(toupper(c)); }
    long toInt() const { return std::strtol(value_.c_str(), nullptr, 10); }

private:
    std::string value_;
};
