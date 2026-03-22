// calltrace.cpp
// Extracts (caller, source_path, source_line, callee) tuples from an ELF .o file.
//
// Build:  see CMakeLists.txt
//
// Usage:  calltrace [options] <file.o>
//
// Options:
//   --no-intra           suppress intra-TU calls (needs -ffunction-sections)
//   --no-inter           suppress inter-TU calls (external symbols)
//   --no-stl             suppress rows where caller source is a system header
//   --no-dwarf           skip addr2line; emit '-' for file/line (faster)
//   --callee <str>       only emit rows where mangled callee == <str> (exact)
//   --callee-filter <re> only emit rows where callee matches regex (mangled or demangled)
//   --caller <str>       only emit rows where mangled caller == <str> (exact)
//   --caller-filter <re> only emit rows where caller matches regex (mangled or demangled)
//
// Output (TSV):
//   mangled_caller \t demangled_caller \t source_path \t source_line \t demangled_callee \t mangled_callee
//
// Notes:
//   - Only statically-resolvable direct calls (via relocations) are captured.
//     Virtual dispatch and calls through function pointers are NOT visible.
//   - Intra-TU calls are only visible when compiled with -ffunction-sections.

#include "common.h"

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options {
    bool        no_intra      = false;
    bool        no_inter      = false;
    bool        no_stl        = false;
    bool        no_dwarf      = false;
    const char* obj_path      = nullptr;
    const char* callee_exact  = nullptr;
    const char* callee_filter = nullptr;
    const char* caller_exact  = nullptr;
    const char* caller_filter = nullptr;
};

static void usage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s [options] <file.o>\n"
        "\n"
        "Output (TSV):  mangled_caller \\t demangled_caller \\t source_path \\t source_line"
            " \\t demangled_callee \\t mangled_callee\n"
        "\n"
        "Options:\n"
        "  --no-intra           suppress intra-TU calls (needs -ffunction-sections)\n"
        "  --no-inter           suppress inter-TU calls (external symbols)\n"
        "  --no-stl             suppress rows where caller source is a system header\n"
        "  --no-dwarf           skip addr2line; emit '-' for file/line (faster)\n"
        "  --callee <str>       only rows where mangled callee == <str> (exact)\n"
        "  --callee-filter <re> only rows where callee matches regex (mangled or demangled)\n"
        "  --caller <str>       only rows where mangled caller == <str> (exact)\n"
        "  --caller-filter <re> only rows where caller matches regex (mangled or demangled)\n",
        argv0);
}

static Options parse_args(int argc, char** argv) {
    Options o;
    int i = 1;
    for (; i < argc; i++) {
        std::string_view a = argv[i];
        if      (a == "--no-intra") o.no_intra = true;
        else if (a == "--no-inter") o.no_inter = true;
        else if (a == "--no-stl")   o.no_stl   = true;
        else if (a == "--no-dwarf") o.no_dwarf = true;
        else if (a == "--callee") {
            if (++i >= argc) { fprintf(stderr, "--callee requires an argument\n"); exit(1); }
            o.callee_exact = argv[i];
        }
        else if (a == "--callee-filter") {
            if (++i >= argc) { fprintf(stderr, "--callee-filter requires an argument\n"); exit(1); }
            o.callee_filter = argv[i];
        }
        else if (a == "--caller") {
            if (++i >= argc) { fprintf(stderr, "--caller requires an argument\n"); exit(1); }
            o.caller_exact = argv[i];
        }
        else if (a == "--caller-filter") {
            if (++i >= argc) { fprintf(stderr, "--caller-filter requires an argument\n"); exit(1); }
            o.caller_filter = argv[i];
        }
        else if (a.substr(0, 2) == "--") {
            fprintf(stderr, "unknown option: %s\n", argv[i]); exit(1);
        }
        else break; // first non-option argument
    }

    // Must have exactly one non-option argument (the .o path) and it must be last
    if (i >= argc) {
        fprintf(stderr, "error: missing <file.o>\n");
        usage(argv[0]);
        exit(1);
    }
    o.obj_path = argv[i];

    // Reject anything after the file — options must come before the file
    for (int j = i + 1; j < argc; j++) {
        std::string_view extra = argv[j];
        if (extra.substr(0, 2) == "--")
            fprintf(stderr, "error: option '%s' must appear before <file.o>\n", argv[j]);
        else
            fprintf(stderr, "error: unexpected argument '%s' after <file.o>\n", argv[j]);
        usage(argv[0]);
        exit(1);
    }
    return o;
}

