#include "SdcEnumExplicitUnderlyingTypeCheck.h"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcEnumExplicitUnderlyingTypeCheck::SdcEnumExplicitUnderlyingTypeCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcEnumExplicitUnderlyingTypeCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        enumDecl(
            isDefinition(),
            unless(isExpansionInSystemHeader())
        ).bind("enum"),
        this);
}

namespace {

// Returns true if all enumerators in ED use their implicit default value
// (i.e. none carry an explicit initialiser expression).
bool allEnumeratorsAreDefault(const EnumDecl* ED) {
    for (const auto* ECD : ED->enumerators()) {
        if (ECD->getInitExpr() != nullptr)
            return false;
    }
    return true;
}

// Returns true if ED is declared inside an extern "C" linkage block.
bool isInExternCBlock(const EnumDecl* ED) {
    for (const DeclContext* DC = ED->getDeclContext(); DC; DC = DC->getParent()) {
        if (const auto* LS = dyn_cast<LinkageSpecDecl>(DC)) {
            if (LS->getLanguage() == LinkageSpecLanguageIDs::C)
                return true;
        }
    }
    return false;
}

} // namespace

void SdcEnumExplicitUnderlyingTypeCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* ED = Result.Nodes.getNodeAs<EnumDecl>("enum");
    if (!ED)
        return;

    // Already compliant: an explicit underlying type was written in source.
    // Note: ED->isFixed() also returns true for scoped enums (enum class/struct)
    // because they implicitly default to int — that is NOT sufficient.
    // getIntegerTypeSourceInfo() is only non-null when the enum-base was
    // explicitly written (e.g. ": int" or ": uint8_t").
    if (ED->getIntegerTypeSourceInfo() != nullptr)
        return;

    // Exception 1: all enumerators use default values — symbolic use where the
    // underlying type is intentionally left unspecified.
    if (allEnumeratorsAreDefault(ED))
        return;

    // Exception 2: declared inside an extern "C" block — intended for C interop.
    if (isInExternCBlock(ED))
        return;

    // Skip enum definitions that are members of a class-template instantiation;
    // the warning fires on the template definition itself.
    if (const auto* RD = dyn_cast<CXXRecordDecl>(ED->getDeclContext())) {
        if (RD->getTemplateSpecializationKind() != TSK_Undeclared)
            return;
    }

    diag(ED->getLocation(),
         "enumeration '%0' shall have an explicit underlying type")
        << ED->getName();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
