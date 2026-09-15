#pragma once
#include "SdcCodeSelection.h"
#include "bridge_header.h"
#include "clang/AST/DeclTemplate.h"
#include "llvm/Support/raw_ostream.h"

namespace clang::tidy::sdc {
// clang-tidy merges identical diagnostics at the same source location. Keep
// the concrete specialization in the message so POL-MAT-001 multiplicity
// survives the frontend's presentation layer as well as the AST tracker.
inline std::string analysisInstanceSuffix(const Decl *Instance, ASTContext &Context) {
    std::string Suffix;
    if (const auto *Named = dyn_cast_or_null<NamedDecl>(Instance)) {
        llvm::raw_string_ostream OS(Suffix);
        OS << " (in ";
        Named->getNameForDiagnostic(OS, Context.getPrintingPolicy(), true);
        OS << ')';
    }
    return Suffix;
}

template <typename... Args>
void diagnoseAnalysisInstance(ClangTidyCheck &Check, const Decl *Instance,
                              ASTContext &Context, SourceLocation Location,
                              StringRef Message, const Args &...Arguments) {
    std::string Format = Message.str() + "%" + std::to_string(sizeof...(Args));
    {
        auto Diagnostic = Check.diag(Location, Format);
        (Diagnostic << ... << Arguments);
        Diagnostic << analysisInstanceSuffix(Instance, Context);
    }

    SourceLocation Point;
    if (const auto *Function = dyn_cast_or_null<FunctionDecl>(Instance))
        Point = Function->getPointOfInstantiation();
    else if (const auto *Class = dyn_cast_or_null<ClassTemplateSpecializationDecl>(Instance))
        Point = Class->getPointOfInstantiation();
    else if (const auto *Variable = dyn_cast_or_null<VarTemplateSpecializationDecl>(Instance))
        Point = Variable->getPointOfInstantiation();

    // Clang retains a point of instantiation on each specialization. It does
    // not retain Sema's full active instantiation stack for clang-tidy checks.
    if (Point.isValid())
        Check.diag(Point, "in instantiation of %0 requested here", DiagnosticIDs::Note)
            << cast<NamedDecl>(Instance);
}

inline void emitPolicyDiagnostic(ClangTidyCheck &Check, const DynTypedNode &Node,
                                 SourceLocation Location, StringRef Message,
                                 ASTContext &Context, AnalysisInstanceTracker &Instances) {
    for (const Decl *Instance : Instances.claim(Node, Location, Context))
        diagnoseAnalysisInstance(Check, Instance, Context, Location, Message);
}
} // namespace clang::tidy::sdc
