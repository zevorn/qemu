#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

usage()
{
    cat <<EOF
Usage: $0 --source-dir DIR --prefix DIR --output-dir DIR --platform PLATFORM \\
          --host-arch ARCH --version VERSION

Bundle a completed relocatable QEMU installation for a host platform.
PLATFORM must be linux, macos, or windows.
EOF
}

die()
{
    echo "$*" >&2
    exit 1
}

source_dir=
prefix=
output_dir=
platform=
host_arch=
version=

while [ "$#" -gt 0 ]; do
    case "$1" in
    --source-dir)
        source_dir=$2
        shift 2
        ;;
    --prefix)
        prefix=$2
        shift 2
        ;;
    --output-dir)
        output_dir=$2
        shift 2
        ;;
    --platform)
        platform=$2
        shift 2
        ;;
    --host-arch)
        host_arch=$2
        shift 2
        ;;
    --version)
        version=$2
        shift 2
        ;;
    --help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        die "Unknown argument: $1"
        ;;
    esac
done

[ -n "$source_dir" ] || die "--source-dir is required"
[ -n "$prefix" ] || die "--prefix is required"
[ -n "$output_dir" ] || die "--output-dir is required"
[ -n "$platform" ] || die "--platform is required"
[ -n "$host_arch" ] || die "--host-arch is required"
[ -n "$version" ] || die "--version is required"

case "$platform" in
linux|macos|windows)
    ;;
*)
    die "Unsupported platform: $platform"
    ;;
esac

case "$host_arch:$version" in
*[!-A-Za-z0-9._:]*)
    die "Host architecture and version must be file-name safe"
    ;;
esac

[ -d "$source_dir" ] || die "Source directory does not exist: $source_dir"
[ -d "$prefix/bin" ] || die "QEMU installation has no bin directory: $prefix"

bundle_name="qemu-${version}-${platform}-${host_arch}"
mkdir -p "$output_dir"
bundle_dir="$output_dir/$bundle_name"
library_dir="$bundle_dir/lib"

