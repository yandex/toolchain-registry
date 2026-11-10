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
                void recordParenParameter(SourceLocation Location);
                bool isParenParameter(SourceLocation Location) const;

            private:
                std::set<unsigned> ParenParameters_;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
