#include "test_support.hpp"

#include "storage/range_release.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::size_t region_bytes = 64 * 1024;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("nuvio-engine-range-release-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<char> patterned_contents(const std::size_t regions) {
    std::vector<char> contents(regions * region_bytes);
    for (std::size_t index = 0; index < contents.size(); ++index) {
        contents[index] = static_cast<char>('a' + static_cast<char>(index / region_bytes));
    }
    return contents;
}

std::filesystem::path write_file(
    const std::filesystem::path& directory,
    const std::vector<char>& contents
) {
    const auto path = directory / "payload.bin";
    std::ofstream output(path, std::ios::binary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}

std::vector<char> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}

NUVIO_TEST("range release zeroes only the released bytes and keeps the file size") {
    TemporaryDirectory directory;
    const auto contents = patterned_contents(4);
    const auto path = write_file(directory.path(), contents);
    if (!nuvio::storage::supports_range_release(directory.path())) {
        const auto result = nuvio::storage::release_file_range(path, region_bytes, region_bytes);
        NUVIO_EXPECT_TRUE(result != nuvio::storage::RangeRelease::missing);
        return;
    }

    const auto before = nuvio::storage::allocated_file_bytes(path);
    NUVIO_EXPECT_EQ(
        nuvio::storage::release_file_range(path, region_bytes, 2 * region_bytes),
        nuvio::storage::RangeRelease::released
    );
    NUVIO_EXPECT_TRUE(nuvio::storage::allocated_file_bytes(path) < before);
    NUVIO_EXPECT_EQ(std::filesystem::file_size(path), std::uintmax_t(contents.size()));

    const auto released = read_file(path);
    NUVIO_EXPECT_EQ(released.size(), contents.size());
    for (std::size_t index = 0; index < released.size(); ++index) {
        const auto inside = index >= region_bytes && index < 3 * region_bytes;
        NUVIO_EXPECT_EQ(released[index], inside ? '\0' : contents[index]);
    }
}

NUVIO_TEST("range release reports a missing file") {
    TemporaryDirectory directory;
    NUVIO_EXPECT_EQ(
        nuvio::storage::release_file_range(directory.path() / "absent.bin", 0, region_bytes),
        nuvio::storage::RangeRelease::missing
    );
}

NUVIO_TEST("range release accepts an empty range without changing the file") {
    TemporaryDirectory directory;
    const auto contents = patterned_contents(2);
    const auto path = write_file(directory.path(), contents);
    NUVIO_EXPECT_EQ(
        nuvio::storage::release_file_range(path, region_bytes, 0),
        nuvio::storage::RangeRelease::released
    );
    NUVIO_EXPECT_TRUE(read_file(path) == contents);
}

NUVIO_TEST("range release rejects ranges past the largest file offset") {
    TemporaryDirectory directory;
    const auto path = write_file(directory.path(), patterned_contents(1));
    NUVIO_EXPECT_EQ(
        nuvio::storage::release_file_range(
            path,
            std::numeric_limits<std::uint64_t>::max() - 1,
            region_bytes
        ),
        nuvio::storage::RangeRelease::failed
    );
}

NUVIO_TEST("range release probe leaves no file behind") {
    TemporaryDirectory directory;
    static_cast<void>(nuvio::storage::supports_range_release(directory.path()));
    NUVIO_EXPECT_TRUE(std::filesystem::is_empty(directory.path()));
}
