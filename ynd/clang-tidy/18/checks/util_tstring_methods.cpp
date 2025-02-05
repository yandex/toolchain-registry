#include "util_tstring_methods.h"

#include <llvm/ADT/DenseMap.h>
#include <clang/AST/ASTContext.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Lex/Lexer.h>

using namespace clang::ast_matchers;

namespace clang::tidy::arcadia {

namespace {
    const StringRef UpperCaseMethod = "Usage of deprecated method with in Uppercase";
}  // namespace

    UtilTStringUpperCaseMethodsCheck::UtilTStringUpperCaseMethodsCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
    {}

    void UtilTStringUpperCaseMethodsCheck::registerMatchers(MatchFinder* finder) {
        auto matcher = cxxMemberCallExpr(
            thisPointerType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(cxxRecordDecl(
                // TString use implementation from TStringBase.
                isSameOrDerivedFrom("::TStringBase")
            ))))),
            callee(cxxMethodDecl(hasAnyName("Data", "Empty", "Size")))
        ).bind("util-deprecated-check");

        finder->addMatcher(matcher, this);
    }

    void UtilTStringUpperCaseMethodsCheck::check(const MatchFinder::MatchResult& result) {
        const auto& callee = result.Nodes.getNodeAs<CXXMemberCallExpr>("util-deprecated-check")->getCallee();
        const auto memberExpr = dyn_cast<MemberExpr>(callee);
        std::string replace = memberExpr->getMemberDecl()->getName().str();
        replace[0] = std::tolower(replace[0]);
        SourceRange replacementRange(memberExpr->getMemberLoc(), callee->getEndLoc());
        diag(callee->getBeginLoc(), UpperCaseMethod) << FixItHint::CreateReplacement(replacementRange, replace);
        return;
    }

}  // namespace clang::tidy::arcadia
