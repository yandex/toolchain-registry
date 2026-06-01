#include "SdcNoReinterpretCastCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoReinterpretCastCheck::SdcNoReinterpretCastCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoReinterpretCastCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    cxxReinterpretCastExpr(unless(isExpansionInSystemHeader())).bind("cast"),
                    this);
            }

            void SdcNoReinterpretCastCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Cast = Result.Nodes.getNodeAs<CXXReinterpretCastExpr>("cast");
                if (!Cast) {
                    return;
                }

                QualType DestType = Cast->getType();
                QualType SrcType = Cast->getSubExpr()->getType();

                // Exception 1: reinterpret_cast<T*>(p) where T is void, char, unsigned char, or std::byte
                if (DestType->isPointerType() && SrcType->isPointerType()) {
                    QualType SrcPointee = SrcType->getPointeeType();
                    if (SrcPointee->isObjectType() || SrcPointee->isVoidType()) {
                        QualType Pointee = DestType->getPointeeType();
                        Pointee = Pointee.getUnqualifiedType(); // Remove cv-qualifiers

                        bool IsAllowedPointee = Pointee->isVoidType() ||
                                                Pointee->isSpecificBuiltinType(BuiltinType::Char_S) ||
                                                Pointee->isSpecificBuiltinType(BuiltinType::Char_U) ||
                                                Pointee->isSpecificBuiltinType(BuiltinType::UChar);

                        if (!IsAllowedPointee && Pointee->isEnumeralType()) {
                            // Check for std::byte
                            if (const auto* EnumDecl = Pointee->getAs<EnumType>()->getDecl()) {
                                if (EnumDecl->getName() == "byte" && EnumDecl->isInStdNamespace()) {
                                    IsAllowedPointee = true;
                                }
                            }
                        }

                        if (IsAllowedPointee) {
                            return; // Exempt
                        }
                    }
                }

                // Exception 2: reinterpret_cast<T>(p) to convert pointer to integer type
                if (DestType->isIntegerType() && SrcType->isPointerType()) {
                    ASTContext& Ctx = *Result.Context;
                    uint64_t DestSize = Ctx.getTypeSize(DestType);
                    uint64_t SrcSize = Ctx.getTypeSize(SrcType);

                    if (DestSize >= SrcSize) {
                        return; // Exempt (integer large enough)
                    }
                }

                diag(Cast->getBeginLoc(), "reinterpret_cast shall not be used");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
