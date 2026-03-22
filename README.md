# callgraph

A toolkit for extracting call graphs from ELF object files (`.o`) produced by
`g++` or `clang++`. Given a build directory, it can tell you — for any function
— who calls it, and who calls *those* callers, all the way up to `main`.

---

## Concept

When the compiler translates C++ source to machine code it leaves behind
*relocation entries* in each `.o` file. A relocation entry for a function call
records: *"at byte offset X in this section, there is a call to symbol Y"*.
These entries are normally consumed and discarded by the linker, but they are
present in the `.o` files and contain everything needed to reconstruct the call
graph — caller function, callee function, and (with DWARF debug info) the
source file and line number of the call site.

`callgraph` reads these entries directly from `.o` files using the standard ELF
format, resolves symbol names from the symbol table, and maps call sites to
source locations via `addr2line`. No instrumentation, no special build plugins,
no running the program.

---

## Building the tools

```bash
mkdir build && cd build
cmake ..
make
```

Or without CMake:

```bash
g++ -std=c++17 -O2 -o callgraph callgraph.cpp
g++ -std=c++17 -O2 -o functions functions.cpp
```

The only dependency is `addr2line` from GNU binutils, which is standard on any
Linux development machine.
Running the tools with `--no-dwarf` option, doesn't require `addr2line`, though.

---

## Compiling your project for best results

### Enable debug info

Without debug info, `callgraph` can still find all call edges but cannot map
them back to source file and line number. To enable it:

```bash
# Plain CMake project
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..

# MySQL Server specifically
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-ffunction-sections" \
      -DCMAKE_C_FLAGS="-ffunction-sections" ..

# Or with explicit -g flag
cmake -DCMAKE_CXX_FLAGS="-g -ffunction-sections" \
      -DCMAKE_C_FLAGS="-g -ffunction-sections" ..
```

### Enable `-ffunction-sections`

By default the compiler puts all functions from a translation unit into a single
`.text` section. Calls between functions in the *same* section have no
relocation entry — they are resolved at compile time and `callgraph` will have to
use less reliable way to find the calls (see below).

With `-ffunction-sections` each function gets its own section
(`.text.FunctionName`), and all intra-file calls become visible as relocations.

This flag also enables `--gc-sections` dead code elimination in the linker, so
it is commonly used in release builds anyway.

### What if you can't recompile?

`callgraph` still works without these flags:

- **Without `-ffunction-sections`**: cross-file calls (the majority in large 
  projects) are fully captured as usual, but for calls within same compilation unit,
  `callgraph` has to get clever: it falls back to scanning for `e8` (x86-64 direct
  `call`) opcodes for sections containing multiple functions, which recovers most
  intra-file calls as a best-effort heuristic. In can produce false-positives, if
  by chance the binary contained a sequence of `e8` followed by four bytes which
  happen to be exact relative address of another function.
- **Without `-g`**: call edges are found but source paths and line numbers show
  as `-` and `0`. Use `--no-dwarf` to skip the `addr2line` lookup entirely and
  run much faster.

---

## Low-level tools

### `functions` — list functions defined in a `.o` file

```
functions [options] <file.o>

Output (TSV):  mangled_name \t source_path \t source_line \t demangled_name

Options:
  --no-dwarf             skip addr2line; emit '-' for file/line (faster)
  --no-stl               suppress functions from system headers (/usr, /opt)
  --function <str>       only show functions where mangled name == <str> (exact)
  --function-filter <re> only show functions where mangled name matches regex
```

**Example: find the mangled name of `lock_rec_lock`**

```bash
$ ./functions mysql-bin/storage/innobase/CMakeFiles/innobase.dir/lock/lock0lock.cc.o \
      --function-filter "lock_rec_lock"
_ZL13lock_rec_lockb11select_modemPK11buf_block_tmP12dict_index_tP9que_thr_t mysql-server/storage/innobase/lock/lock0lock.cc  1866  lock_rec_lock(bool, select_mode, unsigned long, buf_block_t const*, unsigned long, dict_index_t*, que_thr_t*)
```

**Example: find by source file and line number**

