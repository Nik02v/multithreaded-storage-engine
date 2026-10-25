#pragma once

#include <stdexcept>
#include <string>

inline std::string next_arg(int& i, int argc, char** argv)
{
    if (i + 1 >= argc)
        throw std::invalid_argument(std::string("Nedostaje vrednost za ") + argv[i]);
    return argv[++i];
}

inline int cli_int(const std::string& value, int low, int high)
{
    std::size_t end = 0;
    int n = std::stoi(value, &end);
    if (end != value.size() || n < low || n > high)
        throw std::invalid_argument("Broj van dozvoljenog opsega: " + value);
    return n;
}
