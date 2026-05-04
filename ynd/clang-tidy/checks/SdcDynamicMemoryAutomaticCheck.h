#pragma once
#include "bridge_header.h"

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
            class SdcDynamicMemoryAutomaticCheck: public ClangTidyCheck {
            public:
                SdcDynamicMemoryAutomaticCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                bool isInStdNamespace(const clang::CXXMethodDecl* Method);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
