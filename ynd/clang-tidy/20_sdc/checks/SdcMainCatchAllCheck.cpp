#include "SdcMainCatchAllCheck.h"
#include "SdcCodeSelection.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/DenseSet.h"

#if defined(IX_CLANG_TIDY_BUILD)
#include "../utils/ExceptionAnalyzer.h"
#else
#include <clang-tidy/utils/ExceptionAnalyzer.h>
#endif

#include <vector>

using namespace clang::ast_matchers;

namespace clang::tidy::sdc {
namespace {

struct Escape {
    SourceLocation Origin;
    SourceLocation Anchor;
    QualType Type; // Null means arbitrary exception types are possible.
};
using Escapes = std::vector<Escape>;

class EntryBody {
public:
    explicit EntryBody(ASTContext &Context) : Context(Context) {}

    bool HasCatchAll = false;

    void analyze(const Stmt *S, Escapes &Out, SourceLocation Use = {}) {
        if (!S) return;
        if (const auto *Try = dyn_cast<CXXTryStmt>(S)) {
            Escapes Uncaught;
            analyze(Try->getTryBlock(), Uncaught, Use);
            for (unsigned I = 0; I < Try->getNumHandlers(); ++I) {
                const auto *Handler = Try->getHandler(I);
                QualType Caught = Handler->getCaughtType();
                if (Caught.isNull()) {
                    HasCatchAll = true;
                    Uncaught.clear();
                } else {
                    // Calls have arbitrary exception types under the conservative
                    // specification policy. Typed handlers can filter explicit throws.
                    llvm::erase_if(Uncaught, [&](const Escape &E) {
                        if (E.Type.isNull()) return false;
                        utils::ExceptionAnalyzer::ExceptionInfo Info;
                        Info.registerException(E.Type.getCanonicalType().getTypePtr());
                        return Info.filterByCatch(
                            Caught.getNonReferenceType().getCanonicalType().getTypePtr(), Context);
                    });
                }
                // Sibling handlers do not protect one another. An enclosing
                // try may still catch failures from this handler.
                analyze(Handler->getHandlerBlock(), Out, Use);
                if (const auto *Variable = Handler->getExceptionDecl())
                    destructor(Variable->getType(), Variable->getLocation(), Out, Use);
            }
            Out.insert(Out.end(), Uncaught.begin(), Uncaught.end());
            return;
        }
        if (const auto *Lambda = dyn_cast<LambdaExpr>(S)) {
            // Creating a closure evaluates capture initializers, not its body.
            for (const Expr *Init : Lambda->capture_inits()) analyze(Init, Out, Use);
            return;
        }
        if (isa<UnaryExprOrTypeTraitExpr, CXXNoexceptExpr, TypeTraitExpr,
                ArrayTypeTraitExpr, ExpressionTraitExpr, RequiresExpr>(S)) return;
        if (const auto *Constant = dyn_cast<ConstantExpr>(S)) {
            if (Constant->isImmediateInvocation()) return;
        }
        if (const auto *If = dyn_cast<IfStmt>(S)) {
            if (If->isConstexpr()) {
                analyze(If->getInit(), Out, Use);
                analyze(If->getConditionVariableDeclStmt(), Out, Use);
                if (auto Selected = If->getNondiscardedCase(Context)) {
                    analyze(*Selected, Out, Use);
                    return;
                }
            }
        }
        if (const auto *Declarations = dyn_cast<DeclStmt>(S)) {
            for (const Decl *D : Declarations->decls()) {
                const auto *V = dyn_cast<VarDecl>(D);
                if (!V) continue; // Local classes/functions are not executed here.
                if (!V->isConstexpr() &&
                    !(V->hasGlobalStorage() && V->hasConstantInitialization()))
                    analyze(V->getInit(), Out, Use);
                // Local statics initialize on this path, but their destruction
                // at program/thread shutdown is outside the entry function.
                if (V->hasLocalStorage()) destructor(V->getType(), V->getLocation(), Out, Use);
            }
            return;
        }
        if (const auto *Argument = dyn_cast<CXXDefaultArgExpr>(S)) {
            analyze(Argument->getExpr(), Out, Use.isValid() ? Use : Argument->getUsedLocation());
            return;
        }
        if (const auto *Init = dyn_cast<CXXDefaultInitExpr>(S)) {
            analyze(Init->getExpr(), Out, Use.isValid() ? Use : Init->getUsedLocation());
            return;
        }
        if (const auto *Typeid = dyn_cast<CXXTypeidExpr>(S)) {
            if (!Typeid->isPotentiallyEvaluated()) return;
            const Expr *Operand = Typeid->getExprOperand()->IgnoreParenImpCasts();
            if (const auto *Deref = dyn_cast<UnaryOperator>(Operand))
                if (Deref->getOpcode() == UO_Deref) add(S->getBeginLoc(), Out, Use);
        }
        if (const auto *Throw = dyn_cast<CXXThrowExpr>(S)) {
            QualType Type;
            if (const Expr *Operand = Throw->getSubExpr())
                Type = Operand->getType().getUnqualifiedType();
            add(Throw->getThrowLoc(), Out, Use, Type);
        }
        if (const auto *Call = dyn_cast<CallExpr>(S)) {
            const Expr *Callee = Call->getCallee()->IgnoreParenImpCasts();
            if (!isa<CXXPseudoDestructorExpr>(Callee)) {
                QualType Type = Callee->getType();
                if (const auto *Function = Call->getDirectCallee()) {
                    Type = Function->getType();
                } else if (const auto *MemberPointer = dyn_cast<BinaryOperator>(Callee)) {
                    if (MemberPointer->isPtrMemOp()) Type = MemberPointer->getRHS()->getType();
                }
                if (Type->isPointerType() || Type->isReferenceType() ||
                    Type->isMemberPointerType() || Type->isBlockPointerType())
                    Type = Type->getPointeeType();
                call(Type, Call->getExprLoc(), Out, Use);
            }
        }
        if (const auto *Ctor = dyn_cast<CXXConstructExpr>(S))
            call(Ctor->getConstructor()->getType(), Ctor->getExprLoc(), Out, Use);
        if (const auto *Temporary = dyn_cast<CXXBindTemporaryExpr>(S))
            call(Temporary->getTemporary()->getDestructor()->getType(),
                 Temporary->getBeginLoc(), Out, Use);
        if (const auto *New = dyn_cast<CXXNewExpr>(S))
            if (const auto *Allocation = New->getOperatorNew())
                call(Allocation->getType(), New->getBeginLoc(), Out, Use);
        if (const auto *Delete = dyn_cast<CXXDeleteExpr>(S)) {
            if (const auto *Deallocation = Delete->getOperatorDelete())
                call(Deallocation->getType(), Delete->getBeginLoc(), Out, Use);
            destructor(Delete->getDestroyedType(), Delete->getBeginLoc(), Out, Use);
        }
        if (const auto *Cast = dyn_cast<CXXDynamicCastExpr>(S))
            if (Cast->getCastKind() == CK_Dynamic && Cast->getTypeAsWritten()->isReferenceType())
                add(Cast->getBeginLoc(), Out, Use);
        for (const Stmt *Child : S->children()) analyze(Child, Out, Use);
    }

private:
    void add(SourceLocation Origin, Escapes &Out, SourceLocation Use, QualType Type = {}) {
        Out.push_back({Origin, Use.isValid() ? Use : Origin, Type});
    }

