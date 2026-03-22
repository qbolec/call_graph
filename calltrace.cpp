// calltrace.cpp
// Extracts (caller, source_path, source_line, callee) tuples from an ELF .o file.
//
// Uses:
//   - elf.h  (glibc, always available on Linux) for ELF/relocation parsing
//   - addr2line (binutils, standard on any dev system) for DWARF line info
//
// Build:
//   g++ -std=c++17 -O2 -o calltrace calltrace.cpp
//
// Usage:
//   calltrace [options] <file.o> [callee_filter]
//
// Options:
//   --no-intra    suppress calls within this TU (static/inlined fns; only visible
//                 when compiled with -ffunction-sections)
//   --no-inter    suppress calls to external symbols (cross-TU)
//   --no-stl      suppress rows where the caller's source is a system header
//                 (path starts with /usr or /opt)
//   --no-dwarf    skip addr2line entirely; emit '-' for file/line (faster)
//
// Output (TSV to stdout):
//   caller_demangled \t source_path \t source_line \t callee_demangled
//
// Notes:
//   - Only statically-resolvable direct calls (via relocations) are captured.
//     Virtual dispatch and calls through function pointers are NOT visible.
//   - Intra-TU calls are only visible when compiled with -ffunction-sections.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <elf.h>
#include <cxxabi.h>

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options {
    bool        no_intra      = false;
    bool        no_inter      = false;
    bool        no_stl        = false;
    bool        no_dwarf      = false;
    const char* obj_path      = nullptr;
    const char* callee_filter = nullptr;
};

static void usage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s [options] <file.o> [callee_filter]\n"
        "\n"
        "Output (TSV):  caller \\t source_path \\t line \\t callee\n"
        "\n"
        "Options:\n"
        "  --no-intra   suppress intra-TU calls (needs -ffunction-sections to be visible)\n"
        "  --no-inter   suppress inter-TU calls (external symbols)\n"
        "  --no-stl     suppress rows where caller source is a system header (/usr, /opt)\n"
        "  --no-dwarf   skip addr2line; emit '-' for file/line (faster)\n"
        "\n"
        "callee_filter  substring matched against the raw mangled callee name\n"
        "               e.g. 'write_row' or '_ZN2DB'\n",
        argv0);
}

static Options parse_args(int argc, char** argv) {
    Options o;
    int i = 1;
    for (; i < argc; i++) {
        std::string_view a = argv[i];
        if      (a == "--no-intra") o.no_intra  = true;
        else if (a == "--no-inter") o.no_inter  = true;
        else if (a == "--no-stl")   o.no_stl    = true;
        else if (a == "--no-dwarf") o.no_dwarf  = true;
        else if (a.substr(0, 2) == "--") {
            fprintf(stderr, "unknown option: %s\n", argv[i]); exit(1);
        }
        else break;
    }
    if (i >= argc) { usage(argv[0]); exit(1); }
    o.obj_path      = argv[i++];
    if (i < argc) o.callee_filter = argv[i];
    return o;
}

// ---------------------------------------------------------------------------
// Demangle cache — each unique mangled name is demangled exactly once
// ---------------------------------------------------------------------------
struct DemangleCache {
    std::unordered_map<std::string, std::string> cache;

    const std::string& get(const std::string& mangled) {
        auto [it, inserted] = cache.emplace(mangled, std::string{});
        if (inserted) {
            int status = 0;
            char* d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
            it->second = (status == 0 && d) ? std::string(d) : mangled;
            free(d);
        }
        return it->second;
    }
};

// ---------------------------------------------------------------------------
// Batch addr2line lookup
//
// Forks addr2line, writes all offsets to its stdin, closes it to signal EOF,
// then reads all output. No temp files, no select(), no interactive protocol.
//
// addr2line emits exactly 2 lines per query (-f guarantees this):
//   line 1: enclosing function name (discarded)
//   line 2: filepath:line  (or ??:0 if unknown)
// ---------------------------------------------------------------------------

struct Location {
    std::string file;
    int         line = 0;
};

// Key: (section_index, offset_within_section)
using LocationMap = std::map<std::pair<int,uint64_t>, Location>;

