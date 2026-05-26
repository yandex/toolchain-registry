#pragma once

#include "SdcProhibitedFunctionsCheck.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace clang {
    namespace tidy {
        namespace sdc {

            // Dynamic memory shall be managed automatically
            //
            // A program shall not take the address of or use:
            // 1. Any non-placement form of new or delete
            // 2. Any of the functions malloc, calloc, realloc, aligned_alloc, free
            // 3. Any member function named allocate or deallocate enclosed by namespace std
            // 4. std::unique_ptr::release
            class SdcDynamicMemoryAutomaticCheck: public SdcProhibitedFunctionsCheck {
            public:
                SdcDynamicMemoryAutomaticCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            protected:
                // Rule 2: malloc family. Other rules use bespoke matchers below.
                ArrayRef<StringRef> getProhibitedFunctions() const override;
                std::string getDiagnosticMessage(StringRef FunctionName) const override;

            private:
                bool isInStdNamespace(const clang::CXXMethodDecl* Method);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
