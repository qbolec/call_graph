sqlite3 calls.db <<'SQL'
CREATE TABLE calls(
    mangled_caller    TEXT,
    demangled_caller  TEXT,
    path              TEXT,
    line              INTEGER,
    demangled_callee  TEXT,
    mangled_callee    TEXT
);
.separator "\t"
.import newer.fs calls
CREATE INDEX idx_callee ON calls(mangled_callee);

WITH RECURSIVE tree(m, demangled, depth, path) AS (
    -- roots: direct callers of the target
    SELECT DISTINCT mangled_caller, demangled_caller, 0, mangled_caller
    FROM calls WHERE mangled_callee = 'stderr'
    UNION
    -- recursive step: callers of nodes already in tree
    SELECT DISTINCT calls.mangled_caller, calls.demangled_caller,
                    tree.depth + 1,
                    tree.path || ' > ' || calls.mangled_caller
    FROM calls JOIN tree ON calls.mangled_callee = tree.m
    -- guard against cycles (function calling itself, mutual recursion)
    WHERE tree.path NOT LIKE '%' || calls.mangled_caller || '%'
)
SELECT printf('%*s%s', depth*2, '', demangled) FROM tree ORDER BY path;
SQL