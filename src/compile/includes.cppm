module clice:compile.includes;

import :compile.compilation;
import :support.prelude;

namespace clice::includes {

/// What the include analysis reads, recorded while the compile parses.
struct IncludeRecorder {
    /// The IWYU pragmas of the files the compile enters, and those a loaded
    /// preamble recorded.
    clang::include_cleaner::PragmaIncludes pragmas;

    /// The focus file's includes and the macros it uses.
    clang::include_cleaner::RecordedPP recorded;

    /// The names the restored pragmas point at.
    llvm::BumpPtrAllocator names;
};

}  // namespace clice::includes
