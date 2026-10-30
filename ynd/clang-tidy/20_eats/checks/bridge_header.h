#pragma once

#if !defined(IX_CLANG_TIDY_BUILD)

// Used only for the plugin build against a system LLVM 20
#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyModule.h>
#include <clang-tidy/ClangTidyModuleRegistry.h>
#include <clang-tidy/utils/FileExtensionsUtils.h>

#else

// According to llvm repo
#include "../ClangTidyCheck.h"
#include "../ClangTidy.h"
#include "../ClangTidyModule.h"
#include "../ClangTidyModuleRegistry.h"
#include "../utils/FileExtensionsUtils.h"

#endif