```bash
$ ./functions sql/handler.cc.o | grep -P "handler.cc\t4521"
```

**Example: list all non-STL functions in a `.o` file**

```bash
$ ./functions --no-stl sql/handler.cc.o
```

---

### `callgraph` — find call edges in a `.o` file

```
callgraph [options] <file.o>

Output (TSV):
  mangled_caller \t demangled_caller \t source_path \t source_line \t demangled_callee \t mangled_callee

Options:
  --no-intra           suppress intra-TU calls (only visible with -ffunction-sections)
  --no-inter           suppress inter-TU calls (external symbols)
  --no-stl             suppress rows where caller source is a system header (/usr, /opt)
  --no-dwarf           skip addr2line; emit '-' for file/line (faster)
  --callee <str>       only rows where mangled callee == <str> (exact)
  --callee-filter <re> only rows where mangled callee matches regex
  --caller <str>       only rows where mangled caller == <str> (exact)
  --caller-filter <re> only rows where mangled caller matches regex
```

**Example: find all callers of `lock_rec_lock` in one `.o` file**

```bash
$ ./callgraph --no-stl \
      --callee _ZL13lock_rec_lockb11select_modemPK11buf_block_tmP12dict_index_tP9que_thr_t \
      storage/innobase/lock/lock0lock.cc.o

_ZL30lock_sec_rec_modify_check...  lock_sec_rec_modify_check_and_lock(...)  lock0lock.cc  5425  lock_rec_lock(...)  _ZL13lock_rec_lock...
_ZL32lock_clust_rec_modify_check...  lock_clust_rec_modify_check_and_lock(...)  lock0lock.cc  5370  lock_rec_lock(...)  _ZL13lock_rec_lock...
```

**Example: find calls using a human-readable regex **

```bash
$ ./callgraph --no-stl --callee-filter "lock_rec_lock" lock0lock.cc.o
```

Note: `--callee-filter` is a regexp which matches against both the mangled name, 
which typically contains the function name as a substring, so using "lock_rec_lock"
here achieves the goal. `--callee` requires an exact match against
the mangled name — use it when you know the mangled name, and want exclude accidental
matches such as `.cold` compiler splits or other variants that share a demangled name
prefix.

**Example: combine caller and callee filters**

```bash
$ ./callgraph --no-stl \
      --caller-filter "ha_innobase" \
      --callee-filter "lock_rec_lock" \
      ha_innodb.cc.o
```

### `--no-dwarf`: fast mode

`addr2line` is invoked once per section to resolve call-site offsets to source
file and line number. On a large `.o` file with thousands of call sites this
adds up. Use `--no-dwarf` to skip it entirely — source path and line will show
as `-` and `0`, but all call edges are still found:

```bash
$ ./callgraph --no-stl --no-dwarf ha_innodb.cc.o | wc -l   # instant
$ ./callgraph --no-stl ha_innodb.cc.o | wc -l              # slower, has locations
```

### `--no-stl`: suppress standard library noise

With `-ffunction-sections`, every inlined STL method (iterators, `std::string`,
`std::vector` internals, etc.) gets its own section and its own call edges.
These are rarely interesting when tracing application logic and generate enormous
noise. `--no-stl` suppresses any caller whose source path starts with `/usr/`
or `/opt/`, and also skips those sections entirely before spawning `addr2line`,
making it significantly faster than filtering after the fact.

For exploring application call graphs, **always use `--no-stl`**.

---

## Higher-level scripts

### `create_db.sh` — build a SQLite call graph database

Scans an entire build directory in parallel and imports all edges into a SQLite
database for fast recursive queries.

```
create_db.sh [--db=call_graph.db] [--no-dwarf] <build_dir>
```

```bash
$ ./create_db.sh --db=mysql.db /path/to/mysql/build
Using callgraph: /home/user/callgraph/callgraph
Build dir:       /path/to/mysql/build
Database:        mysql.db
Found 4721 object files
Extracting call edges (16 parallel workers)...
Extracted 2847392 edges
Importing into mysql.db...
Done. Database: mysql.db (2847392 rows)
```

The database has a `calls` table with columns:
`mangled_caller`, `demangled_caller`, `path`, `line`, `demangled_callee`, `mangled_callee`

