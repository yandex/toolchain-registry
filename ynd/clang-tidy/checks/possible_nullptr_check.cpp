//===------ possible_nullptr_check.cpp - clang-tidy -------------*- C++ -*-===//
//
// Part of the Checker Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"

#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/ExprOpenMP.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerHelpers.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallDescription.h"
#include "clang/AST/ParentMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Regex.h"

using namespace clang;
using namespace ento;

namespace {
    class PossibleNullptr: public Checker<check::Location, check::Bind, check::PostCall> {
        enum DerefKind {
            NullPointer,
            UndefinedPointerValue
        };
        BugType BT_Null{this, "Dereference of null pointer", categories::LogicError};
        BugType BT_Undef{this, "Dereference of undefined pointer value", categories::LogicError};
        void reportBug(DerefKind K, ProgramStateRef State, const Stmt* S, CheckerContext& C) const;
        bool suppressReport(CheckerContext& C, const Expr* E) const;

    public:
        llvm::SmallVector<llvm::StringRef, 4> NonNullFuncs;
        llvm::Regex IncludeSrcRe;
        llvm::Regex ExcludeSrcRe;
        bool ShouldScanFile(StringRef filepath) const {
            bool inc = IncludeSrcRe.match(filepath);
            bool excl = ExcludeSrcRe.match(filepath);
            return inc && !excl;
        }

        void checkPostCall(const CallEvent& Call, CheckerContext& C) const {
            ProgramStateRef State = C.getState();

            if (isNonNullPtr(Call)) {
                if (auto L = Call.getReturnValue().getAs<Loc>()) {
                    State = State->assume(*L, /*assumption=*/true);
                }
            }

            C.addTransition(State);
        }

        /// \returns Whether the method declaration has the attribute returns_nonnull.
        bool isNonNullPtr(const CallEvent& Call) const {
            QualType ExprRetType = Call.getResultType();
            const Decl* CallDeclaration = Call.getDecl();
            if (!ExprRetType->isAnyPointerType() || !CallDeclaration) {
                return false;
            }

            if (CallDeclaration->hasAttr<ReturnsNonNullAttr>()) {
                return true;
            }

            auto func = CallDeclaration->getAsFunction();
            if (!func) {
                return false;
            }
            if (!NonNullFuncs.empty()) {
                std::string full = func->getQualifiedNameAsString();
                llvm::StringRef ref = full;
                for (auto prefix : NonNullFuncs)
                    if(ref.starts_with(prefix))
                        return true; 
            }
            auto name = func->getNameInfo().getAsString();
            auto startswith = [&](const char* pre) { return name.rfind(pre, 0) == 0; };
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            bool is_proto = false;
            is_proto |= startswith("mutable_") || startswith("add_");
            is_proto |= startswith("mutable") || startswith("add");
            is_proto &= func->param_empty();
            return is_proto;
        }

        template <typename T>
        bool isPartOfRangeForExpression(const Stmt* S, T& C) const {
            const LocationContext* LC = C.getLocationContext();
            const ProgramStateRef state = C.getState();
            const ParentMap& PM = LC->getParentMap();
            while (S) {
                if (isa<CXXForRangeStmt>(S)) {
                    return true;
                }
                S = PM.getParent(S);
            }
            return false;
        }

        bool isLocalVariableOrFunctionParameter(SVal Loc) const {
            if (const MemRegion* Region = Loc.getAsRegion()) {
                if (auto SR = Region->getAs<VarRegion>()) {
                    auto* VD = SR->getDecl();
                    bool isParam = VD && isa<ParmVarDecl>(VD);
                    /* Dont analyze function params that marked as NonNull */
                    if (isParam && VD->hasAttr<NonNullAttr>()) return false;
                    return Region->hasStackStorage() || isParam;
                }
            }
            return false;
        }

        void checkLocation(SVal location, bool isLoad, const Stmt* S, CheckerContext& C) const;
        void checkBind(SVal L, SVal V, const Stmt* S, CheckerContext& C) const;
        static void AddDerefSource(raw_ostream& os, SmallVectorImpl<SourceRange>& Ranges, const Expr* Ex, const ProgramState* state, const LocationContext* LCtx, bool loadedFrom = false);
    };
} // namespace

void PossibleNullptr::AddDerefSource(raw_ostream& os, SmallVectorImpl<SourceRange>& Ranges, const Expr* Ex, const ProgramState* state, const LocationContext* LCtx, bool loadedFrom) {
    Ex = Ex->IgnoreParenLValueCasts();
    switch (Ex->getStmtClass()) {
        default:
            break;
        case Stmt::DeclRefExprClass: {
            const DeclRefExpr* DR = cast<DeclRefExpr>(Ex);
            if (const VarDecl* VD = dyn_cast<VarDecl>(DR->getDecl())) {
                os << " (" << (loadedFrom ? "loaded from" : "from") << " variable '" << VD->getName() << "')";
                Ranges.push_back(DR->getSourceRange());
            }
            break;
        }
        case Stmt::MemberExprClass: {
            const MemberExpr* ME = cast<MemberExpr>(Ex);
            os << " (" << (loadedFrom ? "loaded from" : "via") << " field '" << ME->getMemberNameInfo() << "')";
            SourceLocation L = ME->getMemberLoc();
            Ranges.push_back(SourceRange(L, L));
            break;
        }
    }
}

