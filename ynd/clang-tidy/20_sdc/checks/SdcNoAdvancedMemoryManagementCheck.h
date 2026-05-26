#pragma once

#include "SdcProhibitedFunctionsCheck.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

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
            class SdcNoAdvancedMemoryManagementCheck: public SdcProhibitedFunctionsCheck {
            public:
                SdcNoAdvancedMemoryManagementCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            protected:
                // Rules 1 & 2 for the <memory> function list: std::launder,
                // uninitialized_*, destroy*. The base class matches both calls
                // and address-of references in one pass.
                ArrayRef<StringRef> getProhibitedFunctions() const override;
                std::string getDiagnosticMessage(StringRef FunctionName) const override;

            private:
                bool isUserDeclaredOperatorNewDelete(const FunctionDecl* FD);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
