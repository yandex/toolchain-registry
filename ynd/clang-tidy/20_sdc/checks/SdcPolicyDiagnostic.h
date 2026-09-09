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
    auto Diagnostic = Check.diag(Location, Format);
    (Diagnostic << ... << Arguments);
    Diagnostic << analysisInstanceSuffix(Instance, Context);
}

inline void emitPolicyDiagnostic(ClangTidyCheck &Check, const DynTypedNode &Node,
                                 SourceLocation Location, StringRef Message,
                                 ASTContext &Context, AnalysisInstanceTracker &Instances) {
    for (const Decl *Instance : Instances.claim(Node, Location, Context))
        diagnoseAnalysisInstance(Check, Instance, Context, Location, Message);
}
} // namespace clang::tidy::sdc
