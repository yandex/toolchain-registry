#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Diagnoses return statements that return a reference or pointer
            // to an automatic-storage object, or return a lambda/function
            // object that captures such an object by reference or stores its
            // address. This custom check replaces reliance on Clang's
            // -Wreturn-stack-address
            class SdcNoReturnStackAddressCheck: public ClangTidyCheck {
            public:
                SdcNoReturnStackAddressCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
