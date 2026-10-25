#include "cli.h"
#include "engine.h"
#include "thread_pool.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

struct Options {
    int operations = 5000;
    int keys = 1000;
    int seed = 12345;
    int batch_wait_us = 1000;
    std::string mode = "all";
    std::string output = "benchmark.csv";
    std::string wal_dir = "bench-data";
    std::string cpu_label;
    std::string disk_label = "unknown";
    int physical_cores = 0;
};

struct BenchOp {
    bool write;
    std::string key;
    std::string value;
};

std::vector<BenchOp> generate(const Options& options, int write_percent, bool hot)
{
    std::mt19937 rng(options.seed);
    std::vector<BenchOp> ops;
    ops.reserve(options.operations);
    for (int i = 0; i < options.operations; ++i) {
        int key;
        if (!hot)
            key = static_cast<int>(rng() % options.keys);
        else if (rng() % 100 < 80)
            key = static_cast<int>(rng() % 10);
        else
            key = 10 + static_cast<int>(rng() % (options.keys - 10));
        ops.push_back({i < options.operations * write_percent / 100,
                       "k" + std::to_string(key), "value-" + std::to_string(i)});
    }
    std::shuffle(ops.begin(), ops.end(), rng);
    return ops;
}

template<class Store>
double measure(Store& store, const std::vector<BenchOp>& ops, int count)
{
    ThreadPool pool(count);
    std::mutex mutex;
    std::condition_variable cv;
    int waiting = 0, finished = 0;
    bool start = false;
    std::exception_ptr failure;
    for (int t = 0; t < count; ++t) {
        pool.submit([&, t] {
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++waiting;
                cv.notify_all();
                cv.wait(lock, [&] { return start; });
            }
            try {
                for (std::size_t i = t; i < ops.size(); i += count) {
                    const auto& op = ops[i];
                    if (op.write)
                        store.put(op.key, op.value);
                    else if (!store.get(op.key))
                        throw std::runtime_error("Benchmark je izgubio pocetni kljuc");
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!failure)
                    failure = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++finished;
            }
            cv.notify_all();
        });
    }
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return waiting == count; });
    auto begin = std::chrono::steady_clock::now();
    start = true;
    cv.notify_all();
    cv.wait(lock, [&] { return finished == count; });
    auto end = std::chrono::steady_clock::now();
    lock.unlock();
    pool.close();
    if (failure)
        std::rethrow_exception(failure);
    return std::chrono::duration<double>(end - begin).count();
}

template<class Store>
void prefill(Store& store, int count)
{
    for (int i = 0; i < count; ++i)
        store.put("k" + std::to_string(i), "initial-value");
}

void prepare_wal(const std::filesystem::path& path, int count)
{
    WriteAheadLog wal(path);
    for (int i = 0; i < count; ++i)
        wal.append({static_cast<std::uint64_t>(i + 1), Operation::PUT,
                    "k" + std::to_string(i), "initial-value"});
    wal.sync();
    wal.close();
}

std::string cpu_name()
{
#ifdef _WIN32
    const char* name = std::getenv("PROCESSOR_IDENTIFIER");
    return name ? name : "unknown";
#else
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("model name") == 0) {
            auto colon = line.find(':');
            if (colon != std::string::npos)
                return line.substr(colon + 2);
        }
    }
    return "unknown";
#endif
}

void metadata(const Options& options, const std::filesystem::path& path)
{
    std::ofstream out(options.output + ".meta.txt");
    if (!out)
        throw std::runtime_error("Ne mogu da otvorim benchmark metadata fajl");
    out << "cpu=" << (options.cpu_label.empty() ? cpu_name() : options.cpu_label) << '\n';
    out << "physical_cores=" << (options.physical_cores ? std::to_string(options.physical_cores) : "unknown") << '\n';
    out << "logical_threads=" << std::thread::hardware_concurrency() << '\n';
    out << "disk=" << options.disk_label << "\nwal_directory=" << path.string() << '\n';
#ifdef _WIN32
    out << "os=Windows\ndurability=_commit\n";
#else
    out << "os=Linux\ndurability=fdatasync\n";
#endif
#ifdef NDEBUG
    out << "build=Release\n";
#else
    out << "build=Debug\n";
#endif
#ifdef __VERSION__
    out << "compiler=" << __VERSION__ << '\n';
#else
    out << "compiler=MSVC " << _MSC_VER << '\n';
#endif
    out << "operations=" << options.operations << "\nkeys=" << options.keys << "\nseed=" << options.seed
        << "\nrepetitions=3\nbatch_size=64\nbatch_wait_us=" << options.batch_wait_us << '\n';
    out.close();
    if (!out)
        throw std::runtime_error("Neuspesno pisanje benchmark metadata");
}

