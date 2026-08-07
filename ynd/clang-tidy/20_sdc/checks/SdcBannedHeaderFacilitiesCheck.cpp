#include "SdcBannedHeaderFacilitiesCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/Module.h"
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

const NamedDecl* findProhibitedTypedef(ArrayRef<StringRef> Prohibited,
                                       QualType QT) {
    // Do not use canonicalType here: canonicalization intentionally erases
    // typedef names, including the fact that an alias ultimately names a
    // prohibited standard-library typedef such as va_list.
    while (!QT.isNull()) {
        const Type* Ty = QT.getTypePtrOrNull();
        if (!Ty) return nullptr;
        if (const auto* TT = dyn_cast<TypedefType>(Ty)) {
            const TypedefNameDecl* TD = TT->getDecl();
            if (isProhibitedQualifiedName(Prohibited,
                                          TD->getQualifiedNameAsString())) {
                return TD;
            }
            QT = TD->getUnderlyingType();
            continue;
        }
        if (const auto* UT = dyn_cast<UsingType>(Ty)) {
            QT = UT->getUnderlyingType();
            continue;
        }
        if (const auto* ET = dyn_cast<ElaboratedType>(Ty)) {
            QT = ET->getNamedType();
            continue;
        }
        return nullptr;
    }
    return nullptr;
}

StringRef fileBaseName(StringRef Path) {
    return Path.rsplit('/').second;
}

bool isDefinitionFromStandardHeader(SourceLocation DefLoc,
                                    const SourceManager& SM,
                                    StringRef HeaderName) {
    if (DefLoc.isInvalid()) return false;
    if (SM.isInSystemHeader(DefLoc)) return true;

    // The qualification stubs deliberately model the standard library but
    // are supplied as ordinary include directories, so Clang does not mark
    // them as system headers. Recognize the standard header's basename while
    // still rejecting an unrelated user macro with the same spelling.
    StringRef Header = HeaderName.trim("<>");
    StringRef Base = fileBaseName(SM.getFilename(SM.getSpellingLoc(DefLoc)));
    if (Base == Header) return true;
    if (Header.starts_with("c") && Header.size() > 1) {
        std::string CHeader = (Header.drop_front() + ".h").str();
        if (Base == CHeader) return true;
    }
    return false;
}

class BannedFacilityPPCallbacks: public PPCallbacks {
public:
    BannedFacilityPPCallbacks(SdcBannedHeaderFacilitiesCheck* Check,
                              ArrayRef<StringRef> Macros,
                              ArrayRef<StringRef> Headers,
                              StringRef HeaderName,
                              const SourceManager& SM)
        : Check_(Check), Macros_(Macros), Headers_(Headers),
          HeaderName_(HeaderName), SM_(SM) {}

    void InclusionDirective(SourceLocation HashLoc,
                            const Token& /*IncludeTok*/,
                            StringRef FileName,
                            bool /*IsAngled*/,
                            CharSourceRange /*FilenameRange*/,
                            OptionalFileEntryRef /*File*/,
                            StringRef /*SearchPath*/,
                            StringRef /*RelativePath*/,
                            const Module* /*SuggestedModule*/,
                            bool /*ModuleImported*/,
                            SrcMgr::CharacteristicKind /*FileType*/) override {
        if (HashLoc.isInvalid() || SM_.isInSystemHeader(HashLoc)) return;
        // Do not blame the implementation detail where a C++ wrapper header
        // (for example <csetjmp>) includes its corresponding C header. This is
        // relevant for qualification stubs that are not marked as system
        // headers by Clang.
        if (isDefinitionFromStandardHeader(HashLoc, SM_, HeaderName_)) return;
        for (StringRef Header : Headers_) {
            if (FileName == Header) {
                Check_->recordHeaderUse(FileName, HashLoc);
                return;
            }
        }
    }

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
            if (!isDefinitionFromStandardHeader(DefLoc, SM_, HeaderName_)) {
                return;
            }
        }

        Check_->recordMacroUse(Name, ExpLoc);
    }

private:
    SdcBannedHeaderFacilitiesCheck* Check_;
    ArrayRef<StringRef> Macros_;
    ArrayRef<StringRef> Headers_;
    StringRef HeaderName_;
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
        std::make_unique<BannedFacilityPPCallbacks>(
            this, getProhibitedMacros(), getProhibitedHeaders(),
            getHeaderName(), SM));
}

