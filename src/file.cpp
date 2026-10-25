#include "file.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
std::system_error io_error(const char* what)
{
    return std::system_error(errno, std::generic_category(), what);
}

long long checked_offset(std::uint64_t n)
{
    if (n > static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
        throw std::runtime_error("WAL je prevelik za ovaj file offset");
    return static_cast<long long>(n);
}
}

File::File(const std::filesystem::path& path)
{
#ifdef _WIN32
    int error = _wsopen_s(&fd, path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY,
                         _SH_DENYRW, _S_IREAD | _S_IWRITE);
    if (error != 0)
        throw std::system_error(error, std::generic_category(), "Otvaranje WAL-a");
#else
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        throw io_error("open WAL");
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        auto error = io_error("WAL vec koristi drugi engine");
        ::close(fd);
        fd = -1;
        throw error;
    }
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        auto error = io_error("open WAL directory");
        ::close(fd);
        fd = -1;
        throw error;
    }
    int result;
    do { result = ::fsync(directory); } while (result < 0 && errno == EINTR);
    int saved_error = errno;
    ::close(directory);
    if (result < 0) {
        ::close(fd);
        fd = -1;
        throw std::system_error(saved_error, std::generic_category(), "fsync directory");
    }
#endif
}

File::~File()
{
    try { close(); } catch (...) { }
}

std::uint64_t File::size() const
{
#ifdef _WIN32
    struct _stat64 info{};
    if (_fstat64(fd, &info) != 0)
#else
    struct stat info{};
    if (::fstat(fd, &info) != 0)
#endif
        throw io_error("fstat WAL");
    if (info.st_size < 0)
        throw std::runtime_error("Negativna velicina WAL-a");
    return static_cast<std::uint64_t>(info.st_size);
}

void File::seek(std::uint64_t offset)
{
    auto n = checked_offset(offset);
#ifdef _WIN32
    if (_lseeki64(fd, n, SEEK_SET) < 0)
#else
    if (::lseek(fd, static_cast<off_t>(n), SEEK_SET) < 0)
#endif
        throw io_error("seek WAL");
}

void File::read_exact(char* bytes, std::size_t count)
{
    std::size_t done = 0;
    while (done < count) {
        unsigned int chunk = static_cast<unsigned int>(std::min<std::size_t>(count - done, 1024 * 1024));
#ifdef _WIN32
        int n = _read(fd, bytes + done, chunk);
#else
        auto n = ::read(fd, bytes + done, chunk);
#endif
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw io_error("read WAL");
        if (n == 0)
            throw std::runtime_error("Neocekivan kraj WAL-a");
        done += static_cast<std::size_t>(n);
    }
}

void File::write_all(const char* bytes, std::size_t count)
{
    std::size_t done = 0;
    while (done < count) {
        unsigned int chunk = static_cast<unsigned int>(std::min<std::size_t>(count - done, 1024 * 1024));
#ifdef _WIN32
        int n = _write(fd, bytes + done, chunk);
#else
        auto n = ::write(fd, bytes + done, chunk);
#endif
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw io_error("write WAL");
        if (n == 0)
            throw std::runtime_error("WAL write je vratio nula bajtova");
        done += static_cast<std::size_t>(n);
    }
}

void File::truncate(std::uint64_t length)
{
    auto n = checked_offset(length);
#ifdef _WIN32
    int error = _chsize_s(fd, n);
    if (error != 0)
        throw std::system_error(error, std::generic_category(), "truncate WAL");
#else
    if (::ftruncate(fd, static_cast<off_t>(n)) != 0)
        throw io_error("ftruncate WAL");
#endif
}

void File::sync()
{
    int result;
    do {
#ifdef _WIN32
        result = _commit(fd);
#else
        result = ::fdatasync(fd);
#endif
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        throw io_error("sync WAL");
}

void File::close()
{
    if (fd < 0)
        return;
    int old = fd;
    fd = -1;
#ifdef _WIN32
    if (_close(old) != 0)
#else
    if (::close(old) != 0)
#endif
        throw io_error("close WAL");
}