case "$bundle_dir" in
"$output_dir"/*)
    ;;
*)
    die "Refusing to use a package directory outside the output directory"
    ;;
esac

rm -rf -- "$bundle_dir"
mkdir -p "$bundle_dir"
cp -a "$prefix/." "$bundle_dir"
mkdir -p "$library_dir"
rm -rf -- "$bundle_dir/include" "$bundle_dir/share/doc" "$bundle_dir/share/man"
find "$library_dir" -type f -name '*.a' -delete
find "$library_dir" -type d -name pkgconfig -prune -exec rm -rf -- {} +

for license in COPYING COPYING.LIB LICENSE; do
    install -m 0644 "$source_dir/$license" "$bundle_dir/$license"
done

find_executables()
{
    local executable_dir

    for executable_dir in "$bundle_dir/bin" "$bundle_dir/sbin"; do
        [ -d "$executable_dir" ] || continue
        find "$executable_dir" -type f -perm -u+x -print0
    done
}

is_glibc_library()
{
    case "$(basename "$1")" in
    ld-linux-*.so*|libanl.so.*|libc.so.*|libdl.so.*|libm.so.*|libnsl.so.*|\
    libpthread.so.*|libresolv.so.*|librt.so.*|libutil.so.*)
        return 0
        ;;
    *)
        return 1
        ;;
    esac
}

bundle_linux_libraries()
{
    local executable library relative_library_dir

    while IFS= read -r -d '' executable; do
        if ! file -b "$executable" | grep -q 'ELF'; then
            continue
        fi

        ldd "$executable" |
            awk '$2 == "=>" && $3 ~ /^\// { print $3 }
                 $1 ~ /^\// { print $1 }'
    done < <(find_executables) |
        LC_ALL=C sort -u |
        while IFS= read -r library; do
            [ -r "$library" ] || continue
            if is_glibc_library "$library"; then
                continue
            fi
            cp -L "$library" "$library_dir/"
        done

    while IFS= read -r -d '' executable; do
        if ! file -b "$executable" | grep -q 'ELF'; then
            continue
        fi

        relative_library_dir=$(python3 -c \
            'import os, sys; print(os.path.relpath(sys.argv[1],
             os.path.dirname(sys.argv[2])))' "$library_dir" "$executable")
        patchelf --force-rpath --set-rpath \
            "\$ORIGIN/$relative_library_dir" "$executable"
    done < <(find_executables)
}

bundle_windows_libraries()
{
    local executable library

    while IFS= read -r -d '' executable; do
        ldd "$executable" |
            awk '$2 == "=>" && $3 ~ /^\// { print $3 }'
    done < <(find "$bundle_dir/bin" -type f -name '*.exe' -print0) |
        LC_ALL=C sort -u |
        while IFS= read -r library; do
            case "$library" in
            /clang64/bin/*.dll|/mingw64/bin/*.dll|/ucrt64/bin/*.dll)
                cp -L "$library" "$bundle_dir/bin/"
                ;;
            esac
        done
}

deduplicate_macos_rpaths()
{
    local image duplicate_rpaths rpath

    while IFS= read -r -d '' image; do
        if ! file -b "$image" | grep -q 'Mach-O'; then
            continue
        fi

        while :; do
            duplicate_rpaths=$(otool -l "$image" |
                awk '$1 == "cmd" && $2 == "LC_RPATH" { rpath = 1; next }
                     rpath && $1 == "path" { print $2; rpath = 0 }' |
                LC_ALL=C sort | uniq -d)
            [ -n "$duplicate_rpaths" ] || break

            while IFS= read -r rpath; do
                install_name_tool -delete_rpath "$rpath" "$image"
            done <<< "$duplicate_rpaths"
        done
    done < <(find_executables; find "$library_dir" -type f -print0)
}

bundle_macos_libraries()
{
    local executable entitlements
    local -a command

    command=(dylibbundler -cd -ns -b -d "$library_dir"
             -p '@executable_path/../lib')
    while IFS= read -r -d '' executable; do
        if file -b "$executable" | grep -q 'Mach-O'; then
            command+=(-x "$executable")
        fi
    done < <(find_executables)

    [ "${#command[@]}" -gt 7 ] || die "No Mach-O executables were installed"
    if ! "${command[@]}" > "$bundle_dir/dylibbundler.log" 2>&1; then
        cat "$bundle_dir/dylibbundler.log"
        exit 1
    fi
    rm -f "$bundle_dir/dylibbundler.log"

    deduplicate_macos_rpaths

    while IFS= read -r -d '' executable; do
        if file -b "$executable" | grep -q 'Mach-O'; then
            codesign --force --sign - --timestamp=none "$executable"
        fi
    done < <(find "$library_dir" -type f -print0)

    while IFS= read -r -d '' executable; do
        if ! file -b "$executable" | grep -q 'Mach-O'; then
            continue
        fi

        entitlements=$(mktemp "$bundle_dir/entitlements.XXXXXX")
        if codesign -d --entitlements :- "$executable" > "$entitlements" \
            2>/dev/null && [ -s "$entitlements" ]; then
            codesign --force --sign - --timestamp=none \
                --entitlements "$entitlements" "$executable"
        else
            codesign --force --sign - --timestamp=none "$executable"
        fi
        rm -f "$entitlements"
    done < <(find_executables)
}

case "$platform" in
linux)
    bundle_linux_libraries
    archive_name="$bundle_name.tar.gz"
    ;;
macos)
    bundle_macos_libraries
    archive_name="$bundle_name.tar.gz"
    ;;
windows)
    bundle_windows_libraries
    archive_name="$bundle_name.zip"
    ;;
esac

cat > "$bundle_dir/README.txt" <<EOF
QEMU $version for $platform $host_arch

This archive contains every QEMU executable selected by QEMU's default target
list for this host platform and CPU architecture.  The binaries are in bin/.
Firmware and other QEMU data are in share/.  The SDL display backend is
disabled to keep the package portable across host SDL runtimes.

The package includes QEMU's COPYING, COPYING.LIB, and LICENSE files.  The
release's SHA256SUMS file records the archive checksum.
EOF

archive_path="$output_dir/$archive_name"
rm -f -- "$archive_path"
case "$platform" in
windows)
    (
        cd "$output_dir"
        zip -q -r "$archive_name" "$bundle_name"
    )
    ;;
*)
    tar -C "$output_dir" -czf "$archive_path" "$bundle_name"
    ;;
esac

echo "Created $archive_path"
