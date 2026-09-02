#pragma once
#include <cstddef>
#include <cstdint>

// Inert file system: the preview never touches storage.
namespace fs {
class File {
public:
    operator bool() const { return false; }
    bool isDirectory() const { return false; }
    std::size_t size() const { return 0; }
    std::size_t available() const { return 0; }
    void close() {}
    bool seek(std::uint32_t) { return false; }
    std::size_t read(std::uint8_t*, std::size_t) { return 0; }
    std::size_t write(const std::uint8_t*, std::size_t) { return 0; }
    const char* name() const { return ""; }
};
class FS {};
}  // namespace fs
using fs::File;