    void call(QualType Type, SourceLocation Origin, Escapes &Out, SourceLocation Use) {
        const auto *Prototype = Type->getAs<FunctionProtoType>();
        if (!Prototype || !Prototype->isNothrow()) add(Origin, Out, Use);
    }

    void destructor(QualType Type, SourceLocation Origin, Escapes &Out, SourceLocation Use) {
        if (Type.isNull() || Type->isReferenceType()) return;
        Type = Context.getBaseElementType(Type);
        if (const auto *Record = Type->getAsCXXRecordDecl())
            if (const auto *Dtor = Record->getDestructor()) call(Dtor->getType(), Origin, Out, Use);
    }

    ASTContext &Context;
};

} // namespace

void SdcMainCatchAllCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(functionDecl(isMain(), isDefinition()).bind("main"), this);
}

void SdcMainCatchAllCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *Main = Result.Nodes.getNodeAs<FunctionDecl>("main");
    if (!Main || !isInAnalyzedCode(*Main, Main->getLocation(), *Result.Context)) return;

    EntryBody Body(*Result.Context);
    Escapes Uncaught;
    Body.analyze(Main->getBody(), Uncaught);
    if (!Body.HasCatchAll) {
        diag(Main->getLocation(), "main should contain a try block with a catch-all handler");
        return;
    }
    llvm::DenseSet<unsigned> Reported;
    for (const Escape &E : Uncaught) {
        if (!isWrittenInAnalyzedSource(E.Origin, *Result.SourceManager) ||
            !Reported.insert(E.Anchor.getRawEncoding()).second) continue;
        diag(E.Anchor, "this operation may propagate an exception outside main's catch-all protection");
    }
}

} // namespace clang::tidy::sdc
