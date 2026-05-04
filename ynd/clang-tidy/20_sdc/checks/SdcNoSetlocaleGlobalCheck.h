#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // setlocale and std::locale::global functions shall not be called
            class SdcNoSetlocaleGlobalCheck: public ClangTidyCheck {
            public:
                SdcNoSetlocaleGlobalCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
