//===--- missing_reserve_check.cpp - clang-tidy -------------------------===//
//
// Part of the Checker Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "missing_reserve_check.h"

#include "../utils/DeclRefExprUtils.h"

#include <clang/AST/ASTContext.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/SmallPtrSet.h>

#include <cassert>

using namespace clang::ast_matchers;

namespace clang::tidy::arcadia {
    namespace {

        constexpr char ContainerVarDeclName[] = "container_var_decl";
        constexpr char ContainerVarDeclStmtName[] = "container_var_decl_stmt";
        constexpr char GrowthCallName[] = "growth_call";
        constexpr char LoopCounterName[] = "for_loop_counter";
        constexpr char LoopEndExprName[] = "loop_end_expr";
        constexpr char LoopInitVarName[] = "loop_init_var";
        constexpr char LoopParentName[] = "loop_parent";
        constexpr char RangeLoopName[] = "for_range_loop";

        DeclarationMatcher sequenceContainerDecl() {
            return cxxRecordDecl(hasAnyName(
                "::std::vector",
                "::TVector",
                "::TBasicString",
                "::std::basic_string",
                "::TCompactVector",
                "::NYT::TCompactVector"));
        }

        ast_matchers::internal::Matcher<Expr> sizedRangeType() {
            // Associative containers are supported only as range sources. Their
            // size is the exact number of range-for iterations, but they are not
            // reserve targets: reserving a hash container is not a generally
            // useful requirement.
            return hasType(cxxRecordDecl(hasAnyName(
                "::std::array",
                "::std::basic_string",
                "::std::deque",
                "::std::map",
                "::std::set",
                "::std::unordered_map",
                "::std::unordered_set",
                "::std::vector",
                "::TBasicString",
                "::THashMap",
                "::THashMultiMap",
                "::THashMultiSet",
                "::THashSet",
                "::TVector",
                "::TCompactVector",
                "::NYT::TCompactVector")));
        }

        AST_MATCHER(Expr, hasSideEffects) { // NOLINT(readability-identifier-naming)
            return Node.HasSideEffects(Finder->getASTContext());
        }

        StringRef sourceText(const Expr& expression, const SourceManager& sourceManager, const LangOptions& langOpts) {
            return Lexer::getSourceText(
                CharSourceRange::getTokenRange(expression.getSourceRange()),
                sourceManager,
                langOpts);
        }

    } // namespace

    void MissingReserveCheck::addMatcher(
        const DeclarationMatcher& containerDecl,
        const DeclarationMatcher& growthMethodDecl,
        MatchFinder* finder)
    {
        const auto defaultConstructorCall = cxxConstructExpr(
            hasDeclaration(cxxConstructorDecl(
                isDefaultConstructor(),
                ofClass(containerDecl))));
        const auto containerVarDecl = varDecl(hasInitializer(defaultConstructorCall)).bind(ContainerVarDeclName);
        const auto containerVarDefStmt = declStmt(
                                             hasSingleDecl(equalsBoundNode(ContainerVarDeclName)))
                                             .bind(ContainerVarDeclStmtName);

        // Deliberately match the source variable instead of the class declaring
        // the method. TVector inherits push_back/emplace_back from std::vector, so
        // matching the method's implicit object type loses the TVector type.
        const auto growthCallExpr = cxxMemberCallExpr(
                                        callee(growthMethodDecl),
                                        onImplicitObjectArgument(ignoringParenImpCasts(
                                            declRefExpr(to(containerVarDecl)))))
                                        .bind(GrowthCallName);
        const auto growthCall = expr(ignoringImplicit(growthCallExpr));

        const auto loopVarInit = declStmt(hasSingleDecl(
            varDecl(hasInitializer(ignoringParenImpCasts(
                        integerLiteral(equals(0)))))
                .bind(LoopInitVarName)));
        const auto refersToLoopVar = ignoringParenImpCasts(
            declRefExpr(to(varDecl(equalsBoundNode(LoopInitVarName)))));
        const auto hasInterestingLoopBody = hasBody(anyOf(
            compoundStmt(statementCountIs(1), has(growthCall)),
            growthCall));
        const auto inInterestingCompoundStmt = hasParent(
            compoundStmt(has(containerVarDefStmt)).bind(LoopParentName));

        finder->addMatcher(
            forStmt(
                hasLoopInit(loopVarInit),
                hasCondition(binaryOperator(
                    hasOperatorName("<"),
                    hasLHS(refersToLoopVar),
                    hasRHS(expr(
                               unless(anyOf(
                                   hasDescendant(expr(refersToLoopVar)),
                                   hasSideEffects())))
                               .bind(LoopEndExprName)))),
                hasIncrement(unaryOperator(
                    hasOperatorName("++"),
                    hasUnaryOperand(refersToLoopVar))),
                hasInterestingLoopBody,
                inInterestingCompoundStmt)
                .bind(LoopCounterName),
            this);

        finder->addMatcher(
            cxxForRangeStmt(
                hasRangeInit(anyOf(
                    declRefExpr(sizedRangeType()),
                    memberExpr(
                        hasObjectExpression(unless(hasSideEffects())),
                        sizedRangeType()))),
                hasInterestingLoopBody,
                inInterestingCompoundStmt)
                .bind(RangeLoopName),
            this);
    }

