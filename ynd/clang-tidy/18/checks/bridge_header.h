#pragma once

#if !defined(IX_CLANG_TIDY_BUILD)

// Used only for inner builds
#include <contrib/libs/clang16/tools/extra/clang-tidy/ClangTidyCheck.h>
#include <contrib/libs/clang16/tools/extra/clang-tidy/ClangTidy.h>
#include <contrib/libs/clang16/tools/extra/clang-tidy/ClangTidyModule.h>
#include <contrib/libs/clang16/tools/extra/clang-tidy/ClangTidyModuleRegistry.h>

#else

// According to llvm repo
#include "../ClangTidyCheck.h"
#include "../ClangTidy.h"
#include "../ClangTidyModule.h"
#include "../ClangTidyModuleRegistry.h"

#endif
