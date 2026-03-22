// common.h — shared ELF/DWARF utilities for calltrace and functions
//
// Requires linking with nothing beyond libc and libstdc++.
// addr2line (binutils) must be on PATH for DWARF lookups.

#pragma once

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
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// ELF file view — thin wrapper over mmap'd bytes
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

// Open, mmap, and validate a 64-bit relocatable ELF file.
// On success returns true and fills `obj`; on failure prints to stderr and returns false.
inline bool open_obj(const char* path, ObjFile& obj, uint8_t*& mmapped_base) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) { perror(path); return false; }
    struct stat st;
    fstat(fd, &st);
    mmapped_base = reinterpret_cast<uint8_t*>(
        mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    ::close(fd);
    if (mmapped_base == MAP_FAILED) { perror("mmap"); return false; }

    obj = { mmapped_base, (size_t)st.st_size };
    const Elf64_Ehdr* eh = obj.ehdr();

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "%s: not an ELF file\n", path);
    } else if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "%s: only 64-bit ELF supported\n", path);
    } else if (eh->e_type != ET_REL) {
        fprintf(stderr, "%s: not a relocatable (.o) file\n", path);
    } else {
        return true;
    }
    munmap(mmapped_base, st.st_size);
    return false;
}

// ---------------------------------------------------------------------------
// Symbol table: (section_index, byte_offset) -> enclosing function
//
// Only STT_FUNC symbols stored, sorted descending by (shndx, value) so that
// lower_bound finds the enclosing function in one step.
// ---------------------------------------------------------------------------
struct SymbolTable {
    struct Sym {
        uint64_t    value;
        uint64_t    size;
        std::string name;   // mangled
        int         shndx;

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

    const Sym* containing_function(int shndx, uint64_t offset) const {
        Sym key; key.shndx = shndx; key.value = offset;
        auto it = std::lower_bound(syms.begin(), syms.end(), key);
        if (it == syms.end()) return nullptr;
        if (it->shndx != shndx) return nullptr;
        if (it->size > 0 && offset >= it->value + it->size) return nullptr;
        return &*it;
    }

    // Return [begin, end) iterators for all STT_FUNC symbols in section shndx,
    // in descending order of value (highest offset first).
    // Runs two binary searches — O(log n) total.
    std::pair<std::vector<Sym>::const_iterator,
              std::vector<Sym>::const_iterator>
    section_range(int shndx) const {
        // In our descending ordering, section shndx starts where shndx+1 ends.
        // lower_bound with (shndx, MAX_VALUE) finds the first element of shndx.
        Sym lo_key; lo_key.shndx = shndx; lo_key.value = UINT64_MAX;
        Sym hi_key; hi_key.shndx = shndx; hi_key.value = 0;
        // First element of this section (highest value): lower_bound of lo_key
        auto begin = std::lower_bound(syms.begin(), syms.end(), lo_key);
        // First element past this section (shndx-1): lower_bound of hi_key
        // but we want strictly past, so use the element after the last value==0
        // Simpler: upper_bound with (shndx, 0) using our comparator
        // Since operator< is descending, upper_bound finds first element < key,
        // i.e. first element with value < 0 (impossible) or shndx < shndx (next section).
        // Instead just scan: end = lower_bound(syms.begin(), syms.end(), hi_key)
        // then step past any at value==0 with same shndx.
        // Cleanest: search for first element where shndx < our shndx in descending order.
        Sym end_key; end_key.shndx = shndx - 1; end_key.value = UINT64_MAX;
        auto end = std::lower_bound(syms.begin(), syms.end(), end_key);
        return {begin, end};
    }
};

// ---------------------------------------------------------------------------
// Batch addr2line lookup
//
// Forks addr2line once per section, writes all offsets to its stdin, closes
// stdin to signal EOF, reads all output. No temp files, no select().
//
// addr2line emits exactly 2 lines per query when invoked with -f:
//   line 1: function name (discarded — we get names from the symbol table)
//   line 2: filepath:line  (or ??:0 if unknown)
// ---------------------------------------------------------------------------
struct Location {
    std::string file;
    int         line = 0;
};

// Key: (section_index, offset_within_section)
using LocationMap = std::map<std::pair<int,uint64_t>, Location>;

inline void batch_addr2line(
        const char* obj_path,
        const std::string& section_name,  // empty = primary .text
        int section_idx,
        const std::vector<uint64_t>& offsets,
        LocationMap& results)
{
    if (offsets.empty()) return;

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
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        if (!section_name.empty())
            execlp("addr2line", "addr2line",
                   "-e", obj_path, "-j", section_name.c_str(), "-f", "-C", nullptr);
        else
            execlp("addr2line", "addr2line", "-e", obj_path, "-f", "-C", nullptr);
        _exit(1);
    }

