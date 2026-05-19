#pragma once

#include "bridge_header.h"

namespace clang {

class Preprocessor;

namespace tidy {
namespace sdc {

class SdcUnreachableCodeCheck : public ClangTidyCheck {
public:
    SdcUnreachableCodeCheck(StringRef Name, ClangTidyContext* Context);

    void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                             Preprocessor* ModuleExpanderPP) override;
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    Preprocessor* PP_ = nullptr;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
