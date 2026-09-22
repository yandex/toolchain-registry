#pragma once
#include "SdcCodeSelection.h"
#include "bridge_header.h"
#include "clang/AST/DeclTemplate.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Regex.h"

namespace clang::tidy::sdc {
// Keep the frontend's reporting options available to policy diagnostics. The
// clang-tidy consumer accepts a whole diagnostic group when *any* note passes
// its filters; an instantiation note must not expand the primary's scope.
class SdcPolicyCheck : public ClangTidyCheck {
public:
    SdcPolicyCheck(StringRef Name, ClangTidyContext *Context)
        : ClangTidyCheck(Name, Context), TidyContext(Context),
          HeaderFilter(Context->getOptions().HeaderFilterRegex.value_or("")),
          ExcludeHeaderFilter(Context->getOptions().ExcludeHeaderFilterRegex.value_or("")) {}

    bool shouldReportPolicyDiagnostic(SourceLocation Location,
                                      const SourceManager &SM) const {
        if (Location.isInvalid()) return true;
        if (!TidyContext->getOptions().SystemHeaders.value_or(false) &&
            (SM.isInSystemHeader(Location) || SM.isInSystemMacro(Location)))
            return false;
        const auto File = SM.getFileEntryRefForID(SM.getDecomposedExpansionLoc(Location).first);
        if (!File) return true;
        const StringRef Name = File->getName();
        if (!SM.isInMainFile(Location) &&
            (!HeaderFilter.match(Name) || ExcludeHeaderFilter.match(Name)))
            return false;
        const auto &Filters = TidyContext->getGlobalOptions().LineFilter;
        if (Filters.empty()) return true;
        const unsigned Line = SM.getExpansionLineNumber(Location);
        for (const auto &Filter : Filters) {
            if (!Name.ends_with(Filter.Name)) continue;
            if (Filter.LineRanges.empty()) return true;
            for (const auto &Range : Filter.LineRanges)
                if (Range.first <= Line && Line <= Range.second) return true;
            return false;
        }
        return false;
    }

private:
    ClangTidyContext *TidyContext;
    llvm::Regex HeaderFilter;
    llvm::Regex ExcludeHeaderFilter;
};

inline std::string policyTypeName(QualType Type, const ASTContext &Context) {
    const auto &Policy = Context.getPrintingPolicy();
    const std::string Written = Type.getAsString(Policy);
    const QualType Canonical = Context.getCanonicalType(Type);
    const std::string Resolved = Canonical.getAsString(Policy);
    return "'" + Written + "'" + (Written == Resolved ? "" : " (aka '" + Resolved + "')");
}

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
bool diagnoseAnalysisInstance(SdcPolicyCheck &Check, const Decl *Instance,
                              ASTContext &Context, SourceLocation Location,
                              StringRef Message, const Args &...Arguments) {
    if (!Check.shouldReportPolicyDiagnostic(Location, Context.getSourceManager()))
        return false;
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
    return true;
}

inline bool emitPolicyDiagnostic(SdcPolicyCheck &Check, const DynTypedNode &Node,
                                 SourceLocation Location, StringRef Message,
                                 ASTContext &Context, AnalysisInstanceTracker &Instances) {
    bool Emitted = false;
    for (const Decl *Instance : Instances.claim(Node, Location, Context))
        Emitted |= diagnoseAnalysisInstance(Check, Instance, Context, Location, Message);
    return Emitted;
}
} // namespace clang::tidy::sdc