int main(int argc, char** argv)
{
    try {
        Options options;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--ops") options.operations = cli_int(next_arg(i, argc, argv), 100, 10000000);
            else if (arg == "--keys") options.keys = cli_int(next_arg(i, argc, argv), 11, 1000000);
            else if (arg == "--seed") options.seed = cli_int(next_arg(i, argc, argv), 0, 1000000000);
            else if (arg == "--batch-wait-us") options.batch_wait_us = cli_int(next_arg(i, argc, argv), 0, 5000000);
            else if (arg == "--mode") options.mode = next_arg(i, argc, argv);
            else if (arg == "--out") options.output = next_arg(i, argc, argv);
            else if (arg == "--wal-dir") options.wal_dir = next_arg(i, argc, argv);
            else if (arg == "--cpu-label") options.cpu_label = next_arg(i, argc, argv);
            else if (arg == "--disk-label") options.disk_label = next_arg(i, argc, argv);
            else if (arg == "--physical-cores") options.physical_cores = cli_int(next_arg(i, argc, argv), 1, 1024);
            else if (arg == "--help") {
                std::cout << "storage_bench [--ops 5000] [--keys 1000] [--seed 12345]\n"
                             "  [--mode all|memory|durable] [--out benchmark.csv] [--wal-dir bench-data]\n"
                             "  [--batch-wait-us 1000] [--cpu-label text] [--disk-label text] [--physical-cores N]\n";
                return 0;
            } else throw std::invalid_argument("Nepoznat argument: " + arg);
        }
        if (options.mode != "all" && options.mode != "memory" && options.mode != "durable")
            throw std::invalid_argument("mode mora biti all, memory ili durable");

        auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        auto directory = std::filesystem::absolute(options.wal_dir) / ("run-" + std::to_string(ticks));
        if (!std::filesystem::create_directories(directory))
            throw std::runtime_error("Benchmark WAL direktorijum vec postoji");
        std::ofstream summary(options.output);
        std::ofstream raw(options.output + ".raw.csv");
        if (!summary || !raw)
            throw std::runtime_error("Ne mogu da otvorim CSV izlaz");
        summary << "mode,distribution,write_percent,segments,threads,operations,median_seconds,ops_per_second\n";
        raw << "mode,distribution,write_percent,segments,threads,repetition,operations,seconds,ops_per_second\n";
        summary << std::fixed << std::setprecision(9);
        raw << std::fixed << std::setprecision(9);
        metadata(options, directory);
        int run = 0;
        for (const std::string mode : {"memory", "durable"}) {
            if (options.mode != "all" && options.mode != mode)
                continue;
            for (bool hot : {false, true}) {
                for (int write_percent : {0, 5, 50}) {
                    auto operations = generate(options, write_percent, hot);
                    for (int segments : {1, 16}) {
                        for (int threads : {1, 2, 4, 8}) {
                            std::vector<double> times;
                            for (int rep = 1; rep <= 3; ++rep) {
                                double seconds;
                                if (mode == "memory") {
                                    MemoryTable table(segments);
                                    prefill(table, options.keys);
                                    seconds = measure(table, operations, threads);
                                } else {
                                    EngineOptions config;
                                    config.segments = segments;
                                    config.batch_wait = std::chrono::microseconds(options.batch_wait_us);
                                    auto path = directory / (std::to_string(++run) + ".wal");
                                    prepare_wal(path, options.keys);
                                    StorageEngine engine(path, config);
                                    seconds = measure(engine, operations, threads);
                                    engine.close();
                                }
                                times.push_back(seconds);
                                raw << mode << ',' << (hot ? "hot" : "uniform") << ',' << write_percent
                                    << ',' << segments << ',' << threads << ',' << rep << ',' << options.operations
                                    << ',' << seconds << ',' << options.operations / seconds << '\n';
                            }
                            std::sort(times.begin(), times.end());
                            double median = times[1];
                            summary << mode << ',' << (hot ? "hot" : "uniform") << ',' << write_percent
                                    << ',' << segments << ',' << threads << ',' << options.operations
                                    << ',' << median << ',' << options.operations / median << '\n';
                            std::cout << mode << ' ' << (hot ? "hot" : "uniform") << " write=" << write_percent
                                      << " segments=" << segments << " threads=" << threads
                                      << " median=" << median << " s" << std::endl;
                        }
                    }
                }
            }
        }
        summary.close();
        raw.close();
        if (!summary || !raw)
            throw std::runtime_error("Neuspesno pisanje CSV-a");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "benchmark: " << e.what() << std::endl;
        return 2;
    }
}
