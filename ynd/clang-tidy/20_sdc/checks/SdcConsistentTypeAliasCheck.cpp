#include "SdcConsistentTypeAliasCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <optional>

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // Strip spelling and substitution wrappers, but stop at real
                // typedef/alias declarations. Canonicalizing the whole type
                // would also erase the alias changes this check diagnoses.
                QualType stripTypeWrappers(QualType Q) {
                    Q = Q.getUnqualifiedType();
                    while (true) {
                        if (const auto* ET = dyn_cast<ElaboratedType>(Q))
                            Q = ET->getNamedType().getUnqualifiedType();
                        else if (const auto* ST = dyn_cast<SubstTemplateTypeParmType>(Q))
                            Q = ST->getReplacementType().getUnqualifiedType();
                        else if (const auto* UT = dyn_cast<UsingType>(Q))
                            Q = UT->getUnderlyingType().getUnqualifiedType();
                        else break;
                    }
                    return Q;
                }

                bool sameAlias(QualType A, QualType B, ASTContext& Ctx) {
                    if (A.getUnqualifiedType() == B.getUnqualifiedType())
                        return true;
                    // Strip class-qualification wrappers and retry.
                    QualType SA = stripTypeWrappers(A);
                    QualType SB = stripTypeWrappers(B);
                    if (SA == SB) return true;
                    if (isa<TypedefType>(SA) || isa<TypedefType>(SB)) {
                        const auto* TA = dyn_cast<TypedefType>(SA);
                        const auto* TB = dyn_cast<TypedefType>(SB);
                        return TA && TB && TA->getDecl()->getCanonicalDecl() == TB->getDecl()->getCanonicalDecl();
                    }
                    // Qualification can appear beneath references, pointers,
                    // arrays and template arguments. Preserve alias identity at
                    // every level instead of canonicalizing away typedef sugar.
                    if (const auto* PA = dyn_cast<PointerType>(SA)) {
                        const auto* PB = dyn_cast<PointerType>(SB);
                        return PB && sameAlias(PA->getPointeeType(), PB->getPointeeType(), Ctx);
                    }
                    if (const auto* RA = dyn_cast<ReferenceType>(SA)) {
                        const auto* RB = dyn_cast<ReferenceType>(SB);
                        return RB && SA->getTypeClass() == SB->getTypeClass() &&
                            sameAlias(RA->getPointeeType(), RB->getPointeeType(), Ctx);
                    }
                    if (const auto* AA = dyn_cast<ArrayType>(SA)) {
                        const auto* AB = dyn_cast<ArrayType>(SB);
                        return AB && sameAlias(AA->getElementType(), AB->getElementType(), Ctx);
                    }
                    auto Template = [](QualType T, ArrayRef<TemplateArgument>& Args) -> const TemplateDecl* {
                        if (const auto* ST = dyn_cast<TemplateSpecializationType>(T)) {
                            Args = ST->template_arguments();
                            return ST->getTemplateName().getAsTemplateDecl();
                        }
                        if (const auto* RT = dyn_cast<RecordType>(T)) {
                            if (const auto* Spec = dyn_cast<ClassTemplateSpecializationDecl>(RT->getDecl())) {
                                Args = Spec->getTemplateArgs().asArray();
                                return Spec->getSpecializedTemplate();
                            }
                        }
                        return nullptr;
                    };
                    ArrayRef<TemplateArgument> AArgs, BArgs;
                    const TemplateDecl* AT = Template(SA, AArgs);
                    const TemplateDecl* BT = Template(SB, BArgs);
                    if (AT && BT && AT->getCanonicalDecl() == BT->getCanonicalDecl() && AArgs.size() == BArgs.size()) {
                        for (unsigned I = 0; I < AArgs.size(); ++I) {
                            const auto& AArg = AArgs[I];
                            const auto& BArg = BArgs[I];
                            if (AArg.getKind() == TemplateArgument::Type && BArg.getKind() == TemplateArgument::Type) {
                                if (!sameAlias(AArg.getAsType(), BArg.getAsType(), Ctx)) return false;
                            } else {
                                auto Integral = [&](const TemplateArgument& Arg) -> std::optional<llvm::APSInt> {
                                    if (Arg.getKind() == TemplateArgument::Integral) return Arg.getAsIntegral();
                                    if (Arg.getKind() == TemplateArgument::Expression)
                                        return Arg.getAsExpr()->getIntegerConstantExpr(Ctx);
                                    return std::nullopt;
                                };
                                auto AV = Integral(AArg), BV = Integral(BArg);
                                if (AV && BV) {
                                    if (llvm::APSInt::compareValues(*AV, *BV) != 0) return false;
                                } else if (AArg.getKind() == TemplateArgument::Expression &&
                                           BArg.getKind() == TemplateArgument::Expression) {
                                    llvm::FoldingSetNodeID AI, BI;
                                    AArg.getAsExpr()->Profile(AI, Ctx, true);
                                    BArg.getAsExpr()->Profile(BI, Ctx, true);
                                    if (AI != BI) return false;
                                } else if (!AArg.structurallyEquals(BArg)) return false;
                            }
                        }
                        return true;
                    }
                    // A substituted specialization may be represented by its
                    // record type, while a written specialization retains sugar.
                    // Compare the template and arguments before falling back to
                    // printing; never erase a typedef to obtain this match.
                    const auto* RA = dyn_cast<RecordType>(SA);
                    const auto* RB = dyn_cast<RecordType>(SB);
                    if (RA && RB) return RA->getDecl()->getCanonicalDecl() == RB->getDecl()->getCanonicalDecl();
                    if (const auto* TA = dyn_cast<TemplateTypeParmType>(SA)) {
                        const auto* TB = dyn_cast<TemplateTypeParmType>(SB);
                        return TB && TA->getDepth() == TB->getDepth() &&
                            TA->getIndex() == TB->getIndex() && TA->isParameterPack() == TB->isParameterPack();
                    }
                    return SA.getAsString(Ctx.getPrintingPolicy()) == SB.getAsString(Ctx.getPrintingPolicy());
                }

                bool isWrittenDeclaration(const FunctionDecl* FD) {
                    if (FD->isImplicit()) return false;
                    const FunctionDecl* Pattern = FD->getInstantiatedFromMemberFunction();
                    if (!Pattern) Pattern = FD->getTemplateInstantiationPattern();
                    // Clang can mark the synthesized member explicit-specialization
                    // and relocate its name to the first specialization while
                    // retaining the pattern's source range and canonical types.
                    return !Pattern || FD->getBeginLoc() != Pattern->getBeginLoc();
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
                ASTContext& Ctx = *Result.Context;

                if (const auto* First = Result.Nodes.getNodeAs<VarDecl>("var")) {
                    if (!First->isFirstDecl()) return;
                    QualType Anchor = First->getType();
                    for (const Decl* R : First->redecls()) {
                        const auto* VD = cast<VarDecl>(R);
                        if (VD == First) continue;
                        if (!sameAlias(Anchor, VD->getType(), Ctx)) {
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
                    const FunctionDecl* Chain = First;
                    const FunctionDecl* Written = nullptr;
                    for (const FunctionDecl* R : Chain->redecls()) {
                        if (!isWrittenDeclaration(R)) continue;
                        if (!Written || Result.SourceManager->isBeforeInTranslationUnit(
                                R->getBeginLoc(), Written->getBeginLoc())) Written = R;
                    }
                    if (!Written) return;
                    First = Written;
                    QualType AnchorRet = First->getReturnType();
                    unsigned NumParams = First->getNumParams();
                    for (const FunctionDecl* R : Chain->redecls()) {
                        if (R == First) continue;
                        // Skip compiler-generated redeclarations (e.g. implicit
                        // template instantiations) — they have no written source
                        // location and would produce a locationless diagnostic.
                        if (!isWrittenDeclaration(R)) continue;
                        if (R->getNumParams() != NumParams) {
                            // Mismatch in arity means this is actually a
                            // different overload that happens to share the
                            // canonical chain - shouldn't happen in well-formed
                            // code, but skip defensively.
                            continue;
                        }
                        if (!sameAlias(AnchorRet, R->getReturnType(), Ctx)) {
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
                            if (!sameAlias(PA->getType(), PR->getType(), Ctx)) {
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
