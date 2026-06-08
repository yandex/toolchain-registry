#include "SdcBitFieldAppropriateTypeCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/APSInt.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcBitFieldAppropriateTypeCheck::SdcBitFieldAppropriateTypeCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcBitFieldAppropriateTypeCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        fieldDecl(isBitField(), unless(isExpansionInSystemHeader())).bind("bf"),
        this);
}

namespace {

// Returns true for explicitly-signed or explicitly-unsigned integer builtin
// types. Plain char (implementation-defined signedness), wchar_t, and
// character-width types are excluded.
bool isSignedOrUnsignedIntegerBuiltin(const BuiltinType* BT) {
    switch (BT->getKind()) {
    // Signed integer types.
    case BuiltinType::SChar:
    case BuiltinType::Short:
    case BuiltinType::Int:
    case BuiltinType::Long:
    case BuiltinType::LongLong:
    case BuiltinType::Int128:
    // Unsigned integer types.
    case BuiltinType::UChar:
    case BuiltinType::UShort:
    case BuiltinType::UInt:
    case BuiltinType::ULong:
    case BuiltinType::ULongLong:
    case BuiltinType::UInt128:
        return true;
    default:
        return false;
    }
}

// Returns true when Val is representable in a bit-field of the given width
// with the given signedness (matching the enum's underlying type sign).
bool fitsInBitField(const llvm::APSInt& Val, unsigned Width, bool Signed) {
    if (Width == 0) return false;
    if (Signed) return Val.isSignedIntN(Width);
    return !Val.isNegative() && Val.isIntN(Width);
}

} // namespace

void SdcBitFieldAppropriateTypeCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* FD = Result.Nodes.getNodeAs<FieldDecl>("bf");
    if (!FD) return;

    QualType T = FD->getType().getCanonicalType();

    // bool is always appropriate.
    if (T->isBooleanType()) return;

    // Enum type: requires a fixed underlying integer type, with all values
    // representable within the bit-field width.
    if (const auto* ET = T->getAs<EnumType>()) {
        const EnumDecl* ED = ET->getDecl();

        if (!ED->isFixed()) {
            diag(FD->getLocation(),
                 "bit-field '%0' has enumeration type '%1' with no fixed "
                 "underlying type; use an enum with an explicit underlying type")
                << FD->getName() << FD->getType();
            return;
        }

        QualType Underlying = ED->getIntegerType().getCanonicalType();
        const auto* UndBT = Underlying->getAs<BuiltinType>();
        if (!UndBT || !isSignedOrUnsignedIntegerBuiltin(UndBT)) {
            diag(FD->getLocation(),
                 "bit-field '%0' has enumeration type '%1' whose underlying "
                 "type is not a signed or unsigned integer type")
                << FD->getName() << FD->getType();
            return;
        }

        // Skip if the bit-field width is not a constant (dependent expression).
        if (FD->getBitWidth()->isValueDependent()) return;

        unsigned Width = FD->getBitWidthValue();
        bool Signed = Underlying->isSignedIntegerType();
        for (const auto* ECD : ED->enumerators()) {
            if (!fitsInBitField(ECD->getInitVal(), Width, Signed)) {
                diag(FD->getLocation(),
                     "bit-field '%0' of width %1 is too narrow to represent "
                     "all values of '%2'; enumerator '%3' does not fit")
                    << FD->getName() << Width << FD->getType()
                    << ECD->getName();
                return;
            }
        }
        return; // compliant
    }

    // Signed/unsigned integer builtin types.
    if (const auto* BT = T->getAs<BuiltinType>()) {
        if (isSignedOrUnsignedIntegerBuiltin(BT)) return; // compliant
    }

    diag(FD->getLocation(),
         "bit-field '%0' has type '%1' which is not appropriate for a "
         "bit-field; use bool, a signed or unsigned integer type, or an enum "
         "with a fixed underlying type whose values fit in the field width")
        << FD->getName() << FD->getType();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
