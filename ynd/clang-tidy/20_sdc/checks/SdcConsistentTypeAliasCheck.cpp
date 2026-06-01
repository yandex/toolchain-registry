#include "SdcConsistentTypeAliasCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // QualType equality preserves typedef sugar - distinct typedef
                // nodes for the same canonical type compare unequal, which is
                // exactly what this rule wants. We compare the unqualified
                // QualType so that top-level cv-qualifiers - which are
                // discarded from the function signature for parameters - do
                // not cause false positives.
                bool sameAlias(QualType A, QualType B) {
                    return A.getUnqualifiedType() == B.getUnqualifiedType();
                }

                StringRef kindNoun(const NamedDecl* D) {
                    if (isa<ParmVarDecl>(D)) return "parameter";
                    if (isa<VarDecl>(D)) return "variable";
                    if (isa<FunctionDecl>(D)) return "function";
                    return "entity";
                }

            } // namespace

            SdcConsistentTypeAliasCheck::SdcConsistentTypeAliasCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcConsistentTypeAliasCheck::registerMatchers(MatchFinder* Finder) {
                // Anchor on the canonical (first) declaration so each entity
                // is examined once; we then walk the full redeclaration chain.
                // No `isFirstDecl()` matcher is exposed; we bind every decl
                // and filter in check() so each redecl chain is walked once.
                Finder->addMatcher(
                    varDecl(unless(isExpansionInSystemHeader()),
                            unless(parmVarDecl()))
                        .bind("var"),
                    this);
                Finder->addMatcher(
                    functionDecl(unless(isExpansionInSystemHeader()))
                        .bind("fn"),
                    this);
            }

            void SdcConsistentTypeAliasCheck::check(
                const MatchFinder::MatchResult& Result) {
                if (const auto* First = Result.Nodes.getNodeAs<VarDecl>("var")) {
                    if (!First->isFirstDecl()) return;
                    QualType Anchor = First->getType();
                    for (const Decl* R : First->redecls()) {
                        const auto* VD = cast<VarDecl>(R);
                        if (VD == First) continue;
                        if (!sameAlias(Anchor, VD->getType())) {
                            diag(VD->getTypeSpecStartLoc(),
                                 "%0 %1 is redeclared with type %2; the "
                                 "first declaration used %3")
                                << kindNoun(VD) << VD
                                << VD->getType() << Anchor;
                            diag(First->getLocation(),
                                 "previous declaration here",
                                 DiagnosticIDs::Note);
                        }
                    }
                    return;
                }

                if (const auto* First = Result.Nodes.getNodeAs<FunctionDecl>("fn")) {
                    if (!First->isFirstDecl()) return;
                    // Function templates and their instantiations: we anchor
                    // on the pattern; redecls of the pattern are what we want.
                    // For implicit instantiations there are no redeclarations
                    // at the source level, so the loop is a no-op.
                    QualType AnchorRet = First->getReturnType();
                    unsigned NumParams = First->getNumParams();
                    for (const FunctionDecl* R : First->redecls()) {
                        if (R == First) continue;
                        if (R->getNumParams() != NumParams) {
                            // Mismatch in arity means this is actually a
                            // different overload that happens to share the
                            // canonical chain - shouldn't happen in well-formed
                            // code, but skip defensively.
                            continue;
                        }
                        if (!sameAlias(AnchorRet, R->getReturnType())) {
                            diag(R->getReturnTypeSourceRange().getBegin(),
                                 "return type of %0 is redeclared as %1; the "
                                 "first declaration used %2")
                                << R << R->getReturnType() << AnchorRet;
                            diag(First->getLocation(),
                                 "previous declaration here",
                                 DiagnosticIDs::Note);
                        }
                        for (unsigned I = 0; I < NumParams; ++I) {
                            const ParmVarDecl* PA = First->getParamDecl(I);
                            const ParmVarDecl* PR = R->getParamDecl(I);
                            if (!sameAlias(PA->getType(), PR->getType())) {
                                diag(PR->getTypeSpecStartLoc(),
                                     "parameter %0 of %1 is redeclared with "
                                     "type %2; the first declaration used %3")
                                    << PR << R
                                    << PR->getType() << PA->getType();
                                diag(PA->getLocation(),
                                     "previous declaration here",
                                     DiagnosticIDs::Note);
                            }
                        }
                    }
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
