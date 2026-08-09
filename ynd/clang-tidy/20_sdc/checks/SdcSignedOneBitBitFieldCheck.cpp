#include "SdcSignedOneBitBitFieldCheck.h"

#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

bool isSignedIntegerType(QualType Type) {
    const auto* Builtin = Type.getCanonicalType()->getAs<BuiltinType>();
    if (!Builtin) {
        return false;
    }

    // Plain char and the character-width types are deliberately excluded:
    // they are not C++ signed integer types even on targets where their
    // representation uses a signed underlying type.
    switch (Builtin->getKind()) {
    case BuiltinType::SChar:
    case BuiltinType::Short:
    case BuiltinType::Int:
    case BuiltinType::Long:
    case BuiltinType::LongLong:
    case BuiltinType::Int128:
        return true;
    default:
        return false;
    }
}

} // namespace

SdcSignedOneBitBitFieldCheck::SdcSignedOneBitBitFieldCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcSignedOneBitBitFieldCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        fieldDecl(isBitField(), unless(isExpansionInSystemHeader())).bind("field"),
        this);
}

void SdcSignedOneBitBitFieldCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Field = Result.Nodes.getNodeAs<FieldDecl>("field");
    if (!Field || !Field->getIdentifier() || !isSignedIntegerType(Field->getType())) {
        return;
    }

    const Expr* Width = Field->getBitWidth();
    if (!Width || Width->isValueDependent() || Field->getBitWidthValue() != 1) {
        return;
    }

    diag(Field->getLocation(),
         "named bit-field '%0' has signed integer type and width one; use "
         "at least two bits or an unsigned type")
        << Field->getName();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
