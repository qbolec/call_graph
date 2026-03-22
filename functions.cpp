// functions.cpp
// Lists all functions defined in an ELF .o file as TSV:
//   mangled_name \t source_path \t source_line \t demangled_name
//
// Build:  see CMakeLists.txt
//
// Usage:  functions [options] <file.o>
//
// Options:
//   --no-dwarf           skip addr2line; emit '-' for file/line (faster)
//   --no-stl             suppress functions whose source is a system header (/usr, /opt)
//   --function <str>     only show functions where mangled name == <str> (exact)
//   --function-filter <re> only show functions where name matches regex (mangled or demangled)
//
// Typical use: find the mangled name of a function you know by source location
//   functions foo.o | grep "foo.cpp:42"
//   functions foo.o --function-filter "my_function_name"

#include "common.h"

static void usage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s [options] <file.o>\n"
        "\n"
        "Output (TSV):  mangled_name \\t source_path \\t source_line \\t demangled_name\n"
        "\n"
        "Options:\n"
        "  --no-dwarf             skip addr2line; emit '-' for file/line (faster)\n"
        "  --no-stl               suppress functions from system headers (/usr, /opt)\n"
        "  --function <str>       only show functions where mangled name == <str> (exact)\n"
        "  --function-filter <re> only show functions matching regex (mangled or demangled)\n",
        argv0);
}

int main(int argc, char** argv) {
    bool        no_dwarf      = false;
    bool        no_stl        = false;
    const char* obj_path      = nullptr;
    const char* exact_filter  = nullptr;
    const char* regex_filter  = nullptr;

    for (int i = 1; i < argc; i++) {
        std::string_view a = argv[i];
        if      (a == "--no-dwarf") no_dwarf = true;
        else if (a == "--no-stl")   no_stl   = true;
        else if (a == "--function") {
            if (++i >= argc) { fprintf(stderr, "--function requires an argument\n"); return 1; }
            exact_filter = argv[i];
        }
        else if (a == "--function-filter") {
            if (++i >= argc) { fprintf(stderr, "--function-filter requires an argument\n"); return 1; }
            regex_filter = argv[i];
        }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (a.substr(0, 2) == "--") {
            fprintf(stderr, "unknown option: %s\n", argv[i]); return 1;
        }
        else if (!obj_path) obj_path = argv[i];
        else {
            fprintf(stderr, "error: unexpected argument '%s' after <file.o>\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!obj_path) {
        fprintf(stderr, "error: missing <file.o>\n");
        usage(argv[0]);
        return 1;
    }

    ObjFile  obj;
    uint8_t* base = nullptr;
    if (!open_obj(obj_path, obj, base)) return 1;

    const Elf64_Ehdr* eh = obj.ehdr();

    // Collect all defined STT_FUNC symbols, applying filters early
    struct FuncSym {
        std::string name;
        int         shndx;
        uint64_t    value;
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
            if (s.st_shndx == SHN_UNDEF) continue;
            // Skip STL sections early when --no-stl, same as calltrace does
            if (no_stl && is_stl_section(obj.section_name(s.st_shndx))) continue;
            std::string name = strtab + s.st_name;
            if (name.empty()) continue;
            if (!matches_exact(exact_filter, name)) continue;
            if (!matches_filter(regex_filter, name)) continue;
            funcs.push_back({ std::move(name), (int)s.st_shndx, s.st_value });
        }
    }

    if (funcs.empty()) { munmap(base, obj.size); return 0; }

    // Group by section for batch addr2line
    std::unordered_map<int, std::vector<std::pair<uint64_t,size_t>>> by_section;
    for (size_t i = 0; i < funcs.size(); i++)
        by_section[funcs[i].shndx].emplace_back(funcs[i].value, i);

    std::vector<Location> locations(funcs.size());

    if (!no_dwarf) {
        for (auto& [shidx, entries] : by_section) {
            std::string sname(obj.section_name(shidx));
            std::vector<uint64_t> offsets;
            offsets.reserve(entries.size());
            for (auto& [off, _] : entries) offsets.push_back(off);
            std::sort(offsets.begin(), offsets.end());
            offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

            std::string jflag = (sname == ".text") ? "" : sname;
            LocationMap section_locs;
            batch_addr2line(obj_path, jflag, shidx, offsets, section_locs);

            for (auto& [off, idx] : entries) {
                auto it = section_locs.find({shidx, off});
                if (it != section_locs.end())
                    locations[idx] = it->second;
            }
        }
    }

    // Emit TSV — apply --no-stl path filter after DWARF lookup
    DemangleCache dcache;
    for (size_t i = 0; i < funcs.size(); i++) {
        const auto& f   = funcs[i];
        const auto& loc = locations[i];
        if (no_stl && is_system_path(loc.file)) continue;
        printf("%s\t%s\t%d\t%s\n",
               f.name.c_str(),
               loc.file.empty() ? "-" : loc.file.c_str(),
               loc.line,
               dcache.get(f.name).c_str());
    }

    munmap(base, obj.size);
    return 0;
}