static void batch_addr2line(
        const char* obj_path,
        const std::string& section_name,   // empty = primary .text, else e.g. ".text._ZN..."
        int section_idx,
        const std::vector<uint64_t>& offsets,
        LocationMap& results)
{
    if (offsets.empty()) return;

    // Two pipes: parent writes queries to addr2line's stdin,
    //            parent reads results from addr2line's stdout.
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in)  != 0) return;
    if (pipe(pipe_out) != 0) { close(pipe_in[0]); close(pipe_in[1]); return; }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_in[0]);  close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        return;
    }

    if (pid == 0) {
        dup2(pipe_in[0],  STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[0]);  close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        // Redirect stderr to /dev/null to suppress addr2line warnings
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        if (!section_name.empty())
            execlp("addr2line", "addr2line",
                   "-e", obj_path, "-j", section_name.c_str(), "-f", "-C", nullptr);
        else
            execlp("addr2line", "addr2line",
                   "-e", obj_path, "-f", "-C", nullptr);
        _exit(1);
    }

    // Parent: close the ends we don't use
    close(pipe_in[0]);
    close(pipe_out[1]);

    // Write all queries then close stdin so addr2line gets EOF and flushes output
    for (uint64_t off : offsets) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "0x%lx\n", (unsigned long)off);
        if (write(pipe_in[1], buf, n) != n) break; // short write: pipe full or closed
    }
    close(pipe_in[1]);

    // Read all output
    FILE* fp = fdopen(pipe_out[0], "r");
    if (!fp) { waitpid(pid, nullptr, 0); return; }

    char func_buf[4096];
    char loc_buf[4096];
    for (uint64_t off : offsets) {
        if (!fgets(func_buf, sizeof func_buf, fp)) break; // function name (discard)
        if (!fgets(loc_buf,  sizeof loc_buf,  fp)) break; // file:line

        loc_buf[strcspn(loc_buf, "\n")] = 0;

        // Strip optional " (discriminator N)" suffix
        char* space = strchr(loc_buf, ' ');
        if (space) *space = 0;

        // Parse "filepath:line"; skip "??:0" (unknown)
        char* colon = strrchr(loc_buf, ':');
        if (colon && loc_buf[0] != '?') {
            *colon = 0;
            results[{section_idx, off}] = { loc_buf, atoi(colon + 1) };
        }
    }

    fclose(fp);
    waitpid(pid, nullptr, 0);
}

// ---------------------------------------------------------------------------
// ELF file view — thin wrapper over the mmap'd bytes
// ---------------------------------------------------------------------------
struct ObjFile {
    const uint8_t* base = nullptr;
    size_t         size = 0;

    const Elf64_Ehdr* ehdr() const {
        return reinterpret_cast<const Elf64_Ehdr*>(base);
    }
    const Elf64_Shdr* shdr(int i) const {
        return reinterpret_cast<const Elf64_Shdr*>(
            base + ehdr()->e_shoff + i * ehdr()->e_shentsize);
    }
    const char* shstrtab() const {
        return reinterpret_cast<const char*>(
            base + shdr(ehdr()->e_shstrndx)->sh_offset);
    }
    std::string_view section_name(int i) const { return shstrtab() + shdr(i)->sh_name; }
    const uint8_t*   section_data(int i) const { return base + shdr(i)->sh_offset; }
};

// ---------------------------------------------------------------------------
// Symbol table: (section_index, byte_offset) -> enclosing function
//
// Only STT_FUNC symbols are stored, sorted in descending order of (shndx, value).
// This lets lower_bound find the enclosing function in one step: it returns the
// first element whose (shndx, value) is <= (query_shndx, query_offset), which
// is exactly the function that starts at or before the given offset.
// ---------------------------------------------------------------------------
struct SymbolTable {
    struct Sym {
        uint64_t    value;
        uint64_t    size;
        std::string name;   // mangled
        int         shndx;

        // Descending order: higher (shndx, value) sorts first
        bool operator<(const Sym& o) const {
            if (shndx != o.shndx) return shndx > o.shndx;
            return value > o.value;
        }
    };
    std::vector<Sym> syms;

    void load(const ObjFile& obj) {
        const Elf64_Ehdr* eh = obj.ehdr();
        for (int i = 0; i < eh->e_shnum; i++) {
            const Elf64_Shdr* sh = obj.shdr(i);
            if (sh->sh_type != SHT_SYMTAB) continue;
            const char* strtab   = reinterpret_cast<const char*>(obj.section_data(sh->sh_link));
            const auto* sym_base = reinterpret_cast<const Elf64_Sym*>(obj.section_data(i));
            size_t n = sh->sh_size / sizeof(Elf64_Sym);
            for (size_t j = 0; j < n; j++) {
                const Elf64_Sym& s = sym_base[j];
                if (ELF64_ST_TYPE(s.st_info) != STT_FUNC) continue;
                syms.push_back({ s.st_value, s.st_size,
                                  strtab + s.st_name, (int)s.st_shndx });
            }
        }
        std::sort(syms.begin(), syms.end());
    }

