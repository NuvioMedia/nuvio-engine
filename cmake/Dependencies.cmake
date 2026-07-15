include(FetchContent)

function(nuvio_engine_add_libtorrent)
    if(TARGET torrent-rasterbar)
        return()
    endif()

    cmake_policy(PUSH)
    if(POLICY CMP0144)
        cmake_policy(SET CMP0144 NEW)
    endif()
    if(POLICY CMP0167)
        cmake_policy(SET CMP0167 OLD)
    endif()
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    if(POLICY CMP0183)
        cmake_policy(SET CMP0183 OLD)
    endif()

    FetchContent_Declare(
        nuvio_boost
        URL https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2
        URL_HASH SHA256=1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(nuvio_boost)
    if(NOT nuvio_boost_POPULATED)
        FetchContent_Populate(nuvio_boost)
    endif()

    set(BOOST_ROOT "${nuvio_boost_SOURCE_DIR}")
    # FindBoost applies toolchain root-path rules while cross-compiling. Point it
    # at the populated, checksummed headers explicitly so Android and Apple
    # builds cannot accidentally fall back to host Boost installations.
    set(Boost_INCLUDE_DIR "${nuvio_boost_SOURCE_DIR}")
    set(Boost_INCLUDE_DIRS "${nuvio_boost_SOURCE_DIR}")
    set(Boost_NO_SYSTEM_PATHS ON)
    set(Boost_NO_BOOST_CMAKE ON)
    set(CMAKE_POLICY_DEFAULT_CMP0144 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0167 OLD)
    set(CMAKE_POLICY_DEFAULT_CMP0183 OLD)

    FetchContent_Declare(
        nuvio_libtorrent
        GIT_REPOSITORY https://github.com/arvidn/libtorrent.git
        GIT_TAG 740a0b9aeabe00e762cc0efe4a0f27593db2550b
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE
        # try_signal is part of libtorrent's portable signal handling. The
        # simulation and optional GnuTLS adapter submodules are not used by this
        # build and would multiply every cross-ABI checkout.
        GIT_SUBMODULES deps/try_signal
        GIT_SUBMODULES_RECURSE FALSE
    )
    FetchContent_GetProperties(nuvio_libtorrent)
    if(NOT nuvio_libtorrent_POPULATED)
        FetchContent_Populate(nuvio_libtorrent)
    endif()

    set(
        nuvio_libtorrent_patches
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libtorrent-2.0.12-macos-route-bounds.patch"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libtorrent-2.0.12-nuvio-ca-bundle.patch"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libtorrent-2.0.12-nuvio-apple-trust.patch"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libtorrent-2.0.12-nuvio-windows-trust.patch"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/libtorrent-2.0.12-nuvio-windows-system-trust.patch"
    )
    set(
        nuvio_libtorrent_patch_files
        "src/enum_net.cpp"
        "include/libtorrent/settings_pack.hpp"
        "src/session_impl.cpp"
        "src/session_impl.cpp"
        "src/session_impl.cpp"
    )
    set(
        nuvio_libtorrent_patch_sentinels
        "message_end = reinterpret_cast<char*>"
        "nuvio_ssl_ca_bundle"
        "nuvio_apple_configure_tls_trust"
        "nuvio_windows_configure_tls_trust"
        "live Windows store without replacing an explicit PEM store"
    )
    list(LENGTH nuvio_libtorrent_patches nuvio_libtorrent_patch_count)
    math(EXPR nuvio_libtorrent_last_patch "${nuvio_libtorrent_patch_count} - 1")
    foreach(nuvio_libtorrent_patch_index RANGE 0 ${nuvio_libtorrent_last_patch})
        list(GET nuvio_libtorrent_patches ${nuvio_libtorrent_patch_index} nuvio_libtorrent_patch)
        list(GET nuvio_libtorrent_patch_files ${nuvio_libtorrent_patch_index} nuvio_libtorrent_patch_file)
        list(GET nuvio_libtorrent_patch_sentinels ${nuvio_libtorrent_patch_index} nuvio_libtorrent_patch_sentinel)
        file(
            READ
            "${nuvio_libtorrent_SOURCE_DIR}/${nuvio_libtorrent_patch_file}"
            nuvio_libtorrent_patch_source
        )
        string(
            FIND
            "${nuvio_libtorrent_patch_source}"
            "${nuvio_libtorrent_patch_sentinel}"
            nuvio_libtorrent_patch_position
        )
        if(nuvio_libtorrent_patch_position EQUAL -1)
            execute_process(
                COMMAND git apply --unidiff-zero --check "${nuvio_libtorrent_patch}"
                WORKING_DIRECTORY "${nuvio_libtorrent_SOURCE_DIR}"
                RESULT_VARIABLE nuvio_libtorrent_patch_needed
                OUTPUT_QUIET
                ERROR_QUIET
            )
            if(NOT nuvio_libtorrent_patch_needed EQUAL 0)
                get_filename_component(
                    nuvio_libtorrent_patch_name
                    "${nuvio_libtorrent_patch}"
                    NAME
                )
                message(FATAL_ERROR "Pinned libtorrent patch does not apply: ${nuvio_libtorrent_patch_name}")
            endif()
            execute_process(
                COMMAND git apply --unidiff-zero "${nuvio_libtorrent_patch}"
                WORKING_DIRECTORY "${nuvio_libtorrent_SOURCE_DIR}"
                COMMAND_ERROR_IS_FATAL ANY
            )
        endif()
    endforeach()

    set(BUILD_SHARED_LIBS OFF)
    set(build_tests OFF)
    set(build_examples OFF)
    set(build_tools OFF)
    set(python-bindings OFF)
    set(developer-options OFF)
    set(logging OFF)
    add_subdirectory(
        "${nuvio_libtorrent_SOURCE_DIR}"
        "${nuvio_libtorrent_BINARY_DIR}"
        EXCLUDE_FROM_ALL
    )
    cmake_policy(POP)
endfunction()
