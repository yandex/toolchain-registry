//===--- TaxiAsyncUseAfterFreeCheck.cpp - clang-tidy ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "taxi_async_use_after_free_check.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::bugprone {

void TaxiAsyncUseAfterFreeCheck::registerMatchers(MatchFinder* Finder) {
    auto hasAsyncName = hasAnyName(
        "Async",
        "AsyncNoSpan",
        "SharedAsyncNoSpan",
        "CriticalAsyncNoSpan",
        "SharedCriticalAsyncNoSpan",
        "CriticalAsync",
        "SharedCriticalAsync"
    );

    Finder->addMatcher(
        lambdaExpr(hasParent(materializeTemporaryExpr(hasParent(callExpr(
                       hasParent(cxxBindTemporaryExpr(hasParent(materializeTemporaryExpr(hasParent(cxxMemberCallExpr(

                           callee(cxxMethodDecl(hasName("push_back"))),

                           on(declRefExpr(hasDeclaration(varDecl().bind("tasks"))))
                       )))))),

                       callee(functionDecl(hasAsyncName))
                   )))))
            .bind("lambda"),
        this
    );
}

void TaxiAsyncUseAfterFreeCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* MatchedLambda = Result.Nodes.getNodeAs<LambdaExpr>("lambda");
    const auto* MatchedTasks = Result.Nodes.getNodeAs<VarDecl>("tasks");
    const SourceLocation TasksLocation = MatchedTasks->getLocation();

    for (const auto& Capture : MatchedLambda->captures()) {
        if (!Capture.capturesVariable() || Capture.getCaptureKind() != LCK_ByRef) continue;

        const ValueDecl* CapturedVarDecl = Capture.getCapturedVar();
        if (CapturedVarDecl->getType().getCanonicalType()->isLValueReferenceType()) continue;
        if (CapturedVarDecl->getType().getCanonicalType()->isRValueReferenceType()) continue;

        if (CapturedVarDecl->getLocation() >= TasksLocation) {
            diag(Capture.getLocation(), "variable can be used after free");
            diag(CapturedVarDecl->getLocation(), "variable was declared here", DiagnosticIDs::Level::Note);
            diag(TasksLocation, "task container was declared here", DiagnosticIDs::Level::Note);
        }
    }
}

}  // namespace clang::tidy::bugprone
