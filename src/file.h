#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

class File {
public:
    explicit File(const std::filesystem::path& path);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    std::uint64_t size() const;
    void seek(std::uint64_t offset);
    void read_exact(char* bytes, std::size_t count);
    void write_all(const char* bytes, std::size_t count);
    void truncate(std::uint64_t length);
    void sync();
    void close();

private:
    int fd = -1;
};
