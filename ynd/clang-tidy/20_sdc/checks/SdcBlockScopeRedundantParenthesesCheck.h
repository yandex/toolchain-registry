#pragma once

#include "bridge_header.h"

#include <set>
#include <string>

namespace clang {
    namespace tidy {
        namespace sdc {

            class SdcBlockScopeRedundantParenthesesCheck: public ClangTidyCheck {
            public:
                SdcBlockScopeRedundantParenthesesCheck(StringRef Name, ClangTidyContext* Context);
                void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                                         Preprocessor* ModuleExpanderPP) override;
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
                void recordMacroWithRedundantParens(StringRef Name);
                bool macroHasRedundantParens(StringRef Name) const;

            private:
                std::set<std::string> MacrosWithRedundantParens_;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
