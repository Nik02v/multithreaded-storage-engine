#include "engine.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #expr); } while (false)

fs::path test_root;
int file_number = 0;
fs::path next_file() { return test_root / (std::to_string(++file_number) + ".wal"); }

EngineOptions fast(std::size_t segments = 16)
{
    EngineOptions options;
    options.segments = segments;
    options.batch_wait = 0us;
    return options;
}

template<class F>
void rejected(F f)
{
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    CHECK(caught);
}

template<class T>
T ready(std::future<T>& f)
{
    CHECK(f.wait_for(4s) == std::future_status::ready);
    return f.get();
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_file(const fs::path& path, const std::string& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    CHECK(out.good());
}

void basic_and_reopen()
{
    auto path = next_file();
    {
        StorageEngine engine(path, fast(1));
        CHECK(!engine.get("missing"));
        CHECK(!engine.erase("missing"));
        engine.put("key", "first");
        engine.put("key", "second");
        CHECK(engine.get("key") == "second");
        auto copy = engine.get("key");
        *copy = "local copy";
        CHECK(engine.get("key") == "second");
        engine.put("removed", "value");
        CHECK(engine.erase("removed"));
        CHECK(!engine.erase("removed"));
        engine.put("", "");
        engine.put(std::string("a\0b", 3), std::string("x\0y", 3));
        engine.close();
        engine.close();
        rejected([&] { engine.get("key"); });
        rejected([&] { engine.put("key", "third"); });
        rejected([&] { engine.erase("key"); });
    }
    {
        StorageEngine engine(path, fast(64));
        CHECK(engine.recovery_info().records == 6);
        CHECK(engine.get("key") == "second");
        CHECK(!engine.get("removed"));
        CHECK(engine.get("") == "");
        CHECK(engine.get(std::string("a\0b", 3)) == std::string("x\0y", 3));
        engine.put("after restart", "works");
    }
    StorageEngine again(path, fast(4));
    CHECK(again.recovery_info().last_sequence == 7);
    CHECK(again.get("after restart") == "works");
}

void validation_and_file_lock()
{
    EngineOptions bad = fast();
    bad.segments = 0;
    rejected([&] { StorageEngine engine(next_file(), bad); });
    bad = fast();
    bad.batch_size = 0;
    rejected([&] { StorageEngine engine(next_file(), bad); });
    bad = fast();
    bad.batch_wait = -1us;
    rejected([&] { StorageEngine engine(next_file(), bad); });

    auto path = next_file();
    StorageEngine engine(path, fast());
    rejected([&] { StorageEngine other(path, fast()); });
    rejected([&] { engine.put(std::string(257, 'k'), "value"); });
    rejected([&] { engine.put("key", std::string(4097, 'v')); });
    rejected([&] { engine.get(std::string(257, 'k')); });
    rejected([&] { engine.erase(std::string(257, 'k')); });
    std::string key(256, 'k'), value(4096, 'v');
    engine.put(key, value);
    CHECK(engine.get(key) == value);
    engine.close();
    StorageEngine other(path, fast());
    CHECK(other.get(key) == value);
}

void reference_map()
{
    auto path = next_file();
    std::unordered_map<std::string, std::string> expected;
    {
        StorageEngine engine(path, fast());
        std::mt19937 rng(12345);
        for (int i = 0; i < 160; ++i) {
            std::string key = "k" + std::to_string(rng() % 20);
            int operation = rng() % 3;
            if (operation == 0) {
                std::string value = "v" + std::to_string(i);
                engine.put(key, value);
                expected[key] = value;
            } else if (operation == 1) {
                CHECK(engine.erase(key) == (expected.erase(key) != 0));
            } else {
                auto result = engine.get(key);
                auto it = expected.find(key);
                if (it == expected.end())
                    CHECK(!result);
                else
                    CHECK(result == it->second);
            }
        }
    }
    StorageEngine engine(path, fast(1));
    for (int i = 0; i < 20; ++i) {
        std::string key = "k" + std::to_string(i);
        auto it = expected.find(key);
        if (it == expected.end())
            CHECK(!engine.get(key));
        else
            CHECK(engine.get(key) == it->second);
    }
}

void concurrent_distinct_keys()
{
    auto path = next_file();
    {
        auto options = fast(32);
        options.batch_wait = 100us;
        StorageEngine engine(path, options);
        std::vector<std::future<void>> jobs;
        for (int t = 0; t < 8; ++t) {
            jobs.push_back(std::async(std::launch::async, [&, t] {
                for (int i = 0; i < 30; ++i) {
                    std::string key = std::to_string(t) + "-" + std::to_string(i);
                    engine.put(key, "value-" + key);
                    CHECK(engine.get(key) == "value-" + key);
                    if (i % 3 == 0)
                        CHECK(engine.erase(key));
                }
            }));
        }
        for (auto& job : jobs)
            ready(job);
    }
    StorageEngine engine(path, fast(4));
    for (int t = 0; t < 8; ++t) {
        for (int i = 0; i < 30; ++i) {
            std::string key = std::to_string(t) + "-" + std::to_string(i);
            if (i % 3 == 0)
                CHECK(!engine.get(key));
            else
                CHECK(engine.get(key) == "value-" + key);
        }
    }
}

std::vector<std::string> distinct_segments(std::size_t count, std::size_t needed)
{
    std::set<std::size_t> used;
    std::vector<std::string> keys;
    for (int i = 0; keys.size() < needed; ++i) {
        std::string key = "key-" + std::to_string(i);
        auto index = std::hash<std::string>{}(key) % count;
        if (used.insert(index).second)
            keys.push_back(key);
    }
    return keys;
}

void reader_waits_for_durable_writer()
{
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto options = fast(16);
    options.before_sync = [&](std::size_t) {
        entered.set_value();
        if (release_future.wait_for(3s) != std::future_status::ready)
            throw std::runtime_error("Test nije pustio sync");
    };
    StorageEngine engine(next_file(), options);
    auto keys = distinct_segments(16, 2);
    auto writer = std::async(std::launch::async, [&] { engine.put(keys[0], "new value"); });
    ready(entered_future);
    std::promise<void> reader_started;
    auto reader_started_future = reader_started.get_future();
    auto reader = std::async(std::launch::async, [&] {
        reader_started.set_value();
        return engine.get(keys[0]);
    });
    ready(reader_started_future);
    bool writer_blocked = writer.wait_for(0s) == std::future_status::timeout;
    bool reader_blocked = reader.wait_for(50ms) == std::future_status::timeout;
    bool other_missing = !engine.get(keys[1]);
    release.set_value();
    ready(writer);
    auto value = ready(reader);
    CHECK(writer_blocked && reader_blocked && other_missing);
    CHECK(value == "new value");
    CHECK(engine.get(keys[0]) == "new value");
}

void flush_failure_releases_waiters()
{
    auto path = next_file();
    std::promise<std::size_t> grouped;
    auto grouped_future = grouped.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto options = fast(64);
    options.batch_size = 8;
    options.batch_wait = 1s;
    options.before_sync = [&](std::size_t count) {
        grouped.set_value(count);
        if (release_future.wait_for(3s) != std::future_status::ready)
            throw std::runtime_error("Test nije pustio simuliranu gresku");
        throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "simuliran sync failure");
    };
    StorageEngine engine(path, options);
    auto keys = distinct_segments(64, 16);
    std::vector<std::future<bool>> jobs;
    auto start = [&](int i) {
        jobs.push_back(std::async(std::launch::async, [&, i] {
            try {
                engine.put(keys[i], "value");
            } catch (const std::system_error& e) {
                return e.code() == std::make_error_code(std::errc::no_space_on_device);
            }
            return false;
        }));
    };
    for (int i = 0; i < 8; ++i)
        start(i);
    std::size_t batch_count = ready(grouped_future);
    bool blocked = true;
    for (auto& job : jobs)
        blocked = blocked && job.wait_for(0s) == std::future_status::timeout;
    for (int i = 8; i < 16; ++i)
        start(i);
    release.set_value();
    for (auto& job : jobs)
        CHECK(ready(job));
    CHECK(batch_count == 8 && blocked);
    rejected([&] { engine.get(keys[0]); });
    rejected([&] { engine.put("later", "value"); });
    rejected([&] { engine.erase("missing"); });
    rejected([&] { engine.close(); });
    rejected([&] { engine.close(); });
    StorageEngine reopened(path, fast());
    CHECK(reopened.recovery_info().records == 8);
    for (int i = 0; i < 8; ++i)
        CHECK(reopened.get(keys[i]) == "value");
}

