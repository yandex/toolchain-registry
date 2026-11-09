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
                //
                // Fallback: when the pointer comparison fails, compare the
                // printed representations. Inline namespaces (e.g. libcxx's
                // std::__1) can make the *same* typedef resolve to different
                // Type* nodes in different declaration contexts even within the
                // same translation unit.  Printing is stable and reflects what
                // a programmer actually writes, so it correctly treats
                // `::std::string` vs `::std::string` as identical while still
                // flagging genuinely different aliases like `int32_t` vs `int`.
                //
                // Strip ElaboratedType wrappers before comparison.  When a
                // class-member typedef/alias is written with its class qualifier
                // in an out-of-class method definition (e.g. `Projector::CloudPtr`
                // in the .cpp vs `CloudPtr` in the .h), Clang wraps the same
                // underlying TypeAliasType in an ElaboratedType to carry the
                // qualifier.  That makes the two QualTypes unequal even though
                // they refer to the exact same alias declaration.
                QualType stripElaborated(QualType Q) {
                    Q = Q.getUnqualifiedType();
                    while (const auto* ET =
                               dyn_cast<ElaboratedType>(Q.getTypePtr())) {
                        Q = ET->getNamedType().getUnqualifiedType();
                    }
                    return Q;
                }

                bool sameAlias(QualType A, QualType B, const PrintingPolicy& PP) {
                    if (A.getUnqualifiedType() == B.getUnqualifiedType())
                        return true;
                    // Strip class-qualification wrappers and retry.
                    QualType SA = stripElaborated(A);
                    QualType SB = stripElaborated(B);
                    if (SA == SB) return true;
                    // Qualification can appear beneath references, pointers,
                    // arrays and template arguments. Preserve alias identity at
                    // every level instead of canonicalizing away typedef sugar.
                    if (const auto* PA = dyn_cast<PointerType>(SA)) {
                        const auto* PB = dyn_cast<PointerType>(SB);
                        return PB && sameAlias(PA->getPointeeType(), PB->getPointeeType(), PP);
                    }
                    if (const auto* RA = dyn_cast<ReferenceType>(SA)) {
                        const auto* RB = dyn_cast<ReferenceType>(SB);
                        return RB && SA->getTypeClass() == SB->getTypeClass() &&
                            sameAlias(RA->getPointeeType(), RB->getPointeeType(), PP);
                    }
                    if (const auto* AA = dyn_cast<ArrayType>(SA)) {
                        const auto* AB = dyn_cast<ArrayType>(SB);
                        return AB && sameAlias(AA->getElementType(), AB->getElementType(), PP);
                    }
                    if (const auto* TA = dyn_cast<TemplateSpecializationType>(SA)) {
                        const auto* TB = dyn_cast<TemplateSpecializationType>(SB);
                        if (TB && TA->getTemplateName().getAsTemplateDecl() ==
                                      TB->getTemplateName().getAsTemplateDecl() &&
                            TA->template_arguments().size() == TB->template_arguments().size()) {
                            for (unsigned I = 0; I < TA->template_arguments().size(); ++I) {
                                const auto& AArg = TA->template_arguments()[I];
                                const auto& BArg = TB->template_arguments()[I];
                                if (AArg.getKind() != BArg.getKind()) return false;
                                if (AArg.getKind() == TemplateArgument::Type) {
                                    if (!sameAlias(AArg.getAsType(), BArg.getAsType(), PP)) return false;
                                } else if (!AArg.structurallyEquals(BArg)) return false;
                            }
                            return true;
                        }
                    }
                    return SA.getAsString(PP) == SB.getAsString(PP);
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
                const PrintingPolicy& PP = Result.Context->getPrintingPolicy();

                if (const auto* First = Result.Nodes.getNodeAs<VarDecl>("var")) {
                    if (!First->isFirstDecl()) return;
                    QualType Anchor = First->getType();
                    for (const Decl* R : First->redecls()) {
                        const auto* VD = cast<VarDecl>(R);
                        if (VD == First) continue;
                        if (!sameAlias(Anchor, VD->getType(), PP)) {
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
                    // Skip compiler-generated implicit declarations (e.g. the
                    // implicit operator new / operator new[] built-in). Their
                    // parameters use bare fundamental types (e.g. `unsigned long`)
                    // rather than typedef aliases (`std::size_t`), so comparing
                    // them against explicit library redeclarations always reports
                    // a false mismatch, and their source location is invalid so
                    // the "previous declaration here" note points nowhere.
                    if (First->isImplicit()) return;
                    // Function templates and their instantiations: we anchor
                    // on the pattern; redecls of the pattern are what we want.
                    // For implicit instantiations there are no redeclarations
                    // at the source level, so the loop is a no-op.
                    QualType AnchorRet = First->getReturnType();
                    unsigned NumParams = First->getNumParams();
                    for (const FunctionDecl* R : First->redecls()) {
                        if (R == First) continue;
                        // Skip compiler-generated redeclarations (e.g. implicit
                        // template instantiations) — they have no written source
                        // location and would produce a locationless diagnostic.
                        if (R->isImplicit()) continue;
                        if (R->getNumParams() != NumParams) {
                            // Mismatch in arity means this is actually a
                            // different overload that happens to share the
                            // canonical chain - shouldn't happen in well-formed
                            // code, but skip defensively.
                            continue;
                        }
                        if (!sameAlias(AnchorRet, R->getReturnType(), PP)) {
                            SourceLocation RetLoc =
                                R->getReturnTypeSourceRange().getBegin();
                            // Guard against invalid source locations (e.g. for
                            // compiler-synthesised redeclarations that slipped
                            // through the isImplicit() check).  Fall back to the
                            // function-name location so the diagnostic is at
                            // least anchored to a visible line.
                            if (!RetLoc.isValid()) RetLoc = R->getLocation();
                            if (!RetLoc.isValid()) continue;
                            diag(RetLoc,
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
                            if (!sameAlias(PA->getType(), PR->getType(), PP)) {
                                diag(PR->getTypeSpecStartLoc(),
                                     "parameter %0 of %1 is redeclared with "
                                     "type %2; the first declaration used %3")
                                    << PR << R
                                    << PR->getType() << PA->getType();
                                if (PA->getLocation().isValid()) {
                                    diag(PA->getLocation(),
                                         "previous declaration here",
                                         DiagnosticIDs::Note);
                                }
                            }
                        }
                    }
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
