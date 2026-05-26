#pragma once

#include "bridge_header.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace clang {
namespace tidy {
namespace sdc {

// Base for "facilities of header <X> shall not be used" rules.
//
// A "facility" per these rules is anything specified in [X.syn]:
// functions, types (typedefs / structs), and macros. Each kind reaches
// the analyzer through a different path:
//
//   * Functions  - AST DeclRefExpr / UnresolvedLookupExpr
//   * Types      - AST TypeLoc whose declaration is the named typedef
//   * Macros     - PP MacroExpands callback (they're gone by AST time)
//
// Many of these rules also include a narrow exception (e.g. csignal allows
// `signal(X, SIG_IGN)`). Exceptions can mention permitted *calls*, and uses
// of macros inside the permitted call should also be exempt. To get that
// right consistently we defer all diagnostics to onEndOfTranslationUnit:
// collected uses are filtered against the source ranges of permitted calls
// before emitting.
//
// Subclasses provide:
//   * the lists of qualified function names, qualified type names, and
//     macro names (raw, unqualified - macros have no namespace);
//   * the rule's header name (for the diagnostic);
//   * optionally, isExemptCall() to declare exception patterns.
class SdcBannedHeaderFacilitiesCheck: public ClangTidyCheck {
public:
    SdcBannedHeaderFacilitiesCheck(StringRef Name, ClangTidyContext* Context);

    void registerPPCallbacks(const SourceManager& SM,
                             Preprocessor* PP,
                             Preprocessor* ModuleExpanderPP) override;
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
    void onEndOfTranslationUnit() override;

    // Called by the PP callback. Public so the callback can reach it.
    void recordMacroUse(StringRef Name, SourceLocation Loc);

protected:
    // Subclass hooks. Each ArrayRef must point to data with program
    // lifetime (e.g. a static array), since matchers / callbacks may hold
    // references for the duration of the TU.
    virtual ArrayRef<StringRef> getProhibitedFunctions() const = 0;
    virtual ArrayRef<StringRef> getProhibitedTypes() const = 0;
    virtual ArrayRef<StringRef> getProhibitedMacros() const = 0;

    // Header spelling used in diagnostics, e.g. "<csignal>".
    virtual StringRef getHeaderName() const = 0;

    // Default diagnostic message; subclasses can override.
    virtual std::string getDiagnosticMessage(StringRef FacilityName) const;

    // Exception hook: return true if this call is permitted by an
    // exception in the rule. The call's source range will be recorded and
    // macro uses within it will be suppressed. The matched call is always
    // a direct call to one of the prohibited functions.
    virtual bool isExemptCall(const CallExpr* Call) const { return false; }

    const SourceManager* getSourceManager() const { return SM_; }

private:
    struct DeferredUse {
        std::string Name;
        SourceLocation Loc;
    };

    bool isInExemptRange(SourceLocation Loc) const;

    const SourceManager* SM_ = nullptr;
    Preprocessor* PP_ = nullptr;

    SmallVector<DeferredUse, 16> FunctionUses_;
    SmallVector<DeferredUse, 8>  TypeUses_;
    SmallVector<DeferredUse, 32> MacroUses_;
    SmallVector<SourceRange, 4>  ExemptRanges_;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
