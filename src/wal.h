#pragma once

#include "file.h"

#include <cstdint>
#include <functional>
#include <string>

enum class Operation : unsigned char { PUT = 1, ERASE = 2 };

struct WalRecord {
    std::uint64_t sequence = 0;
    Operation operation = Operation::PUT;
    std::string key;
    std::string value;
};

struct RecoveryInfo {
    std::uint64_t last_sequence = 0;
    std::uint64_t records = 0;
    std::uint64_t truncated_bytes = 0;
};

class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::filesystem::path& path) : file(path) {}
    RecoveryInfo recover(const std::function<void(const WalRecord&)>& apply);
    void append(const WalRecord& record);
    void sync() { file.sync(); }
    void close() { file.close(); }

private:
    File file;
};
