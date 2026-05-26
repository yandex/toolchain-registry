#include "SdcBannedHeaderFacilitiesCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

StringRef stripGlobalPrefix(StringRef Name) {
    return Name.starts_with("::") ? Name.drop_front(2) : Name;
}

bool isProhibitedQualifiedName(ArrayRef<StringRef> Prohibited, StringRef QualName) {
    StringRef Normalized = stripGlobalPrefix(QualName);
    for (StringRef P : Prohibited) {
        if (stripGlobalPrefix(P) == Normalized) {
            return true;
        }
    }
    return false;
}

class BannedFacilityPPCallbacks: public PPCallbacks {
public:
    BannedFacilityPPCallbacks(SdcBannedHeaderFacilitiesCheck* Check,
                              ArrayRef<StringRef> Macros,
                              const SourceManager& SM)
        : Check_(Check), Macros_(Macros), SM_(SM) {}

    void MacroExpands(const Token& MacroNameTok,
                      const MacroDefinition& MD,
                      SourceRange /*Range*/,
                      const MacroArgs* /*Args*/) override {
        const IdentifierInfo* II = MacroNameTok.getIdentifierInfo();
        if (!II) {
            return;
        }
        StringRef Name = II->getName();

        bool Match = false;
        for (StringRef M : Macros_) {
            if (M == Name) {
                Match = true;
                break;
            }
        }
        if (!Match) {
            return;
        }

        const SourceLocation ExpLoc = MacroNameTok.getLocation();
        if (ExpLoc.isInvalid() || SM_.isInSystemHeader(ExpLoc)) {
            return;
        }

        // Only flag uses of *standard* macros. If the user has defined their
        // own macro with the same name, its MacroInfo lives in user code -
        // skip those, since they're not "facilities provided by <X>".
        if (const MacroInfo* MI = MD.getMacroInfo()) {
            SourceLocation DefLoc = MI->getDefinitionLoc();
            if (DefLoc.isValid() && !SM_.isInSystemHeader(DefLoc)) {
                return;
            }
        }

        Check_->recordMacroUse(Name, ExpLoc);
    }

private:
    SdcBannedHeaderFacilitiesCheck* Check_;
    ArrayRef<StringRef> Macros_;
    const SourceManager& SM_;
};

} // namespace

SdcBannedHeaderFacilitiesCheck::SdcBannedHeaderFacilitiesCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

std::string
SdcBannedHeaderFacilitiesCheck::getDiagnosticMessage(StringRef FacilityName) const {
    return ("facility '" + FacilityName + "' from " + getHeaderName() +
            " shall not be used").str();
}

void SdcBannedHeaderFacilitiesCheck::registerPPCallbacks(
    const SourceManager& SM, Preprocessor* PP, Preprocessor* /*ModuleExpanderPP*/) {
    SM_ = &SM;
    PP_ = PP;
    PP->addPPCallbacks(
        std::make_unique<BannedFacilityPPCallbacks>(this, getProhibitedMacros(), SM));
}

void SdcBannedHeaderFacilitiesCheck::registerMatchers(MatchFinder* Finder) {
    ArrayRef<StringRef> Functions = getProhibitedFunctions();
    ArrayRef<StringRef> Types = getProhibitedTypes();

    if (!Functions.empty()) {
        // Direct references (calls, address-of, etc.).
        Finder->addMatcher(
            declRefExpr(
                to(functionDecl(hasAnyName(Functions))),
                unless(isExpansionInSystemHeader()))
                .bind("funcUse"),
            this);

        // Unresolved (e.g. inside a still-dependent template).
        Finder->addMatcher(
            unresolvedLookupExpr(
                hasAnyDeclaration(namedDecl(hasAnyName(Functions))),
                unless(isExpansionInSystemHeader()))
                .bind("unresolvedFuncUse"),
            this);

        // Calls themselves - so subclasses can declare exception patterns.
        Finder->addMatcher(
            callExpr(
                callee(functionDecl(hasAnyName(Functions))),
                unless(isExpansionInSystemHeader()))
                .bind("call"),
            this);
    }

    if (!Types.empty()) {
        // Match TypeLoc whose declaration is one of the prohibited typedefs.
        // We pin to typedefType so we don't match the underlying primitive
        // (`int` rather than `sig_atomic_t`).
        Finder->addMatcher(
            typeLoc(
                loc(typedefType(hasDeclaration(namedDecl(hasAnyName(Types))))),
                unless(isExpansionInSystemHeader()))
                .bind("typeUse"),
            this);
    }
}