void partial_tail_at_every_byte()
{
    auto source = next_file();
    std::uint64_t first_size;
    {
        WriteAheadLog wal(source);
        wal.append({1, Operation::PUT, "k", "old"});
        wal.sync();
        first_size = fs::file_size(source);
        wal.append({2, Operation::PUT, "k", "new value"});
        wal.sync();
    }
    std::string bytes = read_file(source);
    auto last_size = bytes.size() - first_size;
    for (std::size_t cut = 0; cut < last_size; ++cut) {
        auto path = next_file();
        write_file(path, bytes.substr(0, first_size + cut));
        {
            StorageEngine engine(path, fast());
            CHECK(engine.get("k") == "old");
            CHECK(engine.recovery_info().records == 1);
            CHECK(engine.recovery_info().truncated_bytes == cut);
            CHECK(fs::file_size(path) == first_size);
            engine.put("after", "append");
        }
        StorageEngine reopened(path, fast(1));
        CHECK(reopened.get("k") == "old");
        CHECK(reopened.get("after") == "append");
        CHECK(reopened.recovery_info().last_sequence == 2);
    }
}

void corrupt_complete_records()
{
    auto path = next_file();
    {
        StorageEngine engine(path, fast());
        engine.put("k", "v");
    }
    auto bytes = read_file(path);
    bytes.back() ^= 0x55;
    write_file(path, bytes);
    rejected([&] { StorageEngine engine(path, fast()); });
    CHECK(read_file(path) == bytes);

    auto gap = next_file();
    {
        WriteAheadLog wal(gap);
        wal.append({1, Operation::PUT, "a", "one"});
        wal.append({3, Operation::PUT, "b", "three"});
        wal.sync();
    }
    rejected([&] { StorageEngine engine(gap, fast()); });
    auto bad_length = next_file();
    write_file(bad_length, std::string(4, '\0'));
    rejected([&] { StorageEngine engine(bad_length, fast()); });
}

