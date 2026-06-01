#include "SdcVirtualBaseCastViaDynamicCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // Reaches the class-type "core" of a pointer-to-class,
                // reference-to-class, or directly a class-type (the latter
                // is what we see for the source of a reference cast, since
                // clang strips the reference when reading the lvalue's
                // type).
                const CXXRecordDecl* classFromPtrOrRef(QualType T) {
                    if (T.isNull()) return nullptr;
                    if (T->isPointerType()) {
                        T = T->getPointeeType();
                    } else if (T->isReferenceType()) {
                        T = T.getNonReferenceType();
                    }
                    return T->getAsCXXRecordDecl();
                }

            } // namespace

            SdcVirtualBaseCastViaDynamicCheck::
                SdcVirtualBaseCastViaDynamicCheck(StringRef Name,
                                                   ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcVirtualBaseCastViaDynamicCheck::registerMatchers(
                MatchFinder* Finder) {
                Finder->addMatcher(
                    explicitCastExpr(unless(isExpansionInSystemHeader()))
                        .bind("cast"),
                    this);
            }

            void SdcVirtualBaseCastViaDynamicCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* CE =
                    Result.Nodes.getNodeAs<ExplicitCastExpr>("cast");
                if (!CE) return;
                // dynamic_cast is the permitted form.
                if (isa<CXXDynamicCastExpr>(CE)) return;

                QualType FromTy = CE->getSubExpr()
                                      ->IgnoreParenImpCasts()
                                      ->getType();
                QualType ToTy = CE->getType();

                const CXXRecordDecl* FromCls = classFromPtrOrRef(FromTy);
                const CXXRecordDecl* ToCls = classFromPtrOrRef(ToTy);
                if (!FromCls || !ToCls) return;
                FromCls = FromCls->getDefinition();
                ToCls = ToCls->getDefinition();
                if (!FromCls || !ToCls || FromCls == ToCls) return;

                // We care only about downcasts: the destination must derive
                // from the source.
                if (!ToCls->isDerivedFrom(FromCls)) return;
                if (!ToCls->isVirtuallyDerivedFrom(FromCls)) return;

                diag(CE->getBeginLoc(),
                     "cast from virtual base %0 to derived %1 must use "
                     "dynamic_cast")
                    << FromCls << ToCls;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