    close(pipe_in[0]);
    close(pipe_out[1]);

    for (uint64_t off : offsets) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "0x%lx\n", (unsigned long)off);
        if (write(pipe_in[1], buf, n) != n) break;
    }
    close(pipe_in[1]);

    FILE* fp = fdopen(pipe_out[0], "r");
    if (!fp) { waitpid(pid, nullptr, 0); return; }

    char func_buf[4096], loc_buf[4096];
    for (uint64_t off : offsets) {
        if (!fgets(func_buf, sizeof func_buf, fp)) break;
        if (!fgets(loc_buf,  sizeof loc_buf,  fp)) break;
        loc_buf[strcspn(loc_buf, "\n")] = 0;
        // Strip optional " (discriminator N)" suffix
        char* space = strchr(loc_buf, ' ');
        if (space) *space = 0;
        // Parse "filepath:line"; skip "??:0"
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
// Demangle cache — each unique mangled name demangled exactly once
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
// Matching — exact equality or regex against mangled OR demangled name
// ---------------------------------------------------------------------------

// Exact: nullptr means "no constraint"
inline bool matches_exact(const char* exact, const std::string& name) {
    return exact == nullptr || name == exact;
}

// Regex: matches against mangled name first, then demangled if needed.
// Compiles and caches the regex on first call per unique pattern pointer.
inline bool matches_filter(const char* pattern, const std::string& mangled) {
    if (pattern == nullptr) return true;
    static const char* last_pattern = nullptr;
    static std::regex  last_re;
    if (pattern != last_pattern) {
        last_re      = std::regex(pattern);
        last_pattern = pattern;
    }
    if (std::regex_search(mangled, last_re)) return true;
    int status = 0;
    char* d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    bool result = (status == 0 && d) && std::regex_search(d, last_re);
    free(d);
    return result;
}

// ---------------------------------------------------------------------------
// ELF / section helpers
// ---------------------------------------------------------------------------

inline bool is_call_reloc(uint32_t rtype) {
    switch (rtype) {
        case R_X86_64_PLT32:
        case R_X86_64_PC32:
            return true;
#ifdef R_AARCH64_CALL26
        case R_AARCH64_CALL26: return true;
#endif
        default: return false;
    }
}

inline bool is_system_path(const std::string& path) {
    return path.compare(0, 5, "/usr/") == 0 ||
           path.compare(0, 5, "/opt/") == 0;
}

inline bool is_stl_section(std::string_view sname) {
    const std::string_view prefix = ".text.";
    if (sname.substr(0, prefix.size()) != prefix) return false;
    std::string_view sym = sname.substr(prefix.size());
    static const char* stl_prefixes[] = {
        "_ZNSt", "_ZNKSt", "_ZNISt",
        "_ZN9__gnu_cxx", "_ZNK9__gnu_cxx",
        "_ZN11__gnu_debug",
        "_ZSt",
        nullptr
    };
    for (const char** p = stl_prefixes; *p; p++)
        if (sym.substr(0, strlen(*p)) == *p) return true;
    return false;
}