void memory_component()
{
    MemoryTable table(16);
    table.put("k", "one");
    table.put("k", "two");
    CHECK(table.get("k") == "two");
    CHECK(table.erase("k"));
    CHECK(!table.erase("k"));
    CHECK(!table.get("k"));
    rejected([] { MemoryTable bad(0); });
}

int main()
{
    auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    test_root = fs::current_path() / ("unit-data-" + std::to_string(ticks));
    if (!fs::create_directory(test_root))
        return 2;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 25s, [&] { return done; })) {
            std::cerr << "Testovi su prekoracili 25 sekundi (moguc deadlock)\n";
            std::abort();
        }
    });
    const std::pair<const char*, void(*)()> tests[] = {
        {"basic operations and restart", basic_and_reopen},
        {"validation and exclusive WAL", validation_and_file_lock},
        {"reference unordered_map", reference_map},
        {"concurrent distinct keys", concurrent_distinct_keys},
        {"reader waits for durable writer", reader_waits_for_durable_writer},
        {"sync failure releases waiters", flush_failure_releases_waiters},
        {"partial tail at every byte", partial_tail_at_every_byte},
        {"complete corruption and sequence gap", corrupt_complete_records},
        {"memory component", memory_component}
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[OK] " << test.first << std::endl;
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": " << e.what() << std::endl;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
    }
    cv.notify_all();
    watchdog.join();
    for (const auto& entry : fs::directory_iterator(test_root))
        fs::remove(entry.path());
    fs::remove(test_root);
    return failures ? 1 : 0;
}
