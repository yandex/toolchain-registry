#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include <optional>

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace cast_utils {

                QualType canonicalUnqualified(QualType Type);
                bool isBool(QualType Type);
                bool isIntegralOrEnumeration(QualType Type);
                bool isObjectPointer(QualType Type);
                bool isFunctionPointer(QualType Type);
                bool isMemberFunctionPointer(QualType Type);
                bool isFunctionOrMemberPointer(QualType Type);
                bool isVoidPointer(QualType Type);
                bool isNullPointerConstant(const Expr* Expression, ASTContext& Context);
                bool isUintptrT(QualType Type, ASTContext& Context);
                QualType pointeeType(QualType Type);
                bool removesCvQualification(QualType From, QualType To);

                // Describes where a cv-qualification removal occurs.
                struct CvRemoval {
                    QualType QualifiedType; // type WITH the qualifier (e.g. 'const char')
                    bool     LostConst;
                    bool     LostVolatile;
                };
                // Returns the first level in the pointer/reference chain where
                // a qualifier is removed, or nullopt if none is.
                std::optional<CvRemoval> findCvRemoval(QualType From, QualType To);

            } // namespace cast_utils
        } // namespace sdc
    } // namespace tidy
} // namespace clang
