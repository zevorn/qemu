#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

usage()
{
    cat <<EOF
Usage: $0 --source-dir DIR --prefix DIR --output-dir DIR --platform PLATFORM \\
          --host-arch ARCH --version VERSION \\
          [--macos-min-version VERSION]

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
macos_min_version=
windows_license_packages=

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
    --macos-min-version)
        macos_min_version=$2
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

if [ -n "$macos_min_version" ] && [ "$platform" != macos ]; then
    die "--macos-min-version is only supported for macos packages"
fi

case "$macos_min_version" in
""|*[!0-9.]*|.*|*..*)
    [ -z "$macos_min_version" ] ||
        die "Invalid macOS version: $macos_min_version"
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
notice_dir="$bundle_dir/licenses"

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

copy_license_file()
{
    local destination source

    source=$1
    destination=$2
    [ -f "$source" ] || return
    mkdir -p "$(dirname "$destination")"
    cp -p "$source" "$destination"
}

copy_linux_license()
{
    local library notice package

    library=$1
    if command -v rpm >/dev/null; then
        package=$(rpm -q --qf '%{NAME}' -f "$library") ||
            die "Cannot identify the RPM for $library"
        notice="/usr/share/licenses/$package"
        [ -e "$notice" ] || die "No RPM license notice for $package"
        mkdir -p "$notice_dir/rpm/$package"
        if [ -d "$notice" ]; then
            cp -a "$notice/." "$notice_dir/rpm/$package"
        else
            copy_license_file "$notice" "$notice_dir/rpm/$package/LICENSE"
        fi
    elif command -v dpkg-query >/dev/null; then
        package=$(dpkg-query -S "$library" | awk -F: 'NR == 1 { print $1 }')
        [ -n "$package" ] ||
            die "Cannot identify the Debian package for $library"
        notice="/usr/share/doc/${package%%:*}/copyright"
        [ -r "$notice" ] || die "No Debian copyright notice for $package"
        copy_license_file "$notice" "$notice_dir/debian/$package/copyright"
    else
        die "Cannot identify Linux package licenses"
    fi
}

copy_windows_license()
{
    local found library notice notices package

    library=$1
    package=$(LC_ALL=C pacman -Qo "$library" | awk '{ print $5 }') ||
        die "Cannot identify the MSYS2 package for $library"
    [ -n "$package" ] || die "Cannot identify the MSYS2 package for $library"
    case " $windows_license_packages " in
    *" $package "*)
        return
        ;;
    esac
    notices=$(pacman -Qql "$package" |
        awk '$2 ~ /\/share\/(doc|licenses)\// { print $2 }')
    found=0
    while IFS= read -r notice; do
        [ -f "$notice" ] || continue
        copy_license_file "$notice" \
            "$notice_dir/pacman/$package/${notice#/}"
        found=1
    done <<< "$notices"
    [ "$found" -eq 1 ] || die "No MSYS2 license notice for $package"
    windows_license_packages="$windows_license_packages $package"
}

