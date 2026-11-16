module clice;

import :compile.compilation;
import :compile.diagnostic;
import :compile.includes;
import :support.text;
import :syntax.lexical_scan;
import :syntax.token;

namespace clice {

namespace {

/// The fields of a PragmaIncludes that hold what its recording learned.
struct PragmaFields {
    llvm::DenseMap<llvm::sys::fs::UniqueID, llvm::StringRef>& public_headers;

    llvm::DenseMap<llvm::sys::fs::UniqueID, llvm::SmallVector<llvm::StringRef>>& exporters;

    llvm::DenseMap<clang::tooling::stdlib::Header, llvm::SmallVector<llvm::StringRef>>&
        standard_exporters;

    llvm::DenseSet<llvm::sys::fs::UniqueID>& not_self_contained;

    llvm::DenseSet<llvm::sys::fs::UniqueID>& kept;
};

/// The private fields of `pragmas`. A Content compile loading a preamble's
/// PCH parses none of the preamble, and PragmaIncludes fills them only while
/// recording a parse. PragmaFieldsOf defines this function: an explicit
/// instantiation may name private members.
PragmaFields pragma_fields(clang::include_cleaner::PragmaIncludes& pragmas);

template <auto Public, auto Exporters, auto StandardExporters, auto NotSelfContained, auto Kept>
struct PragmaFieldsOf {
    friend PragmaFields pragma_fields(clang::include_cleaner::PragmaIncludes& pragmas) {
        return {
            pragmas.*Public,
            pragmas.*Exporters,
            pragmas.*StandardExporters,
            pragmas.*NotSelfContained,
            pragmas.*Kept,
        };
    }
};

template struct PragmaFieldsOf<&clang::include_cleaner::PragmaIncludes::IWYUPublic,
                               &clang::include_cleaner::PragmaIncludes::IWYUExportBy,
                               &clang::include_cleaner::PragmaIncludes::StdIWYUExportBy,
                               &clang::include_cleaner::PragmaIncludes::NonSelfContainedFiles,
                               &clang::include_cleaner::PragmaIncludes::ShouldKeep>;

/// A symbol the focus file uses from a header it does not include.
struct MissingInclude {
    /// Where the focus file names it.
    LocalSourceRange range;

    std::string name;

    /// The header to include, with its delimiters.
    std::string spelling;

