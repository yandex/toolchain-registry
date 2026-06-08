#include "SdcImplicitEnumConstantUniqueValueCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcImplicitEnumConstantUniqueValueCheck::SdcImplicitEnumConstantUniqueValueCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcImplicitEnumConstantUniqueValueCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        enumDecl(isDefinition(), unless(isExpansionInSystemHeader())).bind("enum"),
        this);
}

namespace {

// Returns true when the initialiser expression is solely the name of another
// enumeration constant (possibly wrapped in parentheses or implicit casts).
// Only such initialisers trigger the exception that allows an implicit constant
// to share its value with the explicitly-specified one.
bool isJustEnumConstantName(const Expr* E) {
    if (!E) return false;
    const auto* DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
    return DRE && isa<EnumConstantDecl>(DRE->getDecl());
}

} // namespace

void SdcImplicitEnumConstantUniqueValueCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* ED = Result.Nodes.getNodeAs<EnumDecl>("enum");
    if (!ED) return;

    // Enum constants inside a class template pattern do not have their implicit
    // values computed at parse time (getInitVal() returns 0 for all of them).
    // Skip both the pattern and its instantiations; we would need a different
    // analysis path (walking instantiations) to handle these, which is out of
    // scope for a single-TU AST check.
    if (const auto* RD = dyn_cast<CXXRecordDecl>(ED->getDeclContext())) {
        // Template pattern.
        if (RD->getDescribedClassTemplate() != nullptr) return;
        // Template instantiation.
        if (RD->getTemplateSpecializationKind() != TSK_Undeclared) return;
    }

    // Collect enumerators in declaration order.
    llvm::SmallVector<const EnumConstantDecl*, 16> Enums(
        ED->enumerator_begin(), ED->enumerator_end());

    for (size_t i = 0; i < Enums.size(); ++i) {
        const EnumConstantDecl* Curr = Enums[i];
        if (Curr->getInitExpr() != nullptr) continue; // explicit — not subject to the rule

        const llvm::APSInt& V = Curr->getInitVal();

        // Find conflicting enumerators:
        // - Earlier implicit constant with same value (violates uniqueness; we
        //   flag the later one to report each pair exactly once).
        // - Any explicit constant whose initialiser is NOT just an enum-constant
        //   name (exception does not apply).
        const EnumConstantDecl* ImplicitConflict  = nullptr;
        const EnumConstantDecl* ExplicitConflict  = nullptr;

        for (size_t j = 0; j < Enums.size(); ++j) {
            if (j == i) continue;
            const EnumConstantDecl* Other = Enums[j];
            if (Other->getInitVal() != V) continue;

            if (Other->getInitExpr() == nullptr) {
                // Other is also implicit. Only flag Curr when Other comes
                // earlier (so each pair is reported exactly once on the later
                // member, not on both).
                if (j < i && !ImplicitConflict)
                    ImplicitConflict = Other;
            } else {
                // Other is explicit. Exception applies only when its initialiser
                // is just the name of another enumeration constant.
                if (!isJustEnumConstantName(Other->getInitExpr()) && !ExplicitConflict)
                    ExplicitConflict = Other;
            }
        }

        if (ImplicitConflict) {
            diag(Curr->getLocation(),
                 "implicit enumeration constant '%0' has the same value as "
                 "implicit '%1'")
                << Curr << ImplicitConflict;
        } else if (ExplicitConflict) {
            diag(Curr->getLocation(),
                 "implicit enumeration constant '%0' has the same value as "
                 "explicitly-specified '%1' whose initialiser is not the name "
                 "of another enumeration constant")
                << Curr << ExplicitConflict;
        }
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
