#include "SdcCastUtils.h"

#include "clang/AST/Decl.h"
#include "clang/AST/ExprCXX.h"
#include <optional>

namespace {

clang::QualType nextAccessedType(clang::QualType Type) {
    clang::QualType Canonical = Type.getCanonicalType();
    if (Canonical->isPointerType()) {
        return Canonical->getPointeeType();
    }
    if (Canonical->isReferenceType()) {
        return Canonical->getPointeeType();
    }
    return clang::QualType();
}

clang::QualType firstAccessedType(clang::QualType Type) {
    clang::QualType Canonical = Type.getCanonicalType();
    if (Canonical->isPointerType()) {
        return Canonical->getPointeeType();
    }
    if (Canonical->isReferenceType()) {
        return Canonical->getPointeeType();
    }
    return Canonical;
}

} // namespace

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace cast_utils {

                QualType canonicalUnqualified(QualType Type) {
                    return Type.getCanonicalType().getUnqualifiedType();
                }

                bool isBool(QualType Type) {
                    return canonicalUnqualified(Type)->isBooleanType();
                }

                bool isIntegralOrEnumeration(QualType Type) {
                    QualType Canonical = canonicalUnqualified(Type);
                    return Canonical->isIntegerType() || Canonical->isEnumeralType();
                }

                bool isObjectPointer(QualType Type) {
                    QualType Canonical = canonicalUnqualified(Type);
                    return Canonical->isPointerType() && Canonical->getPointeeType()->isObjectType();
                }

                bool isFunctionPointer(QualType Type) {
                    QualType Canonical = canonicalUnqualified(Type);
                    return Canonical->isPointerType() && Canonical->getPointeeType()->isFunctionType();
                }

                bool isMemberFunctionPointer(QualType Type) {
                    QualType Canonical = canonicalUnqualified(Type);
                    return Canonical->isMemberFunctionPointerType();
                }

                bool isFunctionOrMemberPointer(QualType Type) {
                    QualType Canonical = canonicalUnqualified(Type);
                    return isFunctionPointer(Canonical) || Canonical->isMemberPointerType();
                }

                bool isVoidPointer(QualType Type) {
                    QualType Canonical = canonicalUnqualified(Type);
                    return Canonical->isPointerType() && Canonical->getPointeeType().getUnqualifiedType()->isVoidType();
                }

                bool isNullPointerConstant(const Expr* Expression, ASTContext& Context) {
                    return Expression && Expression->isNullPointerConstant(Context, Expr::NPC_ValueDependentIsNotNull);
                }

                bool isUintptrT(QualType Type, ASTContext& Context) {
                    QualType Canonical = canonicalUnqualified(Type);
                    if (!Canonical->isUnsignedIntegerType()) {
                        return false;
                    }

                    if (const auto* Typedef = Canonical->getAs<TypedefType>()) {
                        return Typedef->getDecl()->getName() == "uintptr_t";
                    }

                    return Context.getTypeSize(Canonical) == Context.getTypeSize(Context.VoidPtrTy);
                }

                QualType pointeeType(QualType Type) {
                    QualType Canonical = canonicalUnqualified(Type);
                    if (!Canonical->isPointerType()) {
                        return QualType();
                    }
                    return Canonical->getPointeeType();
                }

                std::optional<CvRemoval> findCvRemoval(QualType From, QualType To) {
                    QualType FromAccessed = firstAccessedType(From);
                    QualType ToAccessed = firstAccessedType(To);

                    while (!FromAccessed.isNull() && !ToAccessed.isNull()) {
                        Qualifiers FQ = FromAccessed.getQualifiers();
                        Qualifiers TQ = ToAccessed.getQualifiers();
                        bool ConstLost    = FQ.hasConst()    && !TQ.hasConst();
                        bool VolatileLost = FQ.hasVolatile() && !TQ.hasVolatile();
                        if (ConstLost || VolatileLost) {
                            return CvRemoval{FromAccessed, ConstLost, VolatileLost};
                        }
                        FromAccessed = nextAccessedType(FromAccessed);
                        ToAccessed   = nextAccessedType(ToAccessed);
                    }
                    return std::nullopt;
                }

                bool removesCvQualification(QualType From, QualType To) {
                    return findCvRemoval(From, To).has_value();
                }

            } // namespace cast_utils
        } // namespace sdc
    } // namespace tidy
} // namespace clang
