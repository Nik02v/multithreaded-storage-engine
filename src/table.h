#pragma once

#include <cstddef>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

constexpr std::size_t max_key_size = 256;
constexpr std::size_t max_value_size = 4096;
void check_key(const std::string& key);
void check_value(const std::string& value);

struct Segment {
    std::shared_mutex mutex;
    std::unordered_map<std::string, std::string> values;
};

class MemoryTable {
public:
    explicit MemoryTable(std::size_t count);
    std::optional<std::string> get(const std::string& key);
    void put(const std::string& key, const std::string& value);
    bool erase(const std::string& key);
    Segment& segment(const std::string& key);

private:
    std::vector<Segment> segments;
};
