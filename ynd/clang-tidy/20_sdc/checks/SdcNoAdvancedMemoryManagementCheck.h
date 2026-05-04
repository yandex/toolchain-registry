#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Advanced memory management shall not be used
            //
            // Advanced memory management occurs when:
            // 1. An advanced memory management function is called (operator new/delete overloads
            //    not in the allowed list, std::launder, uninitialized_*, destroy*)
            // 2. The address of an advanced memory management function is taken
            // 3. A destructor is called explicitly
            // 4. Any operator new or operator delete is user-declared
            class SdcNoAdvancedMemoryManagementCheck: public ClangTidyCheck {
            public:
                SdcNoAdvancedMemoryManagementCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                bool isBannedMemoryFunction(const FunctionDecl* FD);
                bool isUserDeclaredOperatorNewDelete(const FunctionDecl* FD);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