    void MissingReserveCheck::registerMatchers(MatchFinder* finder) {
        addMatcher(
            sequenceContainerDecl(),
            cxxMethodDecl(hasAnyName("push_back", "emplace_back", "PushBack", "EmplaceBack")),
            finder);
    }

    void MissingReserveCheck::check(const MatchFinder::MatchResult& result) {
        ASTContext& context = *result.Context;
        if (context.getDiagnostics().hasUncompilableErrorOccurred()) {
            return;
        }

        const auto* containerVarDecl = result.Nodes.getNodeAs<VarDecl>(ContainerVarDeclName);
        const auto* growthCall = result.Nodes.getNodeAs<CXXMemberCallExpr>(GrowthCallName);
        const auto* forLoop = result.Nodes.getNodeAs<ForStmt>(LoopCounterName);
        const auto* rangeLoop = result.Nodes.getNodeAs<CXXForRangeStmt>(RangeLoopName);
        const auto* loopEndExpr = result.Nodes.getNodeAs<Expr>(LoopEndExprName);
        const auto* loopParent = result.Nodes.getNodeAs<CompoundStmt>(LoopParentName);

        assert(containerVarDecl && growthCall && loopParent && (forLoop || rangeLoop));

        const Stmt* loop = forLoop ? static_cast<const Stmt*>(forLoop) : rangeLoop;
        const SourceManager& sourceManager = *result.SourceManager;
        if (loop->getBeginLoc().isMacroID()) {
            return;
        }

        const llvm::SmallPtrSet<const DeclRefExpr*, 16> allVarRefs =
            utils::decl_ref_expr::allDeclRefExprs(*containerVarDecl, *loopParent, context);
        for (const auto* ref : allVarRefs) {
            if (sourceManager.isBeforeInTranslationUnit(ref->getLocation(), loop->getBeginLoc())) {
                return;
            }
        }

        std::string reserveSize;
        if (rangeLoop) {
            StringRef rangeExpression = sourceText(
                *rangeLoop->getRangeInit(),
                sourceManager,
                context.getLangOpts());
            reserveSize = (rangeExpression + ".size()").str();
        } else {
            reserveSize = sourceText(*loopEndExpr, sourceManager, context.getLangOpts()).str();
        }
        if (reserveSize.empty()) {
            return;
        }

        StringRef containerName = sourceText(
            *growthCall->getImplicitObjectArgument()->IgnoreParenImpCasts(),
            sourceManager,
            context.getLangOpts());
        if (containerName.empty()) {
            return;
        }

        std::string indentation = Lexer::getIndentationForLine(
                                      loop->getBeginLoc(), sourceManager)
                                      .str();
        std::string reserveStatement =
            (containerName + ".reserve(" + reserveSize + ");\n" + indentation).str();

        diag(
            growthCall->getBeginLoc(),
            "%0 is called inside a loop; reserve the container capacity before the loop")
            << growthCall->getMethodDecl()->getDeclName()
            << FixItHint::CreateInsertion(loop->getBeginLoc(), reserveStatement);
    }

} // namespace clang::tidy::arcadia
