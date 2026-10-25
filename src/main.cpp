#include "cli.h"
#include "engine.h"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
void no_extra(std::istringstream& in)
{
    in >> std::ws;
    if (!in.eof())
        throw std::invalid_argument("Visak argumenata komande");
}
}

int main(int argc, char** argv)
{
    try {
        std::string path;
        EngineOptions options;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--wal") path = next_arg(i, argc, argv);
            else if (arg == "--segments") options.segments = cli_int(next_arg(i, argc, argv), 1, 1024);
            else if (arg == "--batch-wait-us") options.batch_wait = std::chrono::microseconds(cli_int(next_arg(i, argc, argv), 0, 5000000));
            else if (arg == "--help") {
                std::cout << "storage_cli --wal data.wal [--segments 16] [--batch-wait-us 1000]\n"
                             "Komande: put \"key\" \"value\", get \"key\", erase \"key\", quit\n";
                return 0;
            } else throw std::invalid_argument("Nepoznat argument: " + arg);
        }
        if (path.empty())
            throw std::invalid_argument("Obavezan je --wal fajl");
        StorageEngine engine(path, options);
        std::cout << "READY records=" << engine.recovery_info().records
                  << " truncated=" << engine.recovery_info().truncated_bytes << std::endl;
        std::string line;
        while (std::getline(std::cin, line)) {
            try {
                std::istringstream in(line);
                std::string command, key, value;
                in >> command;
                if (command.empty())
                    continue;
                if (command == "quit") {
                    no_extra(in);
                    engine.close();
                    std::cout << "BYE" << std::endl;
                    return 0;
                }
                if (!(in >> std::quoted(key)))
                    throw std::invalid_argument("Nedostaje kljuc");
                if (command == "put") {
                    if (!(in >> std::quoted(value)))
                        throw std::invalid_argument("Nedostaje vrednost");
                    no_extra(in);
                    engine.put(key, value);
                    std::cout << "OK" << std::endl;
                } else if (command == "get") {
                    no_extra(in);
                    auto result = engine.get(key);
                    if (result)
                        std::cout << "VALUE " << std::quoted(*result) << std::endl;
                    else
                        std::cout << "NOT_FOUND" << std::endl;
                } else if (command == "erase") {
                    no_extra(in);
                    std::cout << (engine.erase(key) ? "ERASED" : "NOT_FOUND") << std::endl;
                } else {
                    throw std::invalid_argument("Nepoznata komanda");
                }
            } catch (const std::exception& e) {
                std::cout << "ERROR " << e.what() << std::endl;
            }
        }
        engine.close();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "storage: " << e.what() << std::endl;
        return 2;
    }
}
