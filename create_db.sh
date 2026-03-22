#!/usr/bin/env bash
# create_db.sh — scan a build directory for .o files, extract call edges,
# and import them into a SQLite database.
#
# Usage:
#   create_db.sh [--db=call_graph.db] [--no-dwarf] <build_dir>
#
# Options:
#   --db=<path>   path to the SQLite database to create (default: call_graph.db)
#   --no-dwarf    skip source location lookup (faster but no path/line columns)
#
# The callgraph binary is looked for in:
#   1. Same directory as this script
#   2. PATH

set -euo pipefail

DB="call_graph.db"
NO_DWARF=""
BUILD_DIR=""

for arg in "$@"; do
    case "$arg" in
        --db=*)      DB="${arg#--db=}" ;;
        --no-dwarf)  NO_DWARF="--no-dwarf" ;;
        -*)          echo "unknown option: $arg" >&2; exit 1 ;;
        *)           BUILD_DIR="$arg" ;;
    esac
done

if [[ -z "$BUILD_DIR" ]]; then
    echo "Usage: $0 [--db=call_graph.db] [--no-dwarf] <build_dir>" >&2
    exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: '$BUILD_DIR' is not a directory" >&2
    exit 1
fi

# Locate callgraph
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [[ -x "$SCRIPT_DIR/callgraph" ]]; then
    CALLGRAPH="$SCRIPT_DIR/callgraph"
elif command -v callgraph &>/dev/null; then
    CALLGRAPH="$(command -v callgraph)"
else
    echo "error: callgraph not found next to this script or on PATH" >&2
    exit 1
fi

echo "Using callgraph: $CALLGRAPH" >&2
echo "Build dir:       $BUILD_DIR" >&2
echo "Database:        $DB" >&2

N=$(find "$BUILD_DIR" -name '*.o' | wc -l)
echo "Found $N object files" >&2

NPROC=$(nproc)
SLOT_DIR=$(mktemp -d)
trap 'rm -rf "$SLOT_DIR"' EXIT

echo "Extracting call edges (${NPROC} parallel workers)..." >&2

# --process-slot-var gives each worker a unique index 0..(NPROC-1).
# Each worker appends only to its own slot file — no interleaving possible.
# Without -I{}, xargs batches multiple files per invocation; the sh -c
# loop processes them all, always appending to the same slot file.
find "$BUILD_DIR" -name '*.o' -print0 \
    | xargs -0 -P"$NPROC" -I{} --process-slot-var=SLOT \
        sh -c '"$1" --no-stl $4 "$2" >> "$3/slot_$SLOT.tsv" 2>/dev/null' \
        _ "$CALLGRAPH" {} "$SLOT_DIR" "$NO_DWARF"

TSV="$SLOT_DIR/all.tsv"
cat "$SLOT_DIR"/slot_*.tsv > "$TSV"

ROWS=$(wc -l < "$TSV")
echo "Extracted $ROWS edges" >&2

echo "Importing into $DB..." >&2
rm -f "$DB"
sqlite3 "$DB" << SQL
CREATE TABLE calls(
    mangled_caller   TEXT,
    demangled_caller TEXT,
    path             TEXT,
    line             INTEGER,
    demangled_callee TEXT,
    mangled_callee   TEXT
);
.separator "\t"
.import $TSV calls
CREATE INDEX idx_callee  ON calls(mangled_callee);
CREATE INDEX idx_caller  ON calls(mangled_caller);
SQL

echo "Done. Database: $DB ($ROWS rows)" >&2