    // Find the STT_FUNC symbol that contains (shndx, offset).
    const Sym* containing_function(int shndx, uint64_t offset) const {
        Sym key;
        key.shndx = shndx;
        key.value = offset;

        // In descending order, lower_bound finds the first element <= key,
        // i.e. the function with the largest value still <= offset.
        auto it = std::lower_bound(syms.begin(), syms.end(), key);
        if (it == syms.end()) return nullptr;
        if (it->shndx != shndx) return nullptr;
        if (it->size > 0 && offset >= it->value + it->size) return nullptr;
        return &*it;
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Is this relocation type a direct function call on the current architecture?
static bool is_call_reloc(uint32_t rtype) {
    switch (rtype) {
        case R_X86_64_PLT32:   // call to external/PLT symbol
        case R_X86_64_PC32:    // direct PC-relative call (also used for intra-TU)
            return true;
#ifdef R_AARCH64_CALL26
        case R_AARCH64_CALL26: return true;
#endif
        default: return false;
    }
}

// Is this source path a system header? (/usr/include, /opt/...)
static bool is_system_path(const std::string& path) {
    return path.compare(0, 5, "/usr/") == 0 ||
           path.compare(0, 5, "/opt/") == 0;
}

// Is this .text.* section clearly an inlined STL or runtime function?
// Checked before touching addr2line so we pay zero cost for STL sections
// when --no-stl is set.
static bool is_stl_section(std::string_view sname) {
    const std::string_view prefix = ".text.";
    if (sname.substr(0, prefix.size()) != prefix) return false;
    std::string_view sym = sname.substr(prefix.size());
    static const char* stl_prefixes[] = {
        "_ZNSt", "_ZNKSt", "_ZNISt",        // std:: methods
        "_ZN9__gnu_cxx", "_ZNK9__gnu_cxx",   // __gnu_cxx::
        "_ZN11__gnu_debug",                   // __gnu_debug::
        "_ZSt",                               // std:: free functions
        nullptr
    };
    for (const char** p = stl_prefixes; *p; p++)
        if (sym.substr(0, strlen(*p)) == *p) return true;
    return false;
}

// ---------------------------------------------------------------------------
// A call edge collected during the relocation scan
// ---------------------------------------------------------------------------
struct Edge {
    std::string caller_mangled;
    std::string callee_mangled;
    int         section_idx;   // caller's section index (key into location map)
    uint64_t    call_offset;   // byte offset of the call within that section
};

// ---------------------------------------------------------------------------
// Core processing — two passes
// ---------------------------------------------------------------------------
static void process(const Options& opt) {
    int fd = ::open(opt.obj_path, O_RDONLY);
    if (fd < 0) { perror(opt.obj_path); return; }
    struct stat st;
    fstat(fd, &st);
    const uint8_t* base = reinterpret_cast<const uint8_t*>(
        mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    ::close(fd);
    if (base == MAP_FAILED) { perror("mmap"); return; }

    ObjFile obj { base, (size_t)st.st_size };
    const Elf64_Ehdr* eh = obj.ehdr();

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "%s: not an ELF file\n", opt.obj_path); goto done;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "%s: only 64-bit ELF supported\n", opt.obj_path); goto done;
    }
    if (eh->e_type != ET_REL) {
        fprintf(stderr, "%s: not a relocatable (.o) file\n", opt.obj_path); goto done;
    }

