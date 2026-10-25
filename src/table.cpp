#include "table.h"

#include <functional>
#include <mutex>
#include <stdexcept>

namespace {
std::size_t checked_count(std::size_t count)
{
    if (count == 0 || count > 1024)
        throw std::invalid_argument("Broj segmenata mora biti 1..1024");
    return count;
}
}

void check_key(const std::string& key)
{
    if (key.size() > max_key_size)
        throw std::invalid_argument("Kljuc je duzi od 256 bajtova");
}

void check_value(const std::string& value)
{
    if (value.size() > max_value_size)
        throw std::invalid_argument("Vrednost je duza od 4096 bajtova");
}

MemoryTable::MemoryTable(std::size_t count) : segments(checked_count(count)) {}

Segment& MemoryTable::segment(const std::string& key)
{
    std::size_t index = std::hash<std::string>{}(key) % segments.size();
    return segments[index];
}

std::optional<std::string> MemoryTable::get(const std::string& key)
{
    check_key(key);
    auto& s = segment(key);
    std::shared_lock<std::shared_mutex> lock(s.mutex);
    auto it = s.values.find(key);
    if (it == s.values.end())
        return std::nullopt;
    return it->second;
}

void MemoryTable::put(const std::string& key, const std::string& value)
{
    check_key(key);
    check_value(value);
    auto& s = segment(key);
    std::unique_lock<std::shared_mutex> lock(s.mutex);
    s.values[key] = value;
}

bool MemoryTable::erase(const std::string& key)
{
    check_key(key);
    auto& s = segment(key);
    std::unique_lock<std::shared_mutex> lock(s.mutex);
    return s.values.erase(key) != 0;
}
