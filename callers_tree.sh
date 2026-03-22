#!/usr/bin/env bash
# callers_tree.sh — show the full caller tree of a function as an indented tree.
#
# Usage:
#   callers_tree.sh [--db=call_graph.db] [--depth=10] <mangled_name>
#
# Output: indented tree with demangled names, depth-first order.
#
# The --depth limit guards against very deep or cyclic graphs slipping
# past the path-based cycle guard.

set -euo pipefail

DB="call_graph.db"
MAX_DEPTH=10
TARGET=""

for arg in "$@"; do
    case "$arg" in
        --db=*)    DB="${arg#--db=}" ;;
        --depth=*) MAX_DEPTH="${arg#--depth=}" ;;
        -*)        echo "unknown option: $arg" >&2; exit 1 ;;
        *)
            if [[ -z "$TARGET" ]]; then TARGET="$arg"
            else echo "error: unexpected argument '$arg'" >&2; exit 1
            fi
            ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "Usage: $0 [--db=call_graph.db] [--depth=10] <mangled_name>" >&2
    exit 1
fi

if [[ ! -f "$DB" ]]; then
    echo "error: database '$DB' not found (run create_db.sh first)" >&2
    exit 1
fi

sqlite3 "$DB" << SQL
WITH RECURSIVE tree(m, demangled, depth, path) AS (
    SELECT DISTINCT mangled_caller, demangled_caller, 0, mangled_caller
    FROM calls
    WHERE mangled_callee = '$TARGET'

    UNION

    SELECT DISTINCT
        calls.mangled_caller,
        calls.demangled_caller,
        tree.depth + 1,
        tree.path || ' > ' || calls.mangled_caller
    FROM calls
    JOIN tree ON calls.mangled_callee = tree.m
    WHERE tree.depth < $MAX_DEPTH
      AND tree.path NOT LIKE '%' || calls.mangled_caller || '%'
)
SELECT printf('%*s%s', depth*2, '', demangled)
FROM tree
ORDER BY path;
SQL
