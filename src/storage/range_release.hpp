#ifndef NUVIO_ENGINE_RANGE_RELEASE_HPP
#define NUVIO_ENGINE_RANGE_RELEASE_HPP

#include <cstdint>
#include <filesystem>

namespace nuvio::storage {

enum class RangeRelease {
    released,
    missing,
    unsupported,
    failed,
};

[[nodiscard]] RangeRelease release_file_range(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::uint64_t length
);

[[nodiscard]] std::uint64_t allocated_file_bytes(const std::filesystem::path& path);

[[nodiscard]] bool supports_range_release(const std::filesystem::path& directory);

}

#endif