void SdcBannedHeaderFacilitiesCheck::check(const MatchFinder::MatchResult& Result) {
    ArrayRef<StringRef> Functions = getProhibitedFunctions();
    ArrayRef<StringRef> Types = getProhibitedTypes();

    if (const auto* Call = Result.Nodes.getNodeAs<CallExpr>("call")) {
        if (isExemptCall(Call)) {
            ExemptRanges_.push_back(Call->getSourceRange());
        }
        return;
    }

    if (const auto* DRE = Result.Nodes.getNodeAs<DeclRefExpr>("funcUse")) {
        const NamedDecl* ND = DRE->getDecl();
        if (!isProhibitedQualifiedName(Functions, ND->getQualifiedNameAsString())) {
            return;
        }
        FunctionUses_.push_back({ND->getNameAsString(), DRE->getBeginLoc()});
        return;
    }

    if (const auto* ULE = Result.Nodes.getNodeAs<UnresolvedLookupExpr>("unresolvedFuncUse")) {
        for (const NamedDecl* ND : ULE->decls()) {
            if (isProhibitedQualifiedName(Functions, ND->getQualifiedNameAsString())) {
                FunctionUses_.push_back({ND->getNameAsString(), ULE->getBeginLoc()});
                break;
            }
        }
        return;
    }

    if (const auto* TL = Result.Nodes.getNodeAs<TypeLoc>("typeUse")) {
        const Type* Ty = TL->getType().getTypePtrOrNull();
        if (!Ty) {
            return;
        }
        const auto* TT = Ty->getAs<TypedefType>();
        if (!TT) {
            return;
        }
        const NamedDecl* ND = TT->getDecl();
        if (!isProhibitedQualifiedName(Types, ND->getQualifiedNameAsString())) {
            return;
        }
        TypeUses_.push_back({ND->getNameAsString(), TL->getBeginLoc()});
        return;
    }
}

void SdcBannedHeaderFacilitiesCheck::recordMacroUse(StringRef Name, SourceLocation Loc) {
    MacroUses_.push_back({Name.str(), Loc});
}

bool SdcBannedHeaderFacilitiesCheck::isInExemptRange(SourceLocation Loc) const {
    if (!SM_ || Loc.isInvalid()) {
        return false;
    }
    SourceLocation ExpLoc = SM_->getExpansionLoc(Loc);
    if (ExpLoc.isInvalid()) {
        return false;
    }
    for (const SourceRange& R : ExemptRanges_) {
        SourceLocation B = SM_->getExpansionLoc(R.getBegin());
        SourceLocation E = SM_->getExpansionLoc(R.getEnd());
        if (B.isInvalid() || E.isInvalid()) {
            continue;
        }
        // [B, E] inclusive at the token-start level. ExpLoc inside?
        // Compare via offset within the same file.
        FileID BF = SM_->getFileID(B);
        FileID EF = SM_->getFileID(E);
        FileID LF = SM_->getFileID(ExpLoc);
        if (BF != LF || EF != LF) {
            continue;
        }
        unsigned BO = SM_->getFileOffset(B);
        unsigned EO = SM_->getFileOffset(E);
        unsigned LO = SM_->getFileOffset(ExpLoc);
        if (LO >= BO && LO <= EO) {
            return true;
        }
    }
    return false;
}

void SdcBannedHeaderFacilitiesCheck::onEndOfTranslationUnit() {
    auto Emit = [&](const DeferredUse& U) {
        if (isInExemptRange(U.Loc)) {
            return;
        }
        diag(U.Loc, getDiagnosticMessage(U.Name));
    };

    for (const auto& U : FunctionUses_) {
        Emit(U);
    }
    for (const auto& U : TypeUses_) {
        Emit(U);
    }
    for (const auto& U : MacroUses_) {
        Emit(U);
    }

    FunctionUses_.clear();
    TypeUses_.clear();
    MacroUses_.clear();
    ExemptRanges_.clear();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