// ---------------------------------------------------------------------------
// A call edge collected during the relocation scan
// ---------------------------------------------------------------------------
struct Edge {
    std::string caller_mangled;
    std::string callee_mangled;
    int         section_idx;
    uint64_t    call_offset;
};

// ---------------------------------------------------------------------------
// Core processing
// ---------------------------------------------------------------------------
static void process(const Options& opt) {
    ObjFile  obj;
    uint8_t* base = nullptr;
    if (!open_obj(opt.obj_path, obj, base)) return;

    const Elf64_Ehdr* eh = obj.ehdr();

    SymbolTable symtab;
    symtab.load(obj);

    // Pass 1: scan RELA sections, collect call edges and DWARF queries
    std::vector<Edge> edges;
    std::unordered_map<int, std::vector<uint64_t>> dwarf_queries;

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
        const auto* relas       = reinterpret_cast<const Elf64_Rela*>(obj.section_data(i));
        size_t nrela = rsh->sh_size / sizeof(Elf64_Rela);

        for (size_t j = 0; j < nrela; j++) {
            const Elf64_Rela& r = relas[j];
            if (!is_call_reloc(ELF64_R_TYPE(r.r_info))) continue;

            uint32_t         sym_idx = ELF64_R_SYM(r.r_info);
            const Elf64_Sym& esym    = sym_base[sym_idx];
            uint8_t          stype   = ELF64_ST_TYPE(esym.st_info);

            std::string callee_mangled;

            if (stype == STT_SECTION) {
                if (opt.no_intra) continue;
                int target_fn_shndx = (int)esym.st_shndx;
                if (obj.section_name(target_fn_shndx).find(".text") == std::string_view::npos)
                    continue;
                uint64_t target_offset = (uint64_t)(r.r_addend + 4);
                const SymbolTable::Sym* callee_sym =
                    symtab.containing_function(target_fn_shndx, target_offset);
                if (!callee_sym) continue;
                callee_mangled = callee_sym->name;

            } else if (stype == STT_OBJECT || esym.st_name == 0) {
                continue;

            } else {
                bool is_external = (esym.st_shndx == SHN_UNDEF);
                if ( is_external && opt.no_inter) continue;
                if (!is_external && opt.no_intra) continue;
                callee_mangled = strtab + esym.st_name;
            }

            if (callee_mangled.empty()) continue;
            if (!matches_exact(opt.callee_exact, callee_mangled)) continue;
            if (!matches_filter(opt.callee_filter, callee_mangled)) continue;

            uint64_t call_offset = r.r_offset;
            const SymbolTable::Sym* caller_sym =
                symtab.containing_function(target_shidx, call_offset);
            if (!caller_sym) continue;

            if (!matches_exact(opt.caller_exact, caller_sym->name)) continue;
            if (!matches_filter(opt.caller_filter, caller_sym->name)) continue;

            edges.push_back({ caller_sym->name, callee_mangled,
                               target_shidx, call_offset });
            if (!opt.no_dwarf)
                dwarf_queries[target_shidx].push_back(call_offset);
        }
    }

    // Pass 1b: x86-64 intra-section scan for sections with multiple functions.
    //
    // When a section contains more than one STT_FUNC symbol it was NOT compiled
    // with -ffunction-sections, so intra-section calls have no relocation entries.
    // We find them by scanning for the x86-64 direct-call opcode E8 followed by
    // a 4-byte signed PC-relative displacement, and checking that the target
    // lands exactly on a known function entry point in the same section.
    //
    // Complexity:
    //   - section_range(): O(log N) in total symbol count
    //   - e8 scan:         O(section_size) via memchr
    //   - caller lookup:   amortized O(1) — pointer advances monotonically
    //   - callee lookup:   O(log k) per hit via lower_bound within [begin,end)
    if (!opt.no_intra) {
        for (int shidx = 0; shidx < eh->e_shnum; shidx++) {
            std::string_view sname = obj.section_name(shidx);
            if (sname.find(".text") == std::string_view::npos) continue;
            if (opt.no_stl && is_stl_section(sname)) continue;

            auto [begin, end] = symtab.section_range(shidx);
            // Only useful when multiple functions share this section
            if (end - begin <= 1) continue;

            const Elf64_Shdr* sh  = obj.shdr(shidx);
            const uint8_t*    data = obj.section_data(shidx);
            uint64_t          sec_size = sh->sh_size;

            // Caller pointer: starts at last entry (lowest value = first function),
            // advances backward (toward begin = highest value) as off increases.
            // syms is descending, so begin->value is largest, (end-1)->value is smallest.
            auto caller_ptr = end - 1; // points to function with lowest value

            const uint8_t* p = data;
            const uint8_t* sec_end = data + sec_size;

            while (p < sec_end) {
                // Fast scan for E8 opcode
                p = static_cast<const uint8_t*>(memchr(p, 0xe8, sec_end - p));
                if (!p) break;

                uint64_t off = (uint64_t)(p - data);

                // Need 4 bytes after the opcode
                if (off + 5 > sec_size) { p++; continue; }

                // Decode the signed 32-bit displacement (little-endian)
                int32_t disp;
                memcpy(&disp, p + 1, 4);

                // Target = address of next instruction + displacement
                // Within an unlinked .o, "address" is section-relative offset
                uint64_t target = (uint64_t)((int64_t)(off + 5) + disp);

                // Sanity: target must be within this section
                if (target >= sec_size) { p++; continue; }

                // Callee: target must land exactly on a function entry point.
                // lower_bound in descending [begin,end) for exact value match.
                SymbolTable::Sym key;
                key.shndx = shidx;
                key.value = target;
                auto callee_it = std::lower_bound(begin, end, key);
                if (callee_it == end || callee_it->value != target) { p++; continue; }
                const SymbolTable::Sym* callee_sym = &*callee_it;

                // Caller: advance pointer while the next function starts <= off
                // (remember: decreasing index = increasing value in the section)
                while (caller_ptr != begin && (caller_ptr - 1)->value <= off)
                    --caller_ptr;
                const SymbolTable::Sym* caller_sym = &*caller_ptr;

                // Verify off is actually within caller (if size is known)
                if (caller_sym->size > 0 && off >= caller_sym->value + caller_sym->size)
                    { p++; continue; }

                // Skip self-calls (shouldn't happen but be safe)
                if (callee_sym == caller_sym) { p++; continue; }

                const std::string& callee_mangled = callee_sym->name;
                const std::string& caller_mangled = caller_sym->name;

                if (!matches_exact(opt.callee_exact,  callee_mangled)) { p++; continue; }
                if (!matches_filter(opt.callee_filter, callee_mangled)) { p++; continue; }
                if (!matches_exact(opt.caller_exact,  caller_mangled)) { p++; continue; }
                if (!matches_filter(opt.caller_filter, caller_mangled)) { p++; continue; }

                edges.push_back({ caller_mangled, callee_mangled, shidx, off });
                if (!opt.no_dwarf)
                    dwarf_queries[shidx].push_back(off);

                p++; // advance past this E8 and keep scanning
            }
        }
    }

    // Pass 2: batch DWARF lookups
    LocationMap locations;
    for (auto& [shidx, offsets] : dwarf_queries) {
        std::string sname(obj.section_name(shidx));
        if (opt.no_stl && is_stl_section(sname)) continue;
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
        std::string jflag = (sname == ".text") ? "" : sname;
        batch_addr2line(opt.obj_path, jflag, shidx, offsets, locations);
    }

    // Emit TSV
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

        printf("%s\t%s\t%s\t%d\t%s\t%s\n",
               e.caller_mangled.c_str(),
               dcache.get(e.caller_mangled).c_str(),
               src_file.c_str(),
               src_line,
               dcache.get(e.callee_mangled).c_str(),
               e.callee_mangled.c_str());
    }

    munmap(base, obj.size);
}

int main(int argc, char** argv) {
    process(parse_args(argc, argv));
    return 0;
}