void SdcBannedHeaderFacilitiesCheck::registerMatchers(MatchFinder* Finder) {
    ArrayRef<StringRef> Functions = getProhibitedFunctions();
    ArrayRef<StringRef> Types = getProhibitedTypes();
    ArrayRef<StringRef> Macros = getProhibitedMacros();

    if (!Macros.empty() && Functions.empty() && Types.empty()) {
        // If we only have macros, we must register at least one matcher so that
        // MatchFinder calls our onEndOfTranslationUnit hook.
        Finder->addMatcher(translationUnitDecl().bind("dummy"), this);
    }

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
            typeLoc(loc(typedefType()),
                    unless(hasAncestor(varDecl())),
                    unless(isExpansionInSystemHeader()))
                .bind("typeUse"),
            this);

        // Diagnose every declared object, not merely the shared type spelling
        // in declarations such as `va_list first, second`.
        Finder->addMatcher(
            varDecl(unless(parmVarDecl()),
                    unless(isExpansionInSystemHeader()))
                .bind("variableTypeUse"),
            this);

        // A qualified typedef spelling such as std::sig_atomic_t has an
        // ElaboratedTypeLoc wrapper around the underlying TypedefTypeLoc.
        // The matcher above sees the unqualified spelling but not this outer
        // form, so bind the typedef declaration explicitly for that case.
        Finder->addMatcher(
            elaboratedTypeLoc(
                hasNamedTypeLoc(anyOf(
                    loc(typedefType(hasDeclaration(namedDecl(hasAnyName(Types))))),
                    loc(usingType(hasUnderlyingType(typedefType(hasDeclaration(
                        namedDecl(hasAnyName(Types))))))))),
                unless(isExpansionInSystemHeader()))
                .bind("qualifiedTypeUse"),
            this);

        // Array parameters are adjusted to pointers in the AST. Their normal
        // TypeLoc therefore loses typedef sugar (notably va_list aliases), but
        // ParmVarDecl retains the source-level type through getOriginalType().
        Finder->addMatcher(
            parmVarDecl(unless(isExpansionInSystemHeader()))
                .bind("parameterTypeUse"),
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
        const NamedDecl* ND = findProhibitedTypedef(Types, TL->getType());
        if (!ND) return;
        TypeUses_.push_back({ND->getNameAsString(), TL->getBeginLoc()});
        return;
    }


    if (const auto* Parm =
            Result.Nodes.getNodeAs<ParmVarDecl>("parameterTypeUse")) {
        QualType Original = Parm->getOriginalType();
        const NamedDecl* ND = findProhibitedTypedef(Types, Original);
        if (!ND) return;
        TypeUses_.push_back({ND->getNameAsString(), Parm->getLocation()});
        return;
    }


    if (const auto* VD =
            Result.Nodes.getNodeAs<VarDecl>("variableTypeUse")) {
        const NamedDecl* ND = findProhibitedTypedef(Types, VD->getType());
        if (!ND) return;
        TypeUses_.push_back({ND->getNameAsString(), VD->getLocation()});
        return;
    }


    if (const auto* TL =
            Result.Nodes.getNodeAs<ElaboratedTypeLoc>("qualifiedTypeUse")) {
        const Type* NamedTy =
            TL->getNamedTypeLoc().getType().getTypePtrOrNull();
        const TypedefType* TT = NamedTy ? NamedTy->getAs<TypedefType>() : nullptr;
        if (!TT)
            if (const auto* UT = dyn_cast_or_null<UsingType>(NamedTy))
                TT = UT->getUnderlyingType()->getAs<TypedefType>();
        const NamedDecl* ND = TT ? TT->getDecl() : nullptr;
        if (!ND || !isProhibitedQualifiedName(
                       Types, ND->getQualifiedNameAsString()))
            return;
        TypeUses_.push_back({ND->getNameAsString(), TL->getBeginLoc()});
        return;
    }
}

void SdcBannedHeaderFacilitiesCheck::recordMacroUse(StringRef Name, SourceLocation Loc) {
    MacroUses_.push_back({Name.str(), Loc});
}

void SdcBannedHeaderFacilitiesCheck::recordHeaderUse(StringRef Name,
                                                     SourceLocation Loc) {
    HeaderUses_.push_back({Name.str(), Loc});
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

        // When a prohibited macro (e.g. offsetof) is expanded from within
        // another macro's body, MacroNameTok.getLocation() is a macro-body
        // expansion location rather than a file location.  Clang's diagnostic
        // engine then resolves it through the entire expansion chain and
        // reports the primary error at the outermost call site (user code),
        // making it look as though the user's innocent wrapper macro is the
        // offender and hiding where offsetof actually lives.
        //
        // Fix: report at the spelling location (the file where the prohibited
        // macro text is written) and add a note pointing back to the outermost
        // expansion in user code so both ends of the chain are visible.
        if (SM_ && U.Loc.isMacroID() && SM_->isMacroBodyExpansion(U.Loc)) {
            SourceLocation SpellingLoc = SM_->getSpellingLoc(U.Loc);
            // Walk expansion chain to the outermost file location (user code).
            SourceLocation UserLoc = U.Loc;
            while (UserLoc.isMacroID()) {
                UserLoc = SM_->getExpansionLoc(UserLoc);
            }
            diag(SpellingLoc, getDiagnosticMessage(U.Name));
            if (UserLoc.isValid() && UserLoc != SpellingLoc) {
                diag(UserLoc, "expanded via macro invocation here",
                     DiagnosticIDs::Note);
            }
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
    for (const auto& U : HeaderUses_) {
        Emit(U);
    }

    FunctionUses_.clear();
    TypeUses_.clear();
    MacroUses_.clear();
    HeaderUses_.clear();
    ExemptRanges_.clear();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
