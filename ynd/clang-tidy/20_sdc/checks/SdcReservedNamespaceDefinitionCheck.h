#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// identifiers shall not be
// defined in namespace `std`, `posix`, or `stdN`, where 'N' is any number".
// Template specializations of standard-library templates are explicitly exempt
class SdcReservedNamespaceDefinitionCheck : public ClangTidyCheck {
public:
    SdcReservedNamespaceDefinitionCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
