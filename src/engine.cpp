#include "engine.h"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
EngineOptions check_options(EngineOptions options)
{
    if (options.segments == 0 || options.segments > 1024 || options.batch_size == 0 ||
        options.batch_size > 4096 || options.batch_wait.count() < 0 ||
        options.batch_wait > std::chrono::seconds(5))
        throw std::invalid_argument("Neispravna podesavanja engine-a");
    return options;
}
}

StorageEngine::StorageEngine(const std::filesystem::path& path, EngineOptions config)
    : options(check_options(std::move(config))), table(options.segments), wal(path)
{
    recovered = wal.recover([this](const WalRecord& record) {
        if (record.operation == Operation::PUT)
            table.put(record.key, record.value);
        else
            table.erase(record.key);
    });
    if (recovered.last_sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Potroseni su redni brojevi WAL-a");
    next_sequence = recovered.last_sequence + 1;
    writer = std::thread(&StorageEngine::writer_loop, this);
}

StorageEngine::~StorageEngine()
{
    try { close(); } catch (...) { }
}

void StorageEngine::check_open()
{
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (failure)
        std::rethrow_exception(failure);
    if (closing || closed)
        throw std::logic_error("Engine je zatvoren");
}

std::optional<std::string> StorageEngine::get(const std::string& key)
{
    check_key(key);
    auto& segment = table.segment(key);
    std::shared_lock<std::shared_mutex> lock(segment.mutex);
    check_open();
    auto it = segment.values.find(key);
    if (it == segment.values.end())
        return std::nullopt;
    return it->second;
}

void StorageEngine::put(const std::string& key, const std::string& value)
{
    check_key(key);
    check_value(value);
    auto& segment = table.segment(key);
    std::unique_lock<std::shared_mutex> lock(segment.mutex);
    auto done = enqueue(Operation::PUT, key, value);
    done.get();
    try {
        segment.values[key] = value;
    } catch (...) {
        fail(std::current_exception());
        throw;
    }
}

bool StorageEngine::erase(const std::string& key)
{
    check_key(key);
    auto& segment = table.segment(key);
    std::unique_lock<std::shared_mutex> lock(segment.mutex);
    check_open();
    auto it = segment.values.find(key);
    if (it == segment.values.end())
        return false;
    auto done = enqueue(Operation::ERASE, key, "");
    done.get();
    segment.values.erase(it);
    return true;
}

std::future<void> StorageEngine::enqueue(Operation op, const std::string& key, const std::string& value)
{
    auto request = std::make_shared<Request>();
    request->record.operation = op;
    request->record.key = key;
    request->record.value = value;
    auto future = request->done.get_future();
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (failure)
            std::rethrow_exception(failure);
        if (closing || closed)
            throw std::logic_error("Engine je zatvoren");
        if (next_sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("Potroseni su redni brojevi WAL-a");
        request->record.sequence = next_sequence;
        queue.push_back(request);
        ++next_sequence;
    }
    cv.notify_one();
    return future;
}

void StorageEngine::fail(std::exception_ptr error)
{
    std::deque<std::shared_ptr<Request>> pending;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (!failure)
            failure = error;
        error = failure;
        closing = true;
        pending.swap(queue);
    }
    cv.notify_all();
    for (auto& request : pending)
        request->done.set_exception(error);
}

void StorageEngine::writer_loop()
{
    std::vector<std::shared_ptr<Request>> batch;
    try {
        batch.reserve(options.batch_size);
        while (true) {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                cv.wait(lock, [this] { return closing || !queue.empty(); });
                if (failure || (closing && queue.empty()))
                    return;
                auto deadline = std::chrono::steady_clock::now() + options.batch_wait;
                cv.wait_until(lock, deadline, [this] { return closing || queue.size() >= options.batch_size; });
                if (failure)
                    return;
                while (!queue.empty() && batch.size() < options.batch_size) {
                    batch.push_back(std::move(queue.front()));
                    queue.pop_front();
                }
            }

            for (const auto& request : batch)
                wal.append(request->record);
            if (options.before_sync)
                options.before_sync(batch.size());
            wal.sync();
            for (auto& request : batch)
                request->done.set_value();
            batch.clear();
        }
    } catch (...) {
        auto error = std::current_exception();
        fail(error);
        for (auto& request : batch)
            request->done.set_exception(error);
    }
}

void StorageEngine::close()
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (closed) {
            if (failure)
                std::rethrow_exception(failure);
            return;
        }
        closing = true;
    }
    cv.notify_all();
    if (writer.joinable())
        writer.join();
    try {
        wal.close();
    } catch (...) {
        fail(std::current_exception());
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    closed = true;
    if (failure)
        std::rethrow_exception(failure);
}
