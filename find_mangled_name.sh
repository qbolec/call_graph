#!/usr/bin/env bash
# find_mangled_name.sh — search .o files for functions matching a name pattern,
# optionally filtered by source path suffix.
#
# Usage:
#   find_mangled_name.sh [--suffix=<path_suffix>] <build_dir> <pattern>
#
# <pattern> is a regex matched against mangled OR demangled function name.
# --suffix  filters results to functions whose source path ends with the given string
#           e.g. --suffix=lock/lock0lock.cc
#
# Output (TSV):
#   mangled_name \t source_path \t source_line \t demangled_name

set -euo pipefail

SUFFIX=""
BUILD_DIR=""
PATTERN=""

for arg in "$@"; do
    case "$arg" in
        --suffix=*) SUFFIX="${arg#--suffix=}" ;;
        -*)         echo "unknown option: $arg" >&2; exit 1 ;;
        *)
            if [[ -z "$BUILD_DIR" ]]; then BUILD_DIR="$arg"
            elif [[ -z "$PATTERN" ]]; then PATTERN="$arg"
            else echo "error: unexpected argument '$arg'" >&2; exit 1
            fi
            ;;
    esac
done

if [[ -z "$BUILD_DIR" || -z "$PATTERN" ]]; then
    echo "Usage: $0 [--suffix=<path_suffix>] <build_dir> <pattern>" >&2
    echo "" >&2
    echo "  pattern   regex matched against mangled or demangled function name" >&2
    echo "  --suffix  only show functions whose source path ends with this string" >&2
    exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: '$BUILD_DIR' is not a directory" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [[ -x "$SCRIPT_DIR/functions" ]]; then
    FUNCTIONS="$SCRIPT_DIR/functions"
elif command -v functions &>/dev/null; then
    FUNCTIONS="$(command -v functions)"
else
    echo "error: functions not found next to this script or on PATH" >&2
    exit 1
fi

NPROC=$(nproc)
SLOT_DIR=$(mktemp -d)
trap 'rm -rf "$SLOT_DIR"' EXIT

# Each worker appends only to its own slot file — no interleaving possible.
find "$BUILD_DIR" -name '*.o' -print0 \
    | xargs -0 -P"$NPROC" -I{} --process-slot-var=SLOT \
        sh -c '"$1" --no-stl --function-filter "$2" "$3" >> "$4/slot_$SLOT.tsv" 2>/dev/null' \
        _ "$FUNCTIONS" "$PATTERN" {} "$SLOT_DIR"

cat "$SLOT_DIR"/slot_*.tsv 2>/dev/null \
    | if [[ -n "$SUFFIX" ]]; then
        awk -F'\t' -v suffix="$SUFFIX" 'index($2, suffix) == length($2) - length(suffix) + 1'
      else
        cat
      fi \
    | sort -u