    friend bool operator==(const MissingInclude&, const MissingInclude&) = default;
};

}  // namespace

void CompilationUnit::record_includes() {
    auto kind = params.kind;
    bool content = kind == CompilationKind::Content && params.includes &&
                   (params.includes->unused || params.includes->missing);
    bool restores = content && params.input.pch.second != 0;
    // LIMITATION: a header compiled in its host's context, Objective-C (as
    // clangd) and a compile loading a preamble whose index recorded no
    // includes get no include diagnostics.
    if((kind != CompilationKind::Preamble && !content) || borrows_context() ||
       params.instantiating || lang_options().ObjC || (restores && !params.includes->preamble)) {
        return;
    }

    auto& pp = instance->getPreprocessor();
    include_recorder = std::make_unique<includes::IncludeRecorder>();
    auto& recorder = *include_recorder;
    pp.addPPCallbacks(recorder.recorded.record(pp));
    if(restores) {
        auto& preamble = *params.includes->preamble;
        auto focus = focus_file();
        auto size = focus.content().size();
        auto& files = instance->getFileManager();
        for(const auto& include: preamble.includes) {
            if(include.offset >= size) {
                continue;
            }
            recorder.recorded.Includes.add({
                .Spelled = include.spelled,
                .Resolved = include.resolved.empty() ? clang::OptionalFileEntryRef()
                                                     : files.getOptionalFileRef(include.resolved),
                .HashLocation = focus.location(include.offset),
                .Line = include.line,
                .Angled = include.angled,
            });
        }

        // The preamble's build names the files: their UniqueIDs may differ
        // in this compile's file system.
        auto file_id = [&](llvm::StringRef name) -> std::optional<llvm::sys::fs::UniqueID> {
            if(auto file = files.getOptionalFileRef(name)) {
                return file->getUniqueID();
            }
            return std::nullopt;
        };
        llvm::StringSaver names(recorder.names);
        auto fields = pragma_fields(recorder.pragmas);
        for(const auto& header: preamble.private_headers) {
            if(auto id = file_id(header.file)) {
                fields.public_headers.try_emplace(*id, names.save(header.replacement));
            }
        }
        for(const auto& header: preamble.exported) {
            if(auto id = file_id(header.header)) {
                auto& exporters = fields.exporters[*id];
                for(const auto& exporter: header.exporters) {
                    exporters.push_back(names.save(exporter));
                }
            }
        }
        auto language = lang_options().CPlusPlus ? clang::tooling::stdlib::Lang::CXX
                                                 : clang::tooling::stdlib::Lang::C;
        for(const auto& header: preamble.exported_standard) {
            if(auto standard = clang::tooling::stdlib::Header::named(header.header, language)) {
                auto& exporters = fields.standard_exporters[*standard];
                for(const auto& exporter: header.exporters) {
                    exporters.push_back(names.save(exporter));
                }
            }
        }
        for(const auto& file: preamble.kept) {
            if(auto id = file_id(file)) {
                fields.kept.insert(*id);
            }
        }
        for(const auto& file: preamble.not_self_contained) {
            if(auto id = file_id(file)) {
                fields.not_self_contained.insert(*id);
            }
        }
    }
    recorder.pragmas.record(pp);
}

void CompilationUnit::analyze_includes() {
    if(!include_recorder || params.kind != CompilationKind::Content || fatal_error()) {
        return;
    }
    namespace include_cleaner = clang::include_cleaner;
    auto& recorder = *include_recorder;
    auto& sm = SM();
    auto& pp = instance->getPreprocessor();
    auto& search = pp.getHeaderSearchInfo();
    auto focus = focus_file();
    auto content = focus.content();

    // PragmaIncludes sorts the exporters at the end of the main file, which
    // the analysis runs before.
    for(auto& entry: pragma_fields(recorder.pragmas).exporters) {
        auto& exporters = entry.second;
        ranges::sort(exporters);
        auto duplicates = ranges::unique(exporters);
        exporters.erase(duplicates.begin(), duplicates.end());
    }

    std::vector<include_cleaner::SymbolReference> macros;
    for(const auto& reference: recorder.recorded.MacroReferences) {
        if(reference.RefLocation.isFileID()) {
            macros.push_back(reference);
        }
    }
    auto preamble = sm.getPreambleFileID();
    if(const auto& restored = params.includes->preamble; restored && preamble.isValid()) {
        for(const auto& use: restored->macros) {
            if(use.offset >= content.size()) {
                continue;
            }
            // At the name of an `#undef`, the macro is undefined already.
            auto at = sm.getComposedLoc(preamble, use.offset);
            if(use.offset > 0) {
                at = at.getLocWithOffset(-1);
            }
            auto* name = pp.getIdentifierInfo(use.name);
            auto* macro = pp.getMacroDefinitionAtLoc(name, at).getMacroInfo();
            if(!macro) {
                continue;
            }
            macros.push_back({
                .Target = include_cleaner::Macro{name, macro->getDefinitionLoc()},
                .RefLocation = focus.location(use.offset),
                .RT = use.tested ? include_cleaner::RefType::Ambiguous
                                 : include_cleaner::RefType::Explicit,
            });
        }
    }

    auto* main = sm.getFileEntryForID(focus.file());
    auto* preamble_entry = preamble.isValid() ? sm.getFileEntryForID(preamble) : nullptr;
    auto builtin = search.getModuleMap().getBuiltinDir();
    const auto& written = recorder.recorded.Includes;
    auto from_modules = [&](const include_cleaner::Symbol& symbol) {
        auto imported_at = [&](clang::SourceLocation location) {
            return location.isValid() && imported(sm.getFileID(sm.getExpansionLoc(location)));
        };
        if(symbol.kind() == include_cleaner::Symbol::Macro) {
            return imported_at(symbol.macro().Definition);
        }
        for(const auto* declaration: symbol.declaration().redecls()) {
            if(!imported_at(declaration->getLocation())) {
                return false;
            }
        }
        return true;
    };

    llvm::DenseSet<const clang::FileEntry*> used;
    std::vector<MissingInclude> missing;
    include_cleaner::walkUsed(
        top_level_decls(),
        macros,
        &recorder.pragmas,
        pp,
        [&](const include_cleaner::SymbolReference& reference,
            llvm::ArrayRef<include_cleaner::Header> providers) {
            bool satisfied = false;
            auto match = [&](const include_cleaner::Header& header) {
                for(const auto* include: written.match(header)) {
                    if(include->Resolved) {
                        satisfied = true;
                        used.insert(&include->Resolved->getFileEntry());
                    }
                }
            };
            for(const auto& provider: providers) {
                if(provider.kind() == include_cleaner::Header::Physical) {
                    auto file = provider.physical();
                    if(&file.getFileEntry() == main || &file.getFileEntry() == preamble_entry ||
                       file.getDir() == builtin) {
                        satisfied = true;
                        continue;
                    }
                }
                match(provider);
            }
            if(satisfied || providers.empty() ||
               reference.RT != include_cleaner::RefType::Explicit) {
                return;
            }

            // An include may name a provider only by its spelling, as
            // `#include_next "x.h"` does.
            auto spelling = include_cleaner::spellHeader({providers.front(), search, main});
            match(include_cleaner::Header(llvm::StringRef(spelling)));
            if(satisfied || !params.includes->missing || from_modules(reference.Target)) {
                return;
            }

            auto location = sm.getFileLoc(reference.RefLocation);
            while(location.isValid() && !focus.contains(sm.getFileID(location))) {
                location = sm.getIncludeLoc(sm.getFileID(location));
            }
            if(location.isInvalid()) {
                return;
            }
            auto offset = sm.getFileOffset(location);
            missing.push_back({
                .range = {offset, offset + token_length(focus.location(offset))},
                .name = reference.Target.name(),
                .spelling = std::move(spelling),
            });
        });

    ranges::sort(missing, {}, [](const MissingInclude& include) {
        return std::tie(include.range.begin, include.range.end, include.name, include.spelling);
    });
    auto duplicates = ranges::unique(missing);
    missing.erase(duplicates.begin(), duplicates.end());

    // As clangd, the headers an `#include` names anywhere, inactive or not,
    // are never reported missing.
    llvm::StringSet<> included;
    const auto& lexical = focus.lexical();
    for(const auto& directive: lexical.directives) {
        if(directive.kind != LexicalInfo::Directive::Kind::Include) {
            continue;
        }
        auto words = lexical.words(directive);
        if(words.size() < 2 || words[0].text(content) == "import" || !words[1].is_header_name()) {
            continue;
        }
        auto name = words[1].text(content);
        included.insert(name.substr(name.find_first_of("<\"")));
    }
    for(auto& include: missing) {
        if(included.contains(include.spelling)) {
            continue;
        }
        collected_diagnostics.push_back({
            .id = {.value = 0,
                   .level = DiagnosticLevel::Note,
                   .source = DiagnosticSource::Clice,
                   .name = "missing-includes"},
            .fid = focus.file(),
            .range = include.range,
            .message =
                std::format(R"(No header providing "{}" is directly included)", include.name),
            .fix = {{.range = {0, 0}, .text = std::format("#include {}\n", include.spelling)}},
        });
    }

    if(!params.includes->unused) {
        return;
    }
    auto language = lang_options().CPlusPlus ? clang::tooling::stdlib::Lang::CXX
                                             : clang::tooling::stdlib::Lang::C;
    auto path = file_path(focus.file());
    for(const auto& include: written.all()) {
        if(!include.Resolved || used.contains(&include.Resolved->getFileEntry())) {
            continue;
        }
        auto file = *include.Resolved;
        if(file.getDir() == builtin || recorder.pragmas.shouldKeep(&file.getFileEntry())) {
            continue;
        }
        // As clangd's default, a header written in angle brackets is
        // reported only when the standard library names it.
        if(include.Angled) {
            auto name = std::format("<{}>", include.Spelled);
            if(!clang::tooling::stdlib::Header::named(name, language)) {
                continue;
            }
        } else {
            auto replacement = recorder.pragmas.getPublic(&file.getFileEntry());
            if((!replacement.empty() && path.ends_with(replacement.trim("<>\""))) ||
               !search.isFileMultipleIncludeGuarded(file)) {
                continue;
            }
        }

        auto hash = sm.getFileOffset(include.HashLocation);
        auto end = std::min(content.find_first_of("\r\n", hash), content.size());
        collected_diagnostics.push_back({
            .id = {.value = 0,
                   .level = DiagnosticLevel::Warning,
                   .source = DiagnosticSource::Clice,
                   .name = "unused-includes"},
            .fid = focus.file(),
            .range = {hash, static_cast<std::uint32_t>(end)},
            .message = std::format(
                "included header {} is not used directly",
                llvm::sys::path::filename(include.Spelled, llvm::sys::path::Style::posix)),
            .fix = {{.range = {line_begin(content, hash), line_end(content, hash)}, .text = ""}},
        });
    }
}

auto CompilationUnit::preamble_includes() -> std::optional<includes::PreambleIncludes> {
    if(!include_recorder) {
        return std::nullopt;
    }
    auto& recorder = *include_recorder;
    auto& sm = SM();
    llvm::DenseMap<llvm::sys::fs::UniqueID, llvm::StringRef> names;
    for(auto it = sm.fileinfo_begin(); it != sm.fileinfo_end(); ++it) {
        names.try_emplace(it->first.getUniqueID(), it->first.getName());
    }
    auto name_of = [&](llvm::sys::fs::UniqueID id) -> llvm::StringRef {
        auto it = names.find(id);
        return it == names.end() ? llvm::StringRef() : it->second;
    };

    includes::PreambleIncludes result;
    for(const auto& include: recorder.recorded.Includes.all()) {
        result.includes.push_back({
            .spelled = include.Spelled.str(),
            .resolved = include.Resolved ? include.Resolved->getName().str() : std::string(),
            .offset = sm.getFileOffset(include.HashLocation),
            .line = include.Line,
            .angled = include.Angled,
        });
    }
    for(const auto& reference: recorder.recorded.MacroReferences) {
        auto location = reference.RefLocation;
        if(!location.isFileID() || sm.getFileID(location) != sm.getMainFileID()) {
            continue;
        }
        result.macros.push_back({
            .name = reference.Target.macro().Name->getName().str(),
            .offset = sm.getFileOffset(location),
            .tested = reference.RT == clang::include_cleaner::RefType::Ambiguous,
        });
    }

    auto fields = pragma_fields(recorder.pragmas);
    for(const auto& [id, replacement]: fields.public_headers) {
        if(auto file = name_of(id); !file.empty()) {
            result.private_headers.push_back(
                {.file = file.str(), .replacement = replacement.str()});
        }
    }
    auto exports_of = [](llvm::StringRef header, llvm::ArrayRef<llvm::StringRef> exporters) {
        includes::PreambleIncludes::ExportedHeader entry{.header = header.str()};
        for(auto exporter: exporters) {
            entry.exporters.push_back(exporter.str());
        }
        ranges::sort(entry.exporters);
        auto duplicates = ranges::unique(entry.exporters);
        entry.exporters.erase(duplicates.begin(), duplicates.end());
        return entry;
    };
    for(const auto& [id, exporters]: fields.exporters) {
        if(auto file = name_of(id); !file.empty()) {
            result.exported.push_back(exports_of(file, exporters));
        }
    }
    for(const auto& [header, exporters]: fields.standard_exporters) {
        result.exported_standard.push_back(exports_of(header.name(), exporters));
    }
    for(auto id: fields.kept) {
        if(auto file = name_of(id); !file.empty()) {
            result.kept.push_back(file.str());
        }
    }
    for(auto id: fields.not_self_contained) {
        if(auto file = name_of(id); !file.empty()) {
            result.not_self_contained.push_back(file.str());
        }
    }

    ranges::sort(result.private_headers, {}, &includes::PreambleIncludes::PrivateHeader::file);
    ranges::sort(result.exported, {}, &includes::PreambleIncludes::ExportedHeader::header);
    ranges::sort(result.exported_standard, {}, &includes::PreambleIncludes::ExportedHeader::header);
    ranges::sort(result.kept);
    ranges::sort(result.not_self_contained);
    return result;
}

}  // namespace clice
