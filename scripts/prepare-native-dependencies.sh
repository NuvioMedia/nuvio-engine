#!/usr/bin/env bash
set -euo pipefail

boost_version=1.86.0
boost_archive_version=1_86_0
boost_sha256=1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b
boost_url="https://archives.boost.io/release/${boost_version}/source/boost_${boost_archive_version}.tar.bz2"
libtorrent_commit=740a0b9aeabe00e762cc0efe4a0f27593db2550b
libtorrent_tag=v2.0.12
try_signal_commit=105cce59972f925a33aa6b1c3109e4cd3caf583d

if [[ $# -ne 1 ]]; then
    echo "usage: $0 OUTPUT_DIRECTORY" >&2
    exit 2
fi

output_root=$1
downloads="$output_root/downloads"
sources="$output_root/sources"
mkdir -p "$downloads" "$sources"

file_sha256() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

boost_archive="$downloads/boost_${boost_archive_version}.tar.bz2"
if [[ ! -f "$boost_archive" \
    || "$(file_sha256 "$boost_archive")" != "$boost_sha256" ]]; then
    rm -f "$boost_archive.download"
    curl --fail --location --retry 5 --retry-all-errors --silent --show-error \
        --output "$boost_archive.download" \
        "$boost_url"
    if [[ "$(file_sha256 "$boost_archive.download")" != "$boost_sha256" ]]; then
        echo "checksum mismatch for downloaded $boost_archive" >&2
        exit 1
    fi
    mv "$boost_archive.download" "$boost_archive"
fi
if [[ "$(file_sha256 "$boost_archive")" != "$boost_sha256" ]]; then
    echo "checksum mismatch for $boost_archive" >&2
    exit 1
fi

boost_source="$sources/boost_${boost_archive_version}"
if [[ ! -f "$boost_source/boost/version.hpp" ]]; then
    cmake -E remove_directory "$boost_source"
    tar -xjf "$boost_archive" -C "$sources"
fi

libtorrent_source="$sources/libtorrent-2.0.12"
if [[ ! -d "$libtorrent_source/.git" ]]; then
    cmake -E remove_directory "$libtorrent_source"
    git clone \
        --branch "$libtorrent_tag" \
        --depth 1 \
        --no-checkout \
        https://github.com/arvidn/libtorrent.git \
        "$libtorrent_source"
    git -C "$libtorrent_source" checkout --detach "$libtorrent_commit"
fi
if [[ "$(git -C "$libtorrent_source" rev-parse HEAD)" != "$libtorrent_commit" ]]; then
    echo "libtorrent source does not match the pinned commit" >&2
    exit 1
fi
if [[ ! -f "$libtorrent_source/deps/try_signal/LICENSE" ]]; then
    git -C "$libtorrent_source" submodule update \
        --init \
        --depth 1 \
        deps/try_signal
fi
if [[ "$(git -C "$libtorrent_source/deps/try_signal" rev-parse HEAD)" != "$try_signal_commit" ]]; then
    echo "try_signal source does not match the pinned commit" >&2
    exit 1
fi

patched_file_states=(
    'include/libtorrent/settings_pack.hpp:101e5bd1561765eedfe498e753cc23cb66611bf8a47b66455497fb91e7514d67:f0a2af9a8045f0c6cad24e37115db93a760c5121806740e1d321ae32bf027de2'
    'src/enum_net.cpp:4e97a556d64a57df6176bada627526e1f8a00ef8c7ad0d86ca6d06b52a8301b7:19db38bc56229b36e6533e0aebb337fc834d7930e4c243f6d59c21acbd20f614'
    'src/session_impl.cpp:e33778939a03634f7ca588752f924afcdf790ac48ddd2a127af71e00f34d1c65:e9e7716829023651072714fa26e39f19326df8d3ec7104a82b7288abb6cb044c'
    'src/settings_pack.cpp:059759d20da14fc8ada6177df77562addda1a033ef6fe08edf631f757dfd9bf4:7e938dba3b5d3d0184b1446e6c8e82406d1196f241cc60414ca6224bec682a10'
)
legacy_session_impl_hashes=(
    1d25c8d56d7342db5be0e5cf147955ca97542f52d490203fe6408a6acc15d702
    23a77b31941aca6090f79517ede37bb96b1cf6af1b9834d616bcfff1da5bfd37
)
source_state=
expected_modified_files=
for specification in "${patched_file_states[@]}"; do
    file=${specification%%:*}
    hashes=${specification#*:}
    original_hash=${hashes%%:*}
    patched_hash=${hashes#*:}
    actual_hash=$(file_sha256 "$libtorrent_source/$file")
    if [[ "$actual_hash" == "$original_hash" ]]; then
        file_state=original
    elif [[ "$actual_hash" == "$patched_hash" ]]; then
        file_state=patched
        expected_modified_files+="$file"$'\n'
    elif [[ "$file" == src/session_impl.cpp ]]; then
        file_state=
        for legacy_hash in "${legacy_session_impl_hashes[@]}"; do
            if [[ "$actual_hash" == "$legacy_hash" ]]; then
                file_state=patched
                expected_modified_files+="$file"$'\n'
                break
            fi
        done
        if [[ -z "$file_state" ]]; then
            echo "libtorrent source has an unexpected change in $file" >&2
            exit 1
        fi
    else
        echo "libtorrent source has an unexpected change in $file" >&2
        exit 1
    fi
    if [[ -n "$source_state" && "$source_state" != "$file_state" ]]; then
        echo "libtorrent source is only partially patched" >&2
        exit 1
    fi
    source_state=$file_state
done

actual_modified_files=$(git -C "$libtorrent_source" diff --name-only | LC_ALL=C sort)
expected_modified_files=$(printf '%s' "$expected_modified_files" | LC_ALL=C sort)
if [[ "$actual_modified_files" != "$expected_modified_files" ]]; then
    echo "libtorrent source contains changes outside the verified patch set" >&2
    exit 1
fi
if [[ -n "$(git -C "$libtorrent_source" ls-files --others --exclude-standard)" ]]; then
    echo "libtorrent source contains unexpected untracked files" >&2
    exit 1
fi

echo "prepared Boost ${boost_version} and libtorrent ${libtorrent_tag}"
