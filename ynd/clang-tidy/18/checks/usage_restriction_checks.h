#pragma once

#include "bridge_header.h"

namespace clang::tidy::arcadia {
    /// Finds usage of `typeid(smth).name`
    /// For more info see IGNF-1522
    class TypeidNameRestrictionCheck: public ClangTidyCheck {
    public:
        TypeidNameRestrictionCheck(StringRef Name, ClangTidyContext* Context)
            : ClangTidyCheck(Name, Context)
        {
        }
        void registerMatchers(ast_matchers::MatchFinder* Finder) override;
        void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
    };

}