static const Expr* getDereferenceExpr(const Stmt* S, bool IsBind = false) {
    const Expr* E = nullptr;
    if (const Expr* expr = dyn_cast<Expr>(S)) {
        E = expr->IgnoreParenLValueCasts();
    }
    if (IsBind) {
        const VarDecl* VD;
        const Expr* Init;
        std::tie(VD, Init) = parseAssignment(S);
        if (VD && Init) {
            E = Init;
        }
    }
    return E;
}

static bool IsUsedCustomArrow(const Expr* E) {
    if (const auto* CE = dyn_cast<CXXOperatorCallExpr>(E)) {
        return true;
    }

    for (const Stmt* Child : E->children()) {
        if (const auto* CE = dyn_cast<Expr>(Child)) {
            if (IsUsedCustomArrow(CE)) {
                return true;
            }
        }
    }
    return false;
}

template <typename T>
static bool ShouldAnalyze(const Stmt* S, T& C, std::function<bool(StringRef)> ShouldScanSrc) {
    if (!S) {
        return false;
    }
    auto E = getDereferenceExpr(S);
    if (!E) {
        return false;
    }

    const SourceManager& SM = C.getSourceManager();
    auto SourceCodeFile = SM.getFilename(E->getExprLoc());
    bool ShouldSkipSource = !ShouldScanSrc(SourceCodeFile);

    switch (E->getStmtClass()) {
        case Stmt::MemberExprClass: {
            const MemberExpr* M = cast<MemberExpr>(E);

            auto MemberSourceLoc = M->getMemberLoc();
            const SourceManager& SM = C.getSourceManager();
            auto SourceCodeFile = SM.getFilename(MemberSourceLoc);

            auto CustomArrow = dyn_cast<CXXOperatorCallExpr>(M->getBase());
            auto ThisExp = dyn_cast<CXXThisExpr>(M->getBase());

            return M->isArrow() && !CustomArrow && !ThisExp && ShouldScanSrc(SourceCodeFile) && !IsUsedCustomArrow(M);
        }
        case Stmt::ArraySubscriptExprClass:
        case Stmt::UnaryOperatorClass:
            return !ShouldSkipSource;
        default:
            return false;
    }
}

bool PossibleNullptr::suppressReport(CheckerContext& C, const Expr* E) const {
    return false;
}

static bool isDeclRefExprToReference(const Expr* E) {
    if (const auto* DRE = dyn_cast<DeclRefExpr>(E)) {
        return DRE->getDecl()->getType()->isReferenceType();
    }
    return false;
}

void PossibleNullptr::reportBug(DerefKind K, ProgramStateRef State, const Stmt* S, CheckerContext& C) const {
    const BugType* BT = nullptr;
    llvm::StringRef DerefStr1;
    llvm::StringRef DerefStr2;
    switch (K) {
        case DerefKind::NullPointer:
            BT = &BT_Null;
            DerefStr1 = " results in a null pointer dereference";
            DerefStr2 = " results in a dereference of a null pointer";
            break;
        case DerefKind::UndefinedPointerValue:
            BT = &BT_Undef;
            DerefStr1 = " results in an undefined pointer dereference";
            DerefStr2 = " results in a dereference of an undefined pointer value";
            break;
    };
    ExplodedNode* N = C.generateErrorNode(State);
    if (!N) {
        return;
    }
    SmallString<100> buf;
    llvm::raw_svector_ostream os(buf);
    SmallVector<SourceRange, 2> Ranges;
    switch (S->getStmtClass()) {
        case Stmt::ArraySubscriptExprClass: {
            os << "Array access";
            const ArraySubscriptExpr* AE = cast<ArraySubscriptExpr>(S);
            AddDerefSource(os, Ranges, AE->getBase()->IgnoreParenCasts(), State.get(), N->getLocationContext());
            os << DerefStr1;
            break;
        }
        case Stmt::UnaryOperatorClass: {
            os << BT->getDescription();
            const UnaryOperator* U = cast<UnaryOperator>(S);
            AddDerefSource(os, Ranges, U->getSubExpr()->IgnoreParens(), State.get(), N->getLocationContext(), true);
            break;
        }
        case Stmt::MemberExprClass: {
            const MemberExpr* M = cast<MemberExpr>(S);
            if (M->isArrow() || isDeclRefExprToReference(M->getBase())) {
                os << "Access to field '" << M->getMemberNameInfo() << "'" << DerefStr2;
                AddDerefSource(os, Ranges, M->getBase()->IgnoreParenCasts(), State.get(), N->getLocationContext(), true);
            }
            break;
        }
        default:
            break;
    }
    auto report = std::make_unique<PathSensitiveBugReport>(*BT, buf.empty() ? BT->getDescription() : buf.str(), N);
    bugreporter::trackExpressionValue(N, bugreporter::getDerefExpr(S), *report);
    for (SmallVectorImpl<SourceRange>::iterator I = Ranges.begin(), E = Ranges.end(); I != E; ++I) {
        report->addRange(*I);
    }
    C.emitReport(std::move(report));
}

