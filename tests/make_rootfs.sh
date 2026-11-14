#!/bin/sh
# Build an Alpine minirootfs root filesystem for the capsula integration tests.
#
# The tarball is resolved in this order:
#   CAPSULA_ALPINE_ROOTFS  path to an already downloaded tarball
#   CAPSULA_ALPINE_URL     explicit download URL
#   ALPINE_VERSION         release branch (default: latest-stable)
#   ALPINE_ARCH            target architecture (default: uname -m)
#
# Downloaded tarballs are cached in CAPSULA_ALPINE_CACHE or
# $XDG_CACHE_HOME/capsula and verified against Alpine's published sha256.
set -eu

dest=${1:?usage: make_rootfs.sh DEST}

say() { echo "make_rootfs: $*" >&2; }
die() {
    say "$*"
    exit 1
}

download() {
    url=$1
    out=$2
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$out"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$out" "$url"
    else
        die "curl or wget is required to download $url"
    fi
}

verify_sha256() {
    file=$1
    sum=$2
    [ -n "$sum" ] || return 0
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s  %s\n' "$sum" "$file" | sha256sum -c - >/dev/null 2>&1
    elif command -v shasum >/dev/null 2>&1; then
        printf '%s  %s\n' "$sum" "$file" | shasum -a 256 -c - >/dev/null 2>&1
    else
        say "warning: no sha256 tool, checksum not verified for $file"
        return 0
    fi
}

if [ -n "${ALPINE_ARCH:-}" ]; then
    arch=$ALPINE_ARCH
else
    case "$(uname -m)" in
        x86_64|amd64) arch=x86_64 ;;
        i?86|x86) arch=x86 ;;
        aarch64|arm64) arch=aarch64 ;;
        armv7*) arch=armv7 ;;
        armv6*|armhf|arm) arch=armhf ;;
        ppc64le) arch=ppc64le ;;
        s390x) arch=s390x ;;
        riscv64) arch=riscv64 ;;
        *) die "unsupported architecture: $(uname -m) (set ALPINE_ARCH)" ;;
    esac
fi
branch=${ALPINE_VERSION:-latest-stable}

if [ -n "${CAPSULA_ALPINE_CACHE:-}" ]; then
    cache=$CAPSULA_ALPINE_CACHE
elif [ -n "${XDG_CACHE_HOME:-}" ]; then
    cache=$XDG_CACHE_HOME/capsula
elif [ -n "${HOME:-}" ]; then
    cache=$HOME/.cache/capsula
else
    cache=${TMPDIR:-/tmp}/capsula-cache
fi
mkdir -p "$cache"

if [ -n "${CAPSULA_ALPINE_ROOTFS:-}" ]; then
    tarball=$CAPSULA_ALPINE_ROOTFS
    [ -f "$tarball" ] || die "tarball not found: $tarball"
else
    if [ -n "${CAPSULA_ALPINE_URL:-}" ]; then
        url=$CAPSULA_ALPINE_URL
        file=${url##*/}
        sha=
    else
        base="https://dl-cdn.alpinelinux.org/alpine/$branch/releases/$arch"
        meta=$cache/latest-releases-$branch-$arch.yaml
        tmp_meta=$meta.$$
        download "$base/latest-releases.yaml" "$tmp_meta" ||
            die "cannot fetch Alpine release metadata from $base"
        file=$(sed -n 's/^  file: *//p' "$tmp_meta" |
            grep 'alpine-minirootfs-' | head -n 1)
        sha=$(sed -n 's/^  sha256: *//p' "$tmp_meta" | head -n 1)
        rm -f "$tmp_meta"
        [ -n "$file" ] || die "no minirootfs for $branch/$arch"
        url=$base/$file
    fi

    tarball=$cache/$file
    if [ -f "$tarball" ] && ! verify_sha256 "$tarball" "$sha"; then
        say "cached tarball is corrupt, downloading again"
        rm -f "$tarball"
    fi
    if [ ! -f "$tarball" ]; then
        tmp_tarball=$tarball.$$
        say "downloading $url"
        download "$url" "$tmp_tarball" ||
            die "download failed: $url"
        verify_sha256 "$tmp_tarball" "$sha" || {
            rm -f "$tmp_tarball"
            die "checksum mismatch for $url"
        }
        mv "$tmp_tarball" "$tarball"
    fi
fi

rm -rf "$dest"
mkdir -p "$dest"
# файлы в архиве Alpine принадлежат root; при запуске тестов из-под обычного
# пользователя права владельца не важны, так как user namespace пробрасывает его в root.
if ! tar --no-same-owner -xzf "$tarball" -C "$dest" 2>/dev/null; then
    rm -rf "$dest"
    mkdir -p "$dest"
    tar -xzf "$tarball" -C "$dest"
fi

# тесты зависят от абсолютных путей, поэтому проверяем,
# что нужные утилиты в ожидаемых каталогах
for exe in bin/sh bin/cat bin/true bin/hostname usr/bin/awk usr/bin/test \
    usr/bin/readlink usr/bin/id; do
    if [ ! -e "$dest/$exe" ] && [ ! -L "$dest/$exe" ]; then
        die "unexpected rootfs layout: /$exe is missing"
    fi
done

[ -e "$dest/etc/resolv.conf" ] || : >"$dest/etc/resolv.conf"
chmod 1777 "$dest/tmp"
chmod 0700 "$dest/root"

echo "$dest"
