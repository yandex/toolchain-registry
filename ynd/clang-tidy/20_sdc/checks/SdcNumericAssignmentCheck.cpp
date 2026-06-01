#include "SdcNumericAssignmentCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/APSInt.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                enum class NumCat { Signed, Unsigned, Floating, NonNumeric };

                bool isCharacter(QualType T) {
                    const auto* BT = T->getAs<BuiltinType>();
                    if (!BT) return false;
                    switch (BT->getKind()) {
                        case BuiltinType::Char_S:
                        case BuiltinType::Char_U:
                        case BuiltinType::WChar_S:
                        case BuiltinType::WChar_U:
                        case BuiltinType::Char8:
                        case BuiltinType::Char16:
                        case BuiltinType::Char32:
                            return true;
                        default:
                            return false;
                    }
                }

                NumCat classify(QualType T) {
                    T = T.getCanonicalType().getUnqualifiedType();
                    if (T->isBooleanType()) return NumCat::NonNumeric;
                    if (isCharacter(T)) return NumCat::NonNumeric;
                    if (T->isFloatingType()) return NumCat::Floating;
                    if (T->isUnsignedIntegerType()) return NumCat::Unsigned;
                    if (T->isSignedIntegerType()) return NumCat::Signed;
                    return NumCat::NonNumeric;
                }

                unsigned bitSize(QualType T, ASTContext& Ctx) {
                    return static_cast<unsigned>(Ctx.getTypeSize(T));
                }

                bool isIdExpression(const Expr* E) {
                    if (!E) return false;
                    const Expr* Stripped = E->IgnoreParenImpCasts();
                    return isa<DeclRefExpr>(Stripped) ||
                           isa<MemberExpr>(Stripped);
                }

                bool intConstFits(const Expr* E, QualType Target,
                                  ASTContext& Ctx) {
                    Expr::EvalResult R;
                    if (!E->EvaluateAsInt(R, Ctx)) return false;
                    // Per rule 3a, an integer constant may target any numeric
                    // type with a range large enough to represent the value;
                    // for a floating target, the rule explicitly allows the
                    // value even if not exactly representable.
                    if (Target->isFloatingType()) return true;
                    if (!Target->isIntegerType()) return false;
                    llvm::APSInt V = R.Val.getInt();
                    unsigned TBits = bitSize(Target, Ctx);
                    bool TUnsigned = Target->isUnsignedIntegerType();
                    if (TUnsigned) {
                        if (V.isNegative()) return false;
                        // Value fits if its unsigned representation needs
                        // <= TBits bits.
                        return V.getActiveBits() <= TBits;
                    } else {
                        // Signed target: value range is
                        // [-(2^(TBits-1)), 2^(TBits-1) - 1].
                        return V.getSignificantBits() <= TBits;
                    }
                }

                // Same type at the rule's "shall have the same type"
                // granularity. We compare canonical, unqualified types.
                bool sameType(QualType A, QualType B) {
                    return A.getCanonicalType().getUnqualifiedType() ==
                           B.getCanonicalType().getUnqualifiedType();
                }

                // ---------- overload-independence -----------

                // The set of overloads visible at FD's lookup name in its
                // surrounding DeclContext, plus FD itself.
                void collectOverloads(const FunctionDecl* FD,
                                      llvm::SmallVectorImpl<const FunctionDecl*>& Out) {
                    if (!FD) return;
                    Out.push_back(FD);
                    DeclarationName N = FD->getDeclName();
                    const DeclContext* DC = FD->getDeclContext();
                    if (!DC) return;
                    // For class members, look up in the class so we see all
                    // overloads of this name in the same class.
                    for (auto* D : DC->lookup(N)) {
                        const auto* Other =
                            dyn_cast<FunctionDecl>(D->getCanonicalDecl());
                        if (Other && Other != FD->getCanonicalDecl()) {
                            Out.push_back(Other);
                        }
                    }
                }

                // True if for FD's overload set, every overload with arity
                // >= ParamIdx+1 has the same parameter type at position
                // ParamIdx as FD does.
                bool sameParamAcrossOverloads(const FunctionDecl* FD,
                                              unsigned ParamIdx) {
                    if (!FD || ParamIdx >= FD->getNumParams()) return true;
                    QualType T = FD->getParamDecl(ParamIdx)
                                     ->getType()
                                     .getCanonicalType()
                                     .getUnqualifiedType();
                    llvm::SmallVector<const FunctionDecl*, 8> Overloads;
                    collectOverloads(FD, Overloads);
                    for (const FunctionDecl* O : Overloads) {
                        if (O->getNumParams() <= ParamIdx) continue;
                        QualType OT = O->getParamDecl(ParamIdx)
                                          ->getType()
                                          .getCanonicalType()
                                          .getUnqualifiedType();
                        if (OT != T) return false;
                    }
                    return true;
                }

                bool isNonExtensibleCall(const CallExpr* CE) {
                    if (!CE) return false;
                    if (isa<CXXMemberCallExpr>(CE)) return true;
                    if (const auto* OC = dyn_cast<CXXOperatorCallExpr>(CE)) {
                        return OC->getOperator() == OO_Call;
                    }
                    return false;
                }

                bool isCallThroughFunctionPointer(const CallExpr* CE) {
                    return CE && CE->getDirectCallee() == nullptr;
                }

                bool argIsOverloadIndependent(const CallExpr* CE,
                                              unsigned ArgIdx) {
                    if (isCallThroughFunctionPointer(CE)) return true;
                    if (!isNonExtensibleCall(CE)) return false;
                    const FunctionDecl* FD = CE->getDirectCallee();
                    if (!FD) return false;
                    return sameParamAcrossOverloads(FD, ArgIdx);
                }

                // ---------- constructor widening exception -----------

                // Returns true if RD has exactly one non-copy/move ctor that
                // takes a single argument and IT IS Ctor. Used to gate the
                // "constructor widening" exception.
                bool isSoleSingleArgCtor(const CXXConstructorDecl* Ctor) {
                    if (!Ctor) return false;
                    const CXXRecordDecl* RD = Ctor->getParent();
                    if (!RD) return false;
                    RD = RD->getDefinition();
                    if (!RD) return false;
                    unsigned Count = 0;
                    bool Found = false;
                    for (const CXXConstructorDecl* C : RD->ctors()) {
                        if (C->isCopyOrMoveConstructor()) continue;
                        // Callable with a single argument means at least one
                        // required parameter and the rest defaulted, with
                        // total arity at least 1.
                        unsigned Min = C->getMinRequiredArguments();
                        unsigned Max = C->getNumParams();
                        if (Min <= 1 && Max >= 1) {
                            ++Count;
                            if (C->getCanonicalDecl() ==
                                Ctor->getCanonicalDecl()) {
                                Found = true;
                            }
                        }
                    }
                    return Found && Count == 1;
                }

            } // namespace

            SdcNumericAssignmentCheck::SdcNumericAssignmentCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNumericAssignmentCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    varDecl(unless(isExpansionInSystemHeader()),
                            hasInitializer(expr().bind("init")))
                        .bind("var"),
                    this);
                Finder->addMatcher(
                    binaryOperator(unless(isExpansionInSystemHeader()),
                                   hasOperatorName("="))
                        .bind("assign"),
                    this);
                Finder->addMatcher(
                    returnStmt(unless(isExpansionInSystemHeader()),
                               hasReturnValue(expr().bind("retval")))
                        .bind("ret"),
                    this);
                Finder->addMatcher(
                    callExpr(unless(isExpansionInSystemHeader())).bind("call"),
                    this);
                Finder->addMatcher(
                    cxxConstructExpr(unless(isExpansionInSystemHeader()))
                        .bind("ctor"),
                    this);
            }

            namespace {

                struct Report {
                    SourceLocation Loc;
                    std::string Msg;
                };

                // Apply the "all other assignments" rules (clauses 1-3) for a
                // source expression and a target type. AllowNonIdWiden is set
                // by the constructor-widening exception.
                void checkGeneralAssignment(const Expr* Src, QualType Dst,
                                            ASTContext& Ctx,
                                            ClangTidyCheck& Check,
                                            const char* ContextLabel,
                                            bool AllowNonIdWiden = false) {
                    if (!Src) return;
                    QualType SrcT = Src->IgnoreImpCasts()
                                        ->getType()
                                        .getCanonicalType()
                                        .getUnqualifiedType();
                    QualType DstT =
                        Dst.getCanonicalType().getUnqualifiedType();
                    if (sameType(SrcT, DstT)) return;
                    NumCat SC = classify(SrcT);
                    NumCat DC = classify(DstT);
                    if (SC == NumCat::NonNumeric || DC == NumCat::NonNumeric) {
                        return;
                    }
                    // Rule 3: integer constant that fits.
                    if (intConstFits(Src->IgnoreImpCasts(), DstT, Ctx)) return;
                    // Same category and signedness?
                    if (SC == DC) {
                        unsigned SS = bitSize(SrcT, Ctx);
                        unsigned DS = bitSize(DstT, Ctx);
                        if (SS == DS) return; // already same type at this point
                        if (SS < DS) {
                            // Widening: per rule 2, source must be an id-expr.
                            if (AllowNonIdWiden ||
                                isIdExpression(Src)) {
                                return;
                            }
                            Check.diag(
                                Src->getExprLoc(),
                                "implicit widening of a non-id-expression "
                                "from %0 to %1 in %2; widening is permitted "
                                "only when the source is an id-expression "
                                "(a named variable or member access)")
                                << Src->IgnoreImpCasts()->getType() << Dst
                                << ContextLabel;
                            return;
                        }
                        // Narrowing.
                        Check.diag(Src->getExprLoc(),
                                   "implicit narrowing from %0 to %1 in %2")
                            << Src->IgnoreImpCasts()->getType() << Dst
                            << ContextLabel;
                        return;
                    }
                    // Different numeric category or signedness.
                    Check.diag(Src->getExprLoc(),
                               "implicit conversion from %0 to %1 in %2 "
                               "changes type category or signedness")
                        << Src->IgnoreImpCasts()->getType() << Dst
                        << ContextLabel;
                }

                void checkCallArg(const CallExpr* CE, unsigned ArgIdx,
                                  const Expr* Arg, QualType ParamTy,
                                  bool Variadic, ASTContext& Ctx,
                                  ClangTidyCheck& Check) {
                    QualType SrcT = Arg->IgnoreImpCasts()
                                        ->getType()
                                        .getCanonicalType()
                                        .getUnqualifiedType();
                    if (Variadic) {
                        // For ellipsis, the rule requires the source type to
                        // equal the *promoted* type of the argument, i.e. the
                        // type the argument actually has at the boundary.
                        // After default argument promotions clang fills in
                        // the cast; if the *original* type already matches
                        // the promoted type we're fine - otherwise flag.
                        QualType Promoted =
                            Arg->getType().getCanonicalType().getUnqualifiedType();
                        if (!sameType(SrcT, Promoted)) {
                            if (classify(SrcT) == NumCat::NonNumeric) return;
                            Check.diag(Arg->getExprLoc(),
                                       "argument passed through ellipsis "
                                       "undergoes default promotion from %0 "
                                       "to %1; the source must already have "
                                       "the promoted type")
                                << Arg->IgnoreImpCasts()->getType()
                                << Arg->getType();
                        }
                        return;
                    }
                    if (argIsOverloadIndependent(CE, ArgIdx)) {
                        checkGeneralAssignment(Arg, ParamTy, Ctx, Check,
                                               "call argument");
                        return;
                    }
                    // Non-overload-independent: require strict same-type.
                    QualType DstT =
                        ParamTy.getCanonicalType().getUnqualifiedType();
                    if (sameType(SrcT, DstT)) return;
                    if (classify(SrcT) == NumCat::NonNumeric ||
                        classify(DstT) == NumCat::NonNumeric) {
                        return;
                    }
                    Check.diag(Arg->getExprLoc(),
                               "argument type %0 does not match parameter "
                               "type %1; the call site is not "
                               "overload-independent, so the types must be "
                               "identical")
                        << Arg->IgnoreImpCasts()->getType() << ParamTy;
                }

            } // namespace

            void SdcNumericAssignmentCheck::check(
                const MatchFinder::MatchResult& Result) {
                ASTContext& Ctx = *Result.Context;

                if (const auto* VD = Result.Nodes.getNodeAs<VarDecl>("var")) {
                    if (VD->isImplicit()) return;
                    if (isa<ParmVarDecl>(VD)) return; // default-arg init
                    if (VD->getInitStyle() == VarDecl::CallInit) {
                        // T x(expr) for class types is a CXXConstructExpr we
                        // handle separately; for built-ins (rare in real
                        // code) it's a builtin conversion which we want to
                        // check.
                    }
                    if (const Expr* Init =
                            Result.Nodes.getNodeAs<Expr>("init")) {
                        checkGeneralAssignment(Init, VD->getType(), Ctx, *this,
                                               "initialization");
                    }
                    return;
                }

                if (const auto* BO =
                        Result.Nodes.getNodeAs<BinaryOperator>("assign")) {
                    checkGeneralAssignment(BO->getRHS(), BO->getLHS()->getType(),
                                           Ctx, *this, "assignment");
                    return;
                }

                if (const auto* RS =
                        Result.Nodes.getNodeAs<ReturnStmt>("ret")) {
                    const FunctionDecl* FD = nullptr;
                    auto Parents = Ctx.getParents(*RS);
                    while (!Parents.empty()) {
                        if (const auto* F =
                                Parents[0].get<FunctionDecl>()) {
                            FD = F;
                            break;
                        }
                        if (const auto* D = Parents[0].get<Decl>()) {
                            Parents = Ctx.getParents(*D);
                        } else if (const auto* S =
                                       Parents[0].get<Stmt>()) {
                            Parents = Ctx.getParents(*S);
                        } else {
                            break;
                        }
                    }
                    if (!FD) return;
                    if (const Expr* Val =
                            Result.Nodes.getNodeAs<Expr>("retval")) {
                        checkGeneralAssignment(Val, FD->getReturnType(), Ctx,
                                               *this, "return");
                    }
                    return;
                }

                if (const auto* CE =
                        Result.Nodes.getNodeAs<CallExpr>("call")) {
                    const FunctionDecl* FD = CE->getDirectCallee();
                    unsigned NumParams = FD ? FD->getNumParams() : 0;
                    bool Variadic = FD ? FD->isVariadic() : false;
                    for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
                        const Expr* Arg = CE->getArg(I);
                        if (!Arg) continue;
                        if (I < NumParams) {
                            checkCallArg(CE, I, Arg,
                                         FD->getParamDecl(I)->getType(),
                                         /*Variadic=*/false, Ctx, *this);
                        } else if (Variadic) {
                            checkCallArg(CE, I, Arg, QualType(),
                                         /*Variadic=*/true, Ctx, *this);
                        }
                    }
                    return;
                }

                if (const auto* CXXC =
                        Result.Nodes.getNodeAs<CXXConstructExpr>("ctor")) {
                    const CXXConstructorDecl* Ctor = CXXC->getConstructor();
                    if (!Ctor) return;
                    // Constructor-widening exception applies when the ctor is
                    // a single-arg ctor and there are no other single-arg
                    // ctors apart from copy/move.
                    bool SoleSingleArg = isSoleSingleArgCtor(Ctor);
                    for (unsigned I = 0; I < CXXC->getNumArgs(); ++I) {
                        if (I >= Ctor->getNumParams()) break;
                        const Expr* Arg = CXXC->getArg(I);
                        if (!Arg) continue;
                        QualType ParamTy = Ctor->getParamDecl(I)->getType();
                        bool AllowNonIdWiden = false;
                        if (SoleSingleArg && CXXC->getNumArgs() == 1) {
                            // Determine if this is a widening within same
                            // category and signedness; if so allow even for
                            // non-id sources.
                            QualType SrcT = Arg->IgnoreImpCasts()
                                                ->getType()
                                                .getCanonicalType()
                                                .getUnqualifiedType();
                            QualType DstT = ParamTy.getCanonicalType()
                                                .getUnqualifiedType();
                            if (classify(SrcT) != NumCat::NonNumeric &&
                                classify(DstT) != NumCat::NonNumeric &&
                                classify(SrcT) == classify(DstT) &&
                                bitSize(SrcT, Ctx) < bitSize(DstT, Ctx)) {
                                AllowNonIdWiden = true;
                            }
                        }
                        checkGeneralAssignment(Arg, ParamTy, Ctx, *this,
                                               "construction",
                                               AllowNonIdWiden);
                    }
                    return;
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