void PossibleNullptr::checkLocation(SVal l, bool isLoad, const Stmt* S, CheckerContext& C) const {
    auto scanMe = [this](StringRef src) { return this->ShouldScanFile(src); };
    if (!ShouldAnalyze(S, C, scanMe)) {
        const LocationContext* LC = C.getLocationContext();
        const ProgramStateRef state = C.getState();
        const ParentMap& PM = LC->getParentMap();

        S = PM.getParentIgnoreParens(S);

        if (!ShouldAnalyze(S, C, scanMe)) {
            return;
        }
    }
    if (isPartOfRangeForExpression(S, C)) {
        return;
    }
    if (!isLocalVariableOrFunctionParameter(l)) {
        return;
    }

    if (l.isUndef()) {
        return;
    }
    DefinedOrUnknownSVal location = l.castAs<DefinedOrUnknownSVal>();
    if (!isa<Loc>(location)) {
        return;
    }

    ProgramStateRef state = C.getState();

    SVal storedVal = state->getSVal(location.castAs<Loc>());
    bool isIterator = false;
    {
        const llvm::Regex iterator_regex(R"__(.*iterator.*)__");
        std::string str;
        llvm::raw_string_ostream stream(str);
        storedVal.dumpToStream(stream);
        isIterator = iterator_regex.match(str);
    }

    if (isa<DefinedOrUnknownSVal>(storedVal) && isa<Loc>(storedVal)) {
        location = storedVal.castAs<DefinedOrUnknownSVal>();
    }
    if (isIterator) {
        return; // dont hande iterators
    }

    ProgramStateRef notNullState, nullState;
    std::tie(notNullState, nullState) = state->assume(location);
    if (nullState) {
        const Expr* expr = getDereferenceExpr(S);
        if (expr && !suppressReport(C, expr)) {
            reportBug(DerefKind::NullPointer, nullState, expr, C);
            return;
        }
    }
    C.addTransition(notNullState);
}

void PossibleNullptr::checkBind(SVal L, SVal V, const Stmt* S, CheckerContext& C) const {
    auto scanMe = [this](StringRef src) { return this->ShouldScanFile(src); };
    if (!ShouldAnalyze(S, C, scanMe) || V.isUndef()) {
        return;
    }
    const MemRegion* MR = L.getAsRegion();
    const TypedValueRegion* TVR = dyn_cast_or_null<TypedValueRegion>(MR);
    if (!TVR || !TVR->getValueType()->isReferenceType()) {
        return;
    }
    ProgramStateRef State = C.getState();
    ProgramStateRef StNonNull, StNull;
    std::tie(StNonNull, StNull) = State->assume(V.castAs<DefinedOrUnknownSVal>());
    if (StNull) {
        const Expr* expr = getDereferenceExpr(S, /*IsBind=*/true);
        if (!suppressReport(C, expr)) {
            reportBug(DerefKind::NullPointer, StNull, expr, C);
            return;
        }
    }
    C.addTransition(State, this);
}

#if defined(IX_CLANG_TIDY_BUILD)
    #define CSA_REGISTER(CLS) void ento::register##CLS
    #define CSA_ENABLE(CLS) bool ento::shouldRegister##CLS
#else
    #define CSA_REGISTER(CLS) static void register##CLS
    #define CSA_ENABLE(CLS) static bool shouldRegister##CLS
#endif

CSA_REGISTER(PossibleNullptr)
(CheckerManager& mgr) {
    auto* check = mgr.registerChecker<PossibleNullptr>();
    auto checkName = mgr.getCurrentCheckerName();
    const auto& options = mgr.getAnalyzerOptions();

    check->IncludeSrcRe = llvm::Regex(options.getCheckerStringOption(checkName, "IncludeSrcRe"));
    check->ExcludeSrcRe = llvm::Regex(options.getCheckerStringOption(checkName, "ExcludeSrcRe"));
    options.getCheckerStringOption(checkName, "NonNullFuncs").split(check->NonNullFuncs, ',');
    llvm::erase_if(check->NonNullFuncs, [](auto& str) { return str.empty(); });
}

CSA_ENABLE(PossibleNullptr)
(const CheckerManager&) {
    return true;
}
