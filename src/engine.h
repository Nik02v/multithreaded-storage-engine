#pragma once

#include "table.h"
#include "wal.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

struct EngineOptions {
    std::size_t segments = 16;
    std::size_t batch_size = 64;
    std::chrono::microseconds batch_wait{1000};
    std::function<void(std::size_t)> before_sync;
};

class StorageEngine {
public:
    explicit StorageEngine(const std::filesystem::path& path, EngineOptions options = {});
    ~StorageEngine();
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    std::optional<std::string> get(const std::string& key);
    void put(const std::string& key, const std::string& value);
    bool erase(const std::string& key);
    void close();
    const RecoveryInfo& recovery_info() const { return recovered; }

private:
    struct Request {
        WalRecord record;
        std::promise<void> done;
    };
    void check_open();
    std::future<void> enqueue(Operation op, const std::string& key, const std::string& value);
    void writer_loop();
    void fail(std::exception_ptr error);

    EngineOptions options;
    MemoryTable table;
    WriteAheadLog wal;
    RecoveryInfo recovered;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Request>> queue;
    std::exception_ptr failure;
    std::uint64_t next_sequence = 1;
    bool closing = false;
    bool closed = false;
    std::thread writer;
};
