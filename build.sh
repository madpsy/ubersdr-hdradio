#!/usr/bin/env bash
#
# Build ubersdr-hdradio for amd64 and arm64, and check that each one decodes.
#
# The binaries get copied into the UberSDR container to run, so they are built
# inside ubuntu:24.04 — the same image the container's runtime stage uses.
# nrsc5, FAAD2 (with nrsc5's HDC patch), FFTW, libstdc++ and libgcc are all
# linked in statically, and the build refuses a binary that needs anything but
# libc and libm: one that dlopens or links a library the container does not
# have dies at startup, and the extension shows that as a decoder that never
# locks.
#
# arm64 is built by running an arm64 ubuntu:24.04 under binfmt/qemu, not by
# cross-compiling, so the toolchain is the target toolchain and CMake sees the
# target arch. Slow, and correct without a sysroot to keep in step.
#
# Building is the easy half. A binary that links and runs can still fail to
# decode anything, so unless told otherwise each one plays every recording in
# testdata/ as UberSDR would feed it (test/check_sample.py, test/samples.txt):
# WSHE 820 kHz, all-digital AM, from the NA5B UberSDR at iq48 and at 12 kHz.
# It must sync, name the station, show what is playing and produce audio.
#
# Usage:
#   ./build.sh [options]
#
#   --arch LIST     comma-separated: amd64, arm64 (default: both)
#   --native        build on this host with the host toolchain instead of in a
#                   container.  This host arch only; for a quick edit-compile
#                   loop, not for anything you intend to ship
#   --clean         delete the build trees first
#   --no-check      build only, skip the decode check
#   --image IMAGE   build container image (default: ubuntu:24.04)
#   -j N            parallel jobs (default: all cores)
#   --publish       upload what this run built to the 'latest' release, which
#                   is what the UberSDR container downloads at build time
#   --yes           answer the publish confirmation in advance
#

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
arches="amd64 arm64"
image=ubuntu:24.04
native=0
clean=0
check=1
publish=0
assume_yes=0
jobs=$(nproc 2>/dev/null || echo 4)

# The UberSDR Dockerfile fetches
#   https://github.com/$REPO/releases/download/$TAG/ubersdr-hdradio_${TARGETARCH}
# so the tag is a moving one and the asset names are constants — publishing
# replaces what that build downloads rather than adding alongside it.
REPO="${UBERSDR_HDRADIO_REPO:-madpsy/ubersdr-hdradio}"
TAG="${UBERSDR_HDRADIO_TAG:-latest}"

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)     arches=$(echo "$2" | tr ',' ' '); shift 2 ;;
        --native)   native=1; shift ;;
        --clean)    clean=1; shift ;;
        --no-check) check=0; shift ;;
        --image)    image=$2; shift 2 ;;
        --publish)  publish=1; shift ;;
        --yes)      assume_yes=1; shift ;;
        -j)         jobs=$2; shift 2 ;;
        -j*)        jobs=${1#-j}; shift ;;
        -h|--help)  sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^#\{0,1\} \{0,1\}//'; exit 0 ;;
        *)          echo "build.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

say()  { printf '\n== %s\n' "$*"; }
fail() { printf '\nbuild.sh: %s\n' "$*" >&2; exit 1; }

for a in $arches; do
    case "$a" in
        amd64|arm64|arm|386) ;;
        *) fail "unknown arch '$a' (expected amd64, arm64, arm or 386)" ;;
    esac
done

# --- Two refusals, decided before anything is built ----------------------
#
# Both are about what a download button is allowed to serve, so they fail here
# rather than after a long build.

if [ "$publish" = 1 ] && [ "$check" = 0 ]; then
    fail "--publish and --no-check together would upload a decoder nothing has
watched decode anything. The check is the only thing standing between a clean
compile and a binary that locks onto nothing; a receiver would show it as a
decoder that never syncs, which looks like a weak station rather than a bad
build. Drop one of the two."
fi

