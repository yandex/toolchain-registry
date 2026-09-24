//===--- missing_reserve_check.h - clang-tidy ---------------*- C++ -*-===//
//
// Part of the Checker Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "bridge_header.h"

namespace clang::tidy::arcadia {

    /// Finds repeated appends to an empty sequence container when a useful
    /// capacity bound is known before the loop and suggests reserving it.
    class MissingReserveCheck: public ClangTidyCheck {
    public:
        MissingReserveCheck(StringRef name, ClangTidyContext* context)
            : ClangTidyCheck(name, context)
        {
        }

        bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
            return langOpts.CPlusPlus;
        }

        void registerMatchers(ast_matchers::MatchFinder* finder) override;
        void check(const ast_matchers::MatchFinder::MatchResult& result) override;

    private:
        void addMatcher(
            const ast_matchers::DeclarationMatcher& containerDecl,
            const ast_matchers::DeclarationMatcher& growthMethodDecl,
            ast_matchers::MatchFinder* finder);
    };

} // namespace clang::tidy::arcadia
