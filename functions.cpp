// functions.cpp
// Lists all functions defined in an ELF .o file as TSV:
//   mangled_name \t source_path \t source_line \t demangled_name
//
// Uses:
//   - elf.h  (glibc, always available on Linux) for ELF parsing
//   - addr2line (binutils) for DWARF source location lookup
//
// Build:
//   g++ -std=c++17 -O2 -o functions functions.cpp
//
// Usage:
//   functions [--no-dwarf] <file.o> [filter]
//
//   filter  optional substring matched against the mangled name
//
// Typical use: find the mangled name of a function you know by source location
//   functions foo.o | grep "foo.cpp:42"
//   functions foo.o | grep "my_function_name"

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
#include <regex>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Demangle
// ---------------------------------------------------------------------------
static std::string demangle(const std::string& mangled) {
    int status = 0;
    char* d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status == 0 && d) {
        std::string r(d);
        free(d);
        return r;
    }
    return mangled;
}

// ---------------------------------------------------------------------------
// ELF file view
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
// Batch addr2line: given a section and a list of offsets (one per function,
// typically 0 since each function starts at the beginning of its section
// with -ffunction-sections, or the symbol's st_value otherwise),
// returns a map of offset -> {file, line}.
// ---------------------------------------------------------------------------
struct Location {
    std::string file;
    int         line = 0;
};

static void batch_addr2line(
        const char* obj_path,
        const std::string& section_name,  // empty = primary .text
        const std::vector<uint64_t>& offsets,
        std::unordered_map<uint64_t, Location>& results)
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
            execlp("addr2line", "addr2line",
                   "-e", obj_path, "-f", "-C", nullptr);
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

    char func_buf[4096];
    char loc_buf[4096];
    for (uint64_t off : offsets) {
        if (!fgets(func_buf, sizeof func_buf, fp)) break;
        if (!fgets(loc_buf,  sizeof loc_buf,  fp)) break;

        loc_buf[strcspn(loc_buf, "\n")] = 0;

        char* space = strchr(loc_buf, ' ');
        if (space) *space = 0;

        char* colon = strrchr(loc_buf, ':');
        if (colon && loc_buf[0] != '?') {
            *colon = 0;
            results[off] = { loc_buf, atoi(colon + 1) };
        }
    }

    fclose(fp);
    waitpid(pid, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    bool        no_dwarf      = false;
    const char* obj_path      = nullptr;
    const char* exact_filter  = nullptr;   // --filter: exact match
    const char* regex_filter  = nullptr;   // --filter-re: regex match

    for (int i = 1; i < argc; i++) {
        std::string_view a = argv[i];
        if (a == "--no-dwarf") no_dwarf = true;
        else if (a == "--filter") {
            if (++i >= argc) { fprintf(stderr, "--filter requires an argument\n"); return 1; }
            exact_filter = argv[i];
        }
        else if (a == "--filter-re") {
            if (++i >= argc) { fprintf(stderr, "--filter-re requires an argument\n"); return 1; }
            regex_filter = argv[i];
        }
        else if (a == "--help" || a == "-h") {
            fprintf(stderr,
                "Usage: %s [--no-dwarf] [--filter <str>] [--filter-re <re>] <file.o>\n"
                "\n"
                "Output (TSV):  mangled_name \\t source_path \\t source_line \\t demangled_name\n"
                "\n"
                "  --filter <str>    only show functions where mangled name == <str> (exact)\n"
                "  --filter-re <re>  only show functions where mangled name matches regex\n",
                argv[0]);
            return 0;
        }
        else if (a.substr(0, 2) == "--") {
            fprintf(stderr, "unknown option: %s\n", argv[i]); return 1;
        }
        else if (!obj_path) obj_path = argv[i];
    }

    if (!obj_path) {
        fprintf(stderr, "Usage: %s [--no-dwarf] [--filter <str>] [--filter-re <re>] <file.o>\n", argv[0]);
        return 1;
    }

    int fd = ::open(obj_path, O_RDONLY);
    if (fd < 0) { perror(obj_path); return 1; }
    struct stat st;
    fstat(fd, &st);
    const uint8_t* base = reinterpret_cast<const uint8_t*>(
        mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    ::close(fd);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    ObjFile obj { base, (size_t)st.st_size };
    const Elf64_Ehdr* eh = obj.ehdr();

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "%s: not an ELF file\n", obj_path); return 1;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "%s: only 64-bit ELF supported\n", obj_path); return 1;
    }
    if (eh->e_type != ET_REL) {
        fprintf(stderr, "%s: not a relocatable (.o) file\n", obj_path); return 1;
    }

    // Collect all STT_FUNC symbols
    struct FuncSym {
        std::string  name;      // mangled
        int          shndx;     // section index
        uint64_t     value;     // offset within section
    };
    std::vector<FuncSym> funcs;

    for (int i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr* sh = obj.shdr(i);
        if (sh->sh_type != SHT_SYMTAB) continue;
        const char* strtab   = reinterpret_cast<const char*>(obj.section_data(sh->sh_link));
        const auto* sym_base = reinterpret_cast<const Elf64_Sym*>(obj.section_data(i));
        size_t n = sh->sh_size / sizeof(Elf64_Sym);
        for (size_t j = 0; j < n; j++) {
            const Elf64_Sym& s = sym_base[j];
            if (ELF64_ST_TYPE(s.st_info) != STT_FUNC) continue;
            if (s.st_shndx == SHN_UNDEF) continue; // declaration, not definition
            std::string name = strtab + s.st_name;
            if (name.empty()) continue;
            if (exact_filter && name != exact_filter) continue;
            if (regex_filter) {
                std::regex re(regex_filter);
                // match against mangled OR demangled — useful for human-readable searches
                if (!std::regex_search(name, re) &&
                    !std::regex_search(demangle(name), re)) continue;
            }
            funcs.push_back({ std::move(name), (int)s.st_shndx, s.st_value });
        }
    }

    if (funcs.empty()) {
        munmap((void*)base, st.st_size);
        return 0;
    }

    // Group by section so we can batch addr2line queries per section
    // section_idx -> list of (offset, index_into_funcs)
    std::unordered_map<int, std::vector<std::pair<uint64_t,size_t>>> by_section;
    for (size_t i = 0; i < funcs.size(); i++)
        by_section[funcs[i].shndx].emplace_back(funcs[i].value, i);

    // For each section: look up source locations in batch
    std::vector<Location> locations(funcs.size());

    if (!no_dwarf) {
        for (auto& [shidx, entries] : by_section) {
            std::string sname(obj.section_name(shidx));

            // Deduplicate offsets (multiple funcs at same offset is rare but possible)
            std::vector<uint64_t> offsets;
            offsets.reserve(entries.size());
            for (auto& [off, _] : entries) offsets.push_back(off);
            std::sort(offsets.begin(), offsets.end());
            offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

            std::string jflag = (sname == ".text") ? "" : sname;
            std::unordered_map<uint64_t, Location> section_locs;
            batch_addr2line(obj_path, jflag, offsets, section_locs);

            for (auto& [off, idx] : entries) {
                auto it = section_locs.find(off);
                if (it != section_locs.end())
                    locations[idx] = it->second;
            }
        }
    }

    // Emit TSV
    for (size_t i = 0; i < funcs.size(); i++) {
        const auto& f   = funcs[i];
        const auto& loc = locations[i];
        printf("%s\t%s\t%d\t%s\n",
               f.name.c_str(),
               loc.file.empty() ? "-" : loc.file.c_str(),
               loc.line,
               demangle(f.name).c_str());
    }

    munmap((void*)base, st.st_size);
    return 0;
}