if [ "$publish" = 1 ] && [ "$native" = 1 ]; then
    fail "--publish and --native together would upload a binary built against
this host's toolchain and libraries, and it runs inside ubuntu:24.04. If this
host is newer it dies at startup on a GLIBC_ version error naming everything
except the real problem. Build it in the container: drop --native."
fi

# --- Publishing ----------------------------------------------------------

publish_release() { # <binary>...
    local uploads=("$@")

    command -v gh >/dev/null 2>&1 || {
        echo "not published: gh not found — install the GitHub CLI, or upload the
  binaries by hand." >&2
        return
    }
    gh auth status >/dev/null 2>&1 || {
        echo "not published: gh is not logged in — run 'gh auth login'." >&2
        return
    }
    gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1 || {
        echo "not published: there is no '$TAG' release on $REPO to upload to." >&2
        return
    }

    echo
    echo "  Upload to https://github.com/$REPO/releases/tag/$TAG, replacing what is there:"
    for b in "${uploads[@]}"; do
        printf '      %-24s %s\n' "$(basename "$b")" "$(du -h "$b" | cut -f1)"
    done
    # Only the arches this run built are replaced. Any other arch already on the
    # release stays exactly as it was, at whatever age it was — so a run with
    # --arch amd64 leaves an arm64 asset from months ago in place, and the
    # release will not say so.
    echo

    # Asked for, one way or the other. The prompt is the default and stays that
    # way: publishing replaces what the container build downloads, and a run
    # that reaches this point by accident must not be able to complete it.
    # `--yes` changes only *when* the answer was given — on the command line
    # rather than at the prompt, which is the same person saying the same thing
    # and is what makes an unattended release possible.
    #
    # A flag rather than an environment variable on purpose: an exported
    # variable is inherited by everything a shell starts, so a `yes` meant for
    # one release would sit there quietly authorising the next.
    if [ "$assume_yes" = 1 ]; then
        echo "  --yes given; uploading."
    elif [ ! -t 0 ]; then
        echo "not published: --publish asks before uploading and there is no terminal
  to ask on. Pass --yes to answer it in advance." >&2
        return
    else
        local reply=''
        read -r -p "  type 'yes' to upload: " reply || true
        if [ "$reply" != "yes" ]; then
            echo "  not published."
            return
        fi
    fi

    # --clobber because the asset names are constants: without it the second
    # release is refused for every name that already exists.
    if gh release upload "$TAG" "${uploads[@]}" --clobber --repo "$REPO"; then
        echo "  uploaded to https://github.com/$REPO/releases/tag/$TAG"
    else
        echo "not published: the upload failed — the binaries are intact, try again." >&2
    fi
}

