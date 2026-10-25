#include "wal.h"
#include "table.h"

#include <limits>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uint32_t min_record = 21;
constexpr std::uint32_t max_record = min_record + max_key_size + max_value_size;

void add32(std::vector<char>& bytes, std::uint32_t n)
{
    for (int i = 0; i < 4; ++i)
        bytes.push_back(static_cast<char>((n >> (i * 8)) & 255));
}

void add64(std::vector<char>& bytes, std::uint64_t n)
{
    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<char>((n >> (i * 8)) & 255));
}

std::uint32_t get32(const char* bytes)
{
    std::uint32_t n = 0;
    for (int i = 0; i < 4; ++i)
        n |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
    return n;
}

std::uint64_t get64(const char* bytes)
{
    std::uint64_t n = 0;
    for (int i = 0; i < 8; ++i)
        n |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
    return n;
}

std::uint32_t crc32(const char* bytes, std::size_t size)
{
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned char>(bytes[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320U : crc >> 1;
    }
    return ~crc;
}

WalRecord decode(const std::vector<char>& bytes)
{
    if (crc32(bytes.data(), bytes.size() - 4) != get32(bytes.data() + bytes.size() - 4))
        throw std::runtime_error("WAL: neispravna CRC32 suma kompletnog zapisa");
    std::uint32_t key_size = get32(bytes.data() + 9);
    std::uint32_t value_size = get32(bytes.data() + 13);
    if (key_size > max_key_size || value_size > max_value_size ||
        bytes.size() != static_cast<std::size_t>(min_record) + key_size + value_size)
        throw std::runtime_error("WAL: neispravne duzine kljuca/vrednosti");

    WalRecord record;
    record.sequence = get64(bytes.data());
    record.operation = static_cast<Operation>(static_cast<unsigned char>(bytes[8]));
    if (record.operation != Operation::PUT && record.operation != Operation::ERASE)
        throw std::runtime_error("WAL: nepoznata operacija");
    if (record.operation == Operation::ERASE && value_size != 0)
        throw std::runtime_error("WAL: ERASE ne sme imati vrednost");
    record.key.assign(bytes.data() + 17, key_size);
    record.value.assign(bytes.data() + 17 + key_size, value_size);
    return record;
}
}

void WriteAheadLog::append(const WalRecord& record)
{
    check_key(record.key);
    check_value(record.value);
    if (record.sequence == 0 || (record.operation != Operation::PUT && record.operation != Operation::ERASE) ||
        (record.operation == Operation::ERASE && !record.value.empty()))
        throw std::invalid_argument("Neispravan WAL zapis");

    std::vector<char> body;
    body.reserve(min_record + record.key.size() + record.value.size());
    add64(body, record.sequence);
    body.push_back(static_cast<char>(record.operation));
    add32(body, static_cast<std::uint32_t>(record.key.size()));
    add32(body, static_cast<std::uint32_t>(record.value.size()));
    body.insert(body.end(), record.key.begin(), record.key.end());
    body.insert(body.end(), record.value.begin(), record.value.end());
    add32(body, crc32(body.data(), body.size()));

    std::vector<char> prefix;
    add32(prefix, static_cast<std::uint32_t>(body.size()));
    file.write_all(prefix.data(), prefix.size());
    file.write_all(body.data(), body.size());
}

RecoveryInfo WriteAheadLog::recover(const std::function<void(const WalRecord&)>& apply)
{
    RecoveryInfo info;
    std::uint64_t length = file.size();
    std::uint64_t offset = 0;
    file.seek(0);

    while (offset < length) {
        if (length - offset < 4)
            break;
        char prefix[4];
        file.read_exact(prefix, sizeof(prefix));
        auto size = get32(prefix);
        if (size < min_record || size > max_record)
            throw std::runtime_error("WAL: neispravna duzina zapisa");
        if (length - offset - 4 < size)
            break;

        std::vector<char> bytes(size);
        file.read_exact(bytes.data(), bytes.size());
        WalRecord record = decode(bytes);
        if (info.last_sequence == std::numeric_limits<std::uint64_t>::max() ||
            record.sequence != info.last_sequence + 1)
            throw std::runtime_error("WAL: redni brojevi nisu uzastopni");
        apply(record);
        info.last_sequence = record.sequence;
        ++info.records;
        offset += 4 + size;
    }

    if (offset < length) {
        info.truncated_bytes = length - offset;
        file.truncate(offset);
        file.sync();
    }
    file.seek(offset);
    return info;
}