    {
        SymbolTable symtab;
        symtab.load(obj);

        // -----------------------------------------------------------------
        // Pass 1: scan RELA sections, collect call edges and DWARF queries
        // -----------------------------------------------------------------
        std::vector<Edge> edges;
        std::unordered_map<int, std::vector<uint64_t>> dwarf_queries; // section_idx -> offsets

        for (int i = 0; i < eh->e_shnum; i++) {
            const Elf64_Shdr* rsh = obj.shdr(i);
            if (rsh->sh_type != SHT_RELA) continue;

            int target_shidx = (int)rsh->sh_info;
            if (target_shidx <= 0 || target_shidx >= eh->e_shnum) continue;

            std::string_view tname = obj.section_name(target_shidx);
            if (tname.find(".text") == std::string_view::npos) continue;
            if (opt.no_stl && is_stl_section(tname)) continue;

            const Elf64_Shdr* symsh = obj.shdr(rsh->sh_link);
            const auto* sym_base    = reinterpret_cast<const Elf64_Sym*>(obj.section_data(rsh->sh_link));
            const char* strtab      = reinterpret_cast<const char*>(obj.section_data(symsh->sh_link));

            const auto* relas = reinterpret_cast<const Elf64_Rela*>(obj.section_data(i));
            size_t nrela = rsh->sh_size / sizeof(Elf64_Rela);

            for (size_t j = 0; j < nrela; j++) {
                const Elf64_Rela& r = relas[j];
                if (!is_call_reloc(ELF64_R_TYPE(r.r_info))) continue;

                uint32_t         sym_idx = ELF64_R_SYM(r.r_info);
                const Elf64_Sym& esym    = sym_base[sym_idx];
                uint8_t          stype   = ELF64_ST_TYPE(esym.st_info);

                std::string callee_mangled;

                if (stype == STT_SECTION) {
                    // Intra-TU call to a local/static function in another section.
                    // With -ffunction-sections, the compiler emits a section-relative
                    // relocation (STT_SECTION target) rather than a named-symbol one.
                    // The target section holds exactly one function; find it at offset 0.
                    // (The PC32 addend is always -4; corrected target offset = addend+4 = 0.)
                    if (opt.no_intra) continue;
                    int target_fn_shndx = (int)esym.st_shndx;
                    if (obj.section_name(target_fn_shndx).find(".text") == std::string_view::npos)
                        continue; // data reference (e.g. .rodata), not a call
                    uint64_t target_offset = (uint64_t)(r.r_addend + 4);
                    const SymbolTable::Sym* callee_sym =
                        symtab.containing_function(target_fn_shndx, target_offset);
                    if (!callee_sym) continue;
                    callee_mangled = callee_sym->name;

                } else if (stype == STT_OBJECT || esym.st_name == 0) {
                    continue; // data reference, not a function call

                } else {
                    // Named symbol — inter-TU if SHN_UNDEF, intra-TU if defined here.
                    // (With -ffunction-sections, non-static functions defined in this
                    // .o get their own sections and are referenced by name with WEAK
                    // binding, not via STT_SECTION relocations.)
                    bool is_external = (esym.st_shndx == SHN_UNDEF);
                    if ( is_external && opt.no_inter) continue;
                    if (!is_external && opt.no_intra) continue;
                    callee_mangled = strtab + esym.st_name;
                }

                if (callee_mangled.empty()) continue;
                if (opt.callee_filter &&
                    callee_mangled.find(opt.callee_filter) == std::string::npos)
                    continue;

                uint64_t call_offset = r.r_offset;

                const SymbolTable::Sym* caller_sym =
                    symtab.containing_function(target_shidx, call_offset);
                if (!caller_sym) continue;

                edges.push_back({ caller_sym->name, callee_mangled,
                                   target_shidx, call_offset });

                if (!opt.no_dwarf)
                    dwarf_queries[target_shidx].push_back(call_offset);
            }
        }

        // -----------------------------------------------------------------
        // Pass 2: batch addr2line queries, then emit TSV rows
        // -----------------------------------------------------------------
        LocationMap locations;

        for (auto& [shidx, offsets] : dwarf_queries) {
            std::string sname(obj.section_name(shidx));
            if (opt.no_stl && is_stl_section(sname)) continue;

            // Deduplicate — same call site can appear in multiple edges
            std::sort(offsets.begin(), offsets.end());
            offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

            // Primary .text needs no -j flag; split sections do
            std::string jflag = (sname == ".text") ? "" : sname;

            batch_addr2line(opt.obj_path, jflag, shidx, offsets, locations);
        }

        DemangleCache dcache;

        for (const Edge& e : edges) {
            std::string src_file = "-";
            int         src_line = 0;

            if (!opt.no_dwarf) {
                auto it = locations.find({e.section_idx, e.call_offset});
                if (it != locations.end() && !it->second.file.empty()) {
                    src_file = it->second.file;
                    src_line = it->second.line;
                }
            }

            if (opt.no_stl && is_system_path(src_file)) continue;

            printf("%s\t%s\t%d\t%s\n",
                   dcache.get(e.caller_mangled).c_str(),
                   src_file.c_str(),
                   src_line,
                   dcache.get(e.callee_mangled).c_str());
        }
    }

done:
    munmap((void*)base, st.st_size);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    process(opt);
    return 0;
}