# --- The work done inside each container ---------------------------------
#
# Fed to bash on stdin rather than passed as `bash -c '...'`: an apostrophe
# anywhere in here — including in a comment — would close the quote and
# silently truncate the rest of the script, and the build would still exit 0.
#
# $1 = arch label, $2 = jobs, $3 = run the check, $4:$5 = host uid:gid
container_script=$(cat <<'CONTAINER_EOF'
set -euo pipefail
arch=$1; jobs=$2; check=$3; uid=$4; gid=$5

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# ca-certificates: CMake fetches nrsc5 and FAAD2 over https at pinned hashes.
apt-get install -y -qq --no-install-recommends \
    cmake ninja-build g++ python3 patch ca-certificates libfftw3-dev binutils >/dev/null

build=/src/build-$arch
cmake -S /src -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build" -j "$jobs"

binary=$(ls "$build"/ubersdr-hdradio_* 2>/dev/null | head -1)
if [ ! -x "$binary" ]; then
    echo "no binary was produced" >&2
    exit 1
fi

# Everything but libc and libm is meant to be linked in. Anything else here
# is a library the UberSDR container may not have.
needed=$(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
extra=$(printf '%s\n' $needed | grep -v -E '^(libc\.so\.6|libm\.so\.6|ld-linux.*)$' || true)
if [ -n "$extra" ]; then
    echo "the binary links libraries that should have been static:" >&2
    echo "$extra" >&2
    exit 1
fi
echo "CHECK_OK links only: $(echo $needed)"

if [ "$check" = 1 ]; then
    "$build/resampler_test" | tail -1 | grep -q PASS || { "$build/resampler_test" >&2; exit 1; }
    echo "CHECK_OK resampler"
    rc=0
    python3 /src/test/check_sample.py "$binary" /src/test/samples.txt > /tmp/check.txt || rc=$?
    sed 's/^/CHECK_OK /' /tmp/check.txt
    [ "$rc" = 0 ] || exit 1
fi

# The container runs as root; without this the build tree and the binary come
# out root-owned and the next non-root build cannot delete them.
chown -R "$uid:$gid" "$build"
CONTAINER_EOF
)

if [ "$clean" = 1 ]; then
    say "Removing build trees"
    rm -rf "$repo"/build-* "$repo"/build "$repo"/ubersdr-hdradio_*
fi

built=""

if [ "$native" = 1 ]; then
    say "Building natively (host toolchain)"
    cmake -S "$repo" -B "$repo/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$repo/build" -j "$jobs"
    binary=$(ls "$repo"/build/ubersdr-hdradio_* 2>/dev/null | head -1) \
        || fail "the build produced no binary"
    cp "$binary" "$repo/"
    built="$repo/$(basename "$binary")"
    if [ "$check" = 1 ]; then
        say "Checking $(basename "$binary")"
        "$repo/build/resampler_test" | tail -1
        python3 "$repo/test/check_sample.py" "$binary" "$repo/test/samples.txt" \
            || fail "the decode check failed"
    fi
    say "Done"
    echo "  $built"
    echo
    echo "Built with this host's toolchain — fine for testing here, not for the"
    echo "container. Drop --native for anything you intend to publish."
    exit 0
fi

command -v docker >/dev/null 2>&1 \
    || fail "docker is needed to build for both architectures; use --native to
build only for this host"

# arm64 on an amd64 host needs the binfmt handler, or docker starts the
# container and every process in it dies with exec format error.
for a in $arches; do
    [ "$a" = "$(dpkg --print-architecture 2>/dev/null || echo amd64)" ] && continue
    if ! ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | grep -qi "qemu-aarch64\|qemu-arm"; then
        fail "no qemu binfmt handler registered, so a $a container cannot run here.
Install it with:
  docker run --privileged --rm tonistiigi/binfmt --install all"
    fi
    break
done

for arch in $arches; do
    say "Building $arch in $image"
    printf %s "$container_script" | docker run --rm -i \
        --platform "linux/$arch" \
        -v "$repo:/src" \
        "$image" \
        bash -s -- "$arch" "$jobs" "$check" "$(id -u)" "$(id -g)" \
        2>&1 | while IFS= read -r line; do
            case "$line" in
                CHECK_OK*) echo "  check: ${line#CHECK_OK }" ;;
                *)         echo "  $line" ;;
            esac
        done

    binary=$(ls "$repo"/build-"$arch"/ubersdr-hdradio_* 2>/dev/null | head -1) \
        || fail "$arch: the build produced no binary"
    cp "$binary" "$repo/"
    built="$built $repo/$(basename "$binary")"
done

say "Done"
for b in $built; do
    printf '  %s\n' "$(file -b "$b" | cut -d, -f1-2) — $(basename "$b")"
done

if [ "$publish" = 1 ]; then
    say "Publishing"
    # shellcheck disable=SC2086  # $built is a deliberate whitespace-separated list
    publish_release $built
else
    echo
    echo "Copy to the receiver:"
    echo "  sudo install -d /opt/ubersdr-hdradio"
    echo "  sudo install -m755 ubersdr-hdradio_<arch> /opt/ubersdr-hdradio/"
    echo
    echo "Or upload both to the '$TAG' release, which is what the UberSDR"
    echo "container build downloads:"
    echo "  ./build.sh --publish"
fi