find_executables()
{
    local executable_dir

    for executable_dir in "$bundle_dir/bin" "$bundle_dir/sbin" \
                          "$bundle_dir/libexec"; do
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
            copy_linux_license "$library"
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
    local executable executable_dir library

    while IFS= read -r -d '' executable; do
        case "$executable" in
        *.exe)
            ;;
        *)
            continue
            ;;
        esac

        executable_dir=$(dirname "$executable")
        ldd "$executable" |
            awk '$2 == "=>" && $3 ~ /^\// { print $3 }' |
        while IFS= read -r library; do
            case "$library" in
            /clang64/bin/*.dll|/mingw64/bin/*.dll|/ucrt64/bin/*.dll)
                cp -L "$library" "$executable_dir/"
                copy_windows_license "$library"
                ;;
            esac
        done
    done < <(find_executables)
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

macos_image_versions()
{
    otool -l "$1" |
        awk '$1 == "cmd" &&
             ($2 == "LC_BUILD_VERSION" || $2 == "LC_VERSION_MIN_MACOSX") {
                 version = 1; next
             }
             version && ($1 == "minos" || $1 == "version") {
                 print $2; version = 0
             }'
}

version_is_newer()
{
    awk -v candidate="$1" -v maximum="$2" '
        BEGIN {
            candidate_count = split(candidate, candidate_parts, ".")
            maximum_count = split(maximum, maximum_parts, ".")
            count = candidate_count > maximum_count ?
                candidate_count : maximum_count
            for (i = 1; i <= count; i++) {
                if (candidate_parts[i] + 0 > maximum_parts[i] + 0) {
                    exit 0
                }
                if (candidate_parts[i] + 0 < maximum_parts[i] + 0) {
                    exit 1
                }
            }
            exit 1
        }'
}

verify_macos_deployment_target()
{
    local image image_version found

    [ -n "$macos_min_version" ] ||
        die "--macos-min-version is required for macos packages"
    while IFS= read -r -d '' image; do
        if ! file -b "$image" | grep -q 'Mach-O'; then
            continue
        fi

        found=0
        while IFS= read -r image_version; do
            [ -n "$image_version" ] || continue
            found=1
            if version_is_newer "$image_version" "$macos_min_version"; then
                die "$image requires macOS $image_version, above" \
                    " $macos_min_version"
            fi
        done < <(macos_image_versions "$image")
        [ "$found" -eq 1 ] || die "No macOS deployment target in $image"
    done < <(find_executables; find "$library_dir" -type f -print0)
}

macos_formula_for_library()
{
    local formula formula_prefix library

    library=$(basename "$1")
    while IFS= read -r formula; do
        formula_prefix=$(brew --prefix "$formula") || continue
        if [ -f "$formula_prefix/lib/$library" ]; then
            echo "$formula"
            return 0
        fi
    done < <(env HOMEBREW_NO_ENV_HINTS=1 brew deps --include-build qemu)
    return 1
}

copy_macos_licenses()
{
    local archive cache_dir formula formula_prefix formulas found library notice
    local source_dir

    command -v brew >/dev/null || die "Homebrew is required for macOS licenses"
    formulas=
    while IFS= read -r -d '' library; do
        formula=$(macos_formula_for_library "$library") ||
            die "Cannot identify the Homebrew formula for $library"
        case " $formulas " in
        *" $formula "*)
            ;;
        *)
            formulas="$formulas $formula"
            ;;
        esac
    done < <(find "$library_dir" -type f -print0)

    for formula in $formulas; do
        formula_prefix=$(brew --prefix "$formula") ||
            die "Cannot find the Homebrew prefix for $formula"
        found=0
        while IFS= read -r -d '' notice; do
            copy_license_file "$notice" \
                "$notice_dir/homebrew/$formula/${notice#"$formula_prefix"/}"
            found=1
        done < <(find -L "$formula_prefix" -type f \
            \( -iname 'COPYING*' -o -iname 'LICENSE*' -o -iname 'NOTICE*' \) \
            -print0)

        if [ "$found" -eq 0 ]; then
            cache_dir="$(brew --cache)/downloads"
            brew fetch --build-from-source --retry --formula "$formula"
            archive=$(find "$cache_dir" -maxdepth 1 -type f \
                -name "*--$formula-*" ! -name '*.bottle*' -print |
                LC_ALL=C sort | tail -n 1)
            [ -n "$archive" ] || die "No Homebrew source archive for $formula"

            source_dir=$(mktemp -d "$bundle_dir/source.XXXXXX")
            case "$archive" in
            *.zip)
                unzip -q "$archive" -d "$source_dir"
                ;;
            *)
                tar -xf "$archive" -C "$source_dir"
                ;;
            esac
            while IFS= read -r -d '' notice; do
                copy_license_file "$notice" \
                    "$notice_dir/homebrew/$formula/${notice#"$source_dir"/}"
                found=1
            done < <(find "$source_dir" -type f \
                \( -iname 'COPYING*' -o -iname 'LICENSE*' \
                -o -iname 'NOTICE*' \) -print0)
            rm -rf -- "$source_dir"
        fi
        [ "$found" -eq 1 ] || die "No Homebrew license notice for $formula"
    done
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
    verify_macos_deployment_target
    copy_macos_licenses

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

The package includes QEMU's COPYING, COPYING.LIB, and LICENSE files.  License
notices for bundled third-party libraries are in licenses/.  The release's
SHA256SUMS file records the archive checksum.
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
