#include "storage/range_release.hpp"

#include "security/random_bytes.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#elif defined(__linux__)
#include <linux/falloc.h>
#endif
#endif

namespace nuvio::storage {
namespace {

constexpr std::uint64_t probe_block_bytes = 64 * 1024;
constexpr std::uint64_t probe_file_bytes = 3 * probe_block_bytes;
constexpr std::uint64_t maximum_file_offset =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

bool range_fits(const std::uint64_t offset, const std::uint64_t length) {
    return offset <= maximum_file_offset && length <= maximum_file_offset - offset;
}

#if defined(_WIN32)

class FileHandle {
public:
    FileHandle(const std::filesystem::path& path, const DWORD access, const DWORD disposition)
        : value_(CreateFileW(
              path.c_str(),
              access,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
              nullptr,
              disposition,
              FILE_ATTRIBUTE_NORMAL,
              nullptr
          )) {
    }

    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const {
        return value_;
    }

    [[nodiscard]] bool is_open() const {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_;
};

bool unsupported_error(const DWORD error) {
    return error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_USER_MAPPED_FILE;
}

bool write_probe(const std::filesystem::path& path) {
    const FileHandle file(path, GENERIC_WRITE, CREATE_NEW);
    if (!file.is_open()) {
        return false;
    }
    const std::vector<char> contents(static_cast<std::size_t>(probe_file_bytes), 'n');
    DWORD written = 0;
    return WriteFile(
               file.get(),
               contents.data(),
               static_cast<DWORD>(contents.size()),
               &written,
               nullptr
           ) &&
        written == contents.size() && FlushFileBuffers(file.get());
}

#else

class Descriptor {
public:
    Descriptor(const std::filesystem::path& path, const int flags, const mode_t mode = 0) {
        do {
            value_ = ::open(path.c_str(), flags | O_CLOEXEC, mode);
        } while (value_ < 0 && errno == EINTR);
    }

    ~Descriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    [[nodiscard]] int get() const {
        return value_;
    }

    [[nodiscard]] bool is_open() const {
        return value_ >= 0;
    }

private:
    int value_ = -1;
};

#if (defined(__APPLE__) && defined(F_PUNCHHOLE)) || defined(__linux__)
bool unsupported_error(const int error) {
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
    if (error == ENOTSUP) {
        return true;
    }
#endif
    return error == EOPNOTSUPP || error == ENOSYS;
}

RangeRelease release_result(const int result) {
    if (result == 0) {
        return RangeRelease::released;
    }
    return unsupported_error(errno) ? RangeRelease::unsupported : RangeRelease::failed;
}
#endif

RangeRelease release_descriptor_range(
    const int descriptor,
    const std::uint64_t offset,
    const std::uint64_t length
) {
#if defined(__APPLE__) && defined(F_PUNCHHOLE)
    struct statfs filesystem {};
    if (::fstatfs(descriptor, &filesystem) != 0) {
        return RangeRelease::failed;
    }
    const auto block = filesystem.f_bsize > 0
        ? static_cast<std::uint64_t>(filesystem.f_bsize)
        : std::uint64_t{4096};
    const auto start = (offset + block - 1) / block * block;
    const auto end = (offset + length) / block * block;
    if (end <= start) {
        return RangeRelease::released;
    }
    fpunchhole_t hole{};
    hole.fp_offset = static_cast<off_t>(start);
    hole.fp_length = static_cast<off_t>(end - start);
    int result = 0;
    do {
        result = ::fcntl(descriptor, F_PUNCHHOLE, &hole);
    } while (result != 0 && errno == EINTR);
    return release_result(result);
#elif defined(__linux__)
    int result = 0;
    do {
        result = ::fallocate64(
            descriptor,
            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
            static_cast<off64_t>(offset),
            static_cast<off64_t>(length)
        );
    } while (result != 0 && errno == EINTR);
    return release_result(result);
#else
    static_cast<void>(descriptor);
    static_cast<void>(offset);
    static_cast<void>(length);
    return RangeRelease::unsupported;
#endif
}

bool write_probe(const std::filesystem::path& path) {
    const Descriptor file(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (!file.is_open()) {
        return false;
    }
    const std::vector<char> contents(static_cast<std::size_t>(probe_file_bytes), 'n');
    std::size_t done = 0;
    while (done < contents.size()) {
        const auto written = ::write(file.get(), contents.data() + done, contents.size() - done);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        done += static_cast<std::size_t>(written);
    }
    return ::fsync(file.get()) == 0;
}

#endif

}

RangeRelease release_file_range(
    const std::filesystem::path& path,
    const std::uint64_t offset,
    const std::uint64_t length
) {
    if (!range_fits(offset, length)) {
        return RangeRelease::failed;
    }
#if defined(_WIN32)
    const FileHandle file(path, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    if (!file.is_open()) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? RangeRelease::missing
            : RangeRelease::failed;
    }
    if (length == 0) {
        return RangeRelease::released;
    }
    DWORD returned = 0;
    if (!DeviceIoControl(file.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
        return unsupported_error(GetLastError()) ? RangeRelease::unsupported : RangeRelease::failed;
    }
    FILE_ZERO_DATA_INFORMATION zero{};
    zero.FileOffset.QuadPart = static_cast<LONGLONG>(offset);
    zero.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(offset + length);
    if (!DeviceIoControl(
            file.get(),
            FSCTL_SET_ZERO_DATA,
            &zero,
            sizeof(zero),
            nullptr,
            0,
            &returned,
            nullptr
        )) {
        return unsupported_error(GetLastError()) ? RangeRelease::unsupported : RangeRelease::failed;
    }
    return RangeRelease::released;
#else
    const Descriptor file(path, O_WRONLY);
    if (!file.is_open()) {
        return errno == ENOENT ? RangeRelease::missing : RangeRelease::failed;
    }
    if (length == 0) {
        return RangeRelease::released;
    }
    return release_descriptor_range(file.get(), offset, length);
#endif
}

std::uint64_t allocated_file_bytes(const std::filesystem::path& path) {
#if defined(_WIN32)
    const FileHandle file(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING);
    if (!file.is_open()) {
        return 0;
    }
    FILE_STANDARD_INFO information{};
    if (!GetFileInformationByHandleEx(
            file.get(),
            FileStandardInfo,
            &information,
            sizeof(information)
        )) {
        return 0;
    }
    return information.AllocationSize.QuadPart > 0
        ? static_cast<std::uint64_t>(information.AllocationSize.QuadPart)
        : 0;
#else
    struct stat information {};
    if (::stat(path.c_str(), &information) != 0 || information.st_blocks <= 0) {
        return 0;
    }
    constexpr std::uint64_t stat_block_bytes = 512;
    const auto blocks = static_cast<std::uint64_t>(information.st_blocks);
    return blocks > std::numeric_limits<std::uint64_t>::max() / stat_block_bytes
        ? std::numeric_limits<std::uint64_t>::max()
        : blocks * stat_block_bytes;
#endif
}

bool supports_range_release(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }
    const auto path = directory / (".nuvio-range-probe-" + security::random_hex_token(8));
    auto supported = false;
    try {
        if (write_probe(path)) {
            const auto before = allocated_file_bytes(path);
            supported = before > 0 &&
                release_file_range(path, probe_block_bytes, probe_block_bytes) ==
                    RangeRelease::released &&
                allocated_file_bytes(path) < before;
        }
    } catch (...) {
        supported = false;
    }
    std::filesystem::remove(path, error);
    return supported;
}

}
