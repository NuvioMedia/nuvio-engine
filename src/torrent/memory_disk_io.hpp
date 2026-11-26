#ifndef NUVIO_ENGINE_MEMORY_DISK_IO_HPP
#define NUVIO_ENGINE_MEMORY_DISK_IO_HPP

#include "storage/memory_piece_store.hpp"

#include <functional>
#include <memory>

#include <libtorrent/disk_interface.hpp>
#include <libtorrent/io_context.hpp>
#include <libtorrent/performance_counters.hpp>
#include <libtorrent/settings_pack.hpp>

namespace nuvio::torrent {

using DiskIoConstructor = std::function<std::unique_ptr<lt::disk_interface>(
    lt::io_context&,
    const lt::settings_interface&,
    lt::counters&
)>;

[[nodiscard]] DiskIoConstructor memory_disk_io_constructor(
    std::shared_ptr<storage::MemoryPieceStore> store
);

}

#endif
