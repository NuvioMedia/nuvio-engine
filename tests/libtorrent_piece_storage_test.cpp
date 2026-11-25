#include "test_support.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/aux_/posix_storage.hpp>
#include <libtorrent/aux_/session_settings.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session_handle.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>

namespace {

class PieceDirectory {
public:
    PieceDirectory()
        : path((std::filesystem::temp_directory_path() / ("nuvio-piece-storage-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string()) {}

    ~PieceDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    const std::string path;
};

}

NUVIO_TEST("piece storage preserves file boundaries and can replace an evicted piece") {
    namespace lt = libtorrent;
    constexpr int piece_size = 16 * 1024;
    const PieceDirectory directory;
    const lt::piece_index_t first{0};
    const lt::piece_index_t second{1};

    lt::file_storage files;
    files.add_file("bundle/first.bin", 4);
    files.add_file("bundle/.pad/4", 4, lt::file_storage::flag_pad_file);
    files.add_file("bundle/second.bin", piece_size);
    files.set_piece_length(piece_size);
    files.set_num_pieces(2);

    const lt::aux::vector<lt::download_priority_t, lt::file_index_t> priorities;
    const lt::storage_params storage_parameters{
        files, nullptr, directory.path, lt::storage_mode_sparse, priorities, {}
    };
    lt::aux::session_settings settings;
    lt::aux::posix_storage storage(storage_parameters, true);
    lt::storage_error error;
    storage.initialize(settings, error);
    NUVIO_EXPECT_TRUE(!error);
    NUVIO_EXPECT_TRUE(!storage.has_any_file(error));

    std::array<char, piece_size> first_piece;
    first_piece.fill('a');
    std::array<char, 8> second_piece;
    second_piece.fill('b');
    NUVIO_EXPECT_EQ(storage.write(settings, first_piece, first, 0, error), piece_size);
    NUVIO_EXPECT_EQ(storage.write(settings, second_piece, second, 0, error), 8);
    NUVIO_EXPECT_TRUE(!error);
    NUVIO_EXPECT_TRUE(storage.has_any_file(error));

    std::array<char, piece_size> readback{};
    NUVIO_EXPECT_EQ(storage.read(settings, readback, first, 0, error), piece_size);
    std::fill(first_piece.begin() + 4, first_piece.begin() + 8, '\0');
    NUVIO_EXPECT_TRUE(first_piece == readback);
    NUVIO_EXPECT_TRUE(!std::filesystem::exists(std::filesystem::path(directory.path) / "bundle"));

    lt::add_torrent_params resume;
    resume.have_pieces.resize(2, true);
    NUVIO_EXPECT_TRUE(storage.verify_resume_data(resume, {}, error));
    NUVIO_EXPECT_TRUE(std::filesystem::remove(lt::nuvio_piece_path(directory.path, first)));
    NUVIO_EXPECT_TRUE(!storage.verify_resume_data(resume, {}, error));
    NUVIO_EXPECT_EQ(storage.read(settings, readback, first, 0, error), 0);
    NUVIO_EXPECT_TRUE(bool(error));
    error = {};

    std::array<char, 8> second_readback{};
    NUVIO_EXPECT_EQ(storage.read(settings, second_readback, second, 0, error), 8);
    NUVIO_EXPECT_TRUE(second_piece == second_readback);
    NUVIO_EXPECT_EQ(storage.write(settings, first_piece, first, 0, error), piece_size);
    NUVIO_EXPECT_TRUE(storage.verify_resume_data(resume, {}, error));
    storage.delete_files(lt::session_handle::delete_files, error);
    NUVIO_EXPECT_TRUE(!error);
    NUVIO_EXPECT_TRUE(!std::filesystem::exists(lt::nuvio_piece_directory(directory.path)));
}
