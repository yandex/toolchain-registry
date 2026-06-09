#pragma once

#include "bridge_header.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

namespace clang {
namespace tidy {
namespace sdc {

class SdcAssertConstantCheck : public ClangTidyCheck {
    // Populated by PP callback; consumed by check().
    // Maps arg-begin spelling location → {assert tok location, arg-end spelling location}.
    llvm::DenseMap<SourceLocation,
                   std::pair<SourceLocation, SourceLocation>> AssertArgs;
    // Tracks which arg-begin locations have already produced a diagnostic.
    llvm::DenseSet<SourceLocation> Diagnosed;

public:
    SdcAssertConstantCheck(StringRef Name, ClangTidyContext* Context);

    void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                              Preprocessor* ModuleExpanderPP) override;
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

    // Called from the PP callback.
    void recordAssert(SourceLocation AssertLoc,
                      SourceLocation ArgBegin,
                      SourceLocation ArgEnd);

    // Called from the PP callback when a constant is detected at the token
    // level — warns immediately and suppresses the later AST pass for the
    // same assert call.
    void warnConstantToken(SourceLocation AssertLoc, SourceLocation ArgBegin);
};

} // namespace sdc
} // namespace tidy
} // namespace clang