**`--no-dwarf` is orders of magnitude faster** (minutes vs hours on a large
codebase) but the `path` and `line` columns will be empty for all rows. Use it
for a first exploratory pass; rebuild with DWARF when you need source locations.

---

### `find_mangled_name.sh` — search for functions across a build

```
find_mangled_name.sh [--suffix=<path_suffix>] <build_dir> <pattern>
```

`pattern` is a regex matched against the mangled name.
Use `--suffix` to restrict results to a specific source file.

```bash
# Find all functions with "lock_rec_lock" in the name
$ ./find_mangled_name.sh /path/to/mysql/build "lock_rec_lock"

_ZL13lock_rec_lockb11select_modemPK11buf_block_tmP12dict_index_tP9que_thr_t \
    .../storage/innobase/lock/lock0lock.cc  4521 \
    lock_rec_lock(bool, select_mode, ...)

# Restrict to a specific source file
$ ./find_mangled_name.sh \
      --suffix=storage/innobase/lock/lock0lock.cc \
      /path/to/mysql/build \
      "lock_rec_lock"
```

---

### `callers_tree.sh` — show the full caller tree as an indented tree

```
callers_tree.sh [--db=call_graph.db] [--depth=20] <mangled_name>
```

Runs a recursive SQL query against the database to find all callers, their
callers, and so on, displaying the result as an indented tree in depth-first
order.

```bash
$ ./callers_tree.sh --db=mysql.db --depth=6 \
      _ZL13lock_rec_lockb11select_modemPK11buf_block_tmP12dict_index_tP9que_thr_t

lock_sec_rec_modify_check_and_lock(...)
  btr_cur_update_in_place(...)
    btr_cur_optimistic_update(...)
      row_upd_clust_rec(...)
        row_upd_clust_step(...)
          ...
lock_clust_rec_modify_check_and_lock(...)
  btr_cur_update_in_place(...)
    ...
```

**`--depth` warning**: the default depth limit is 10. Increasing it beyond that
risks an exponential blowup for functions that are called from many places —
the query time can grow into minutes or hours. The cycle guard (`path NOT LIKE`)
prevents infinite loops but not combinatorial explosion. For deep exploration,
increase depth gradually:

```bash
$ ./callers_tree.sh --db=mysql.db --depth=5  _ZL13lock_rec_lock...   # safe
$ ./callers_tree.sh --db=mysql.db --depth=10 _ZL13lock_rec_lock...   # usually fine
$ ./callers_tree.sh --db=mysql.db --depth=15 _ZL13lock_rec_lock...   # may be slow
```

---

## Typical workflow

```bash
# 1. Build your project with debug info and -ffunction-sections
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-ffunction-sections" \
      -DCMAKE_C_FLAGS="-ffunction-sections" ..
make -j$(nproc)

# 2. Quick pass: build a no-DWARF database to explore structure
./create_db.sh --db=mysql_fast.db --no-dwarf /path/to/build

# 3. Find the mangled name of the function you care about
./find_mangled_name.sh \
    --suffix=storage/innobase/lock/lock0lock.cc \
    /path/to/build \
    "lock_rec_lock"

# 4. Explore the caller tree
./callers_tree.sh --db=mysql_fast.db _ZL13lock_rec_lock...

# 5. Rebuild with DWARF for source locations when needed
./create_db.sh --db=mysql_full.db /path/to/build

# 6. Query with source locations
sqlite3 mysql_full.db "SELECT DISTINCT path, line, demangled_caller
    FROM calls WHERE mangled_callee = '_ZL13lock_rec_lock...'
    ORDER BY path, line;"
```

---

## Limitations

- **Virtual dispatch and function pointers** are not captured — only direct
  calls visible as ELF relocations.
- **Intra-section calls** (same `.o`, no `-ffunction-sections`) are recovered
  via a best-effort `e8` opcode scan on x86-64, which may miss some calls or
  produce rare false positives.
- **Calls through `dlopen`/plugins** are invisible to static analysis.
- Only **64-bit ELF** (x86-64, AArch64) is supported.
