#pragma once

#include "bridge_header.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace clang {
namespace tidy {
namespace sdc {

class SdcProhibitedFunctionsCheck : public ClangTidyCheck {
public:
    SdcProhibitedFunctionsCheck(StringRef Name, ClangTidyContext* Context);

    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

protected:
    // Subclasses must override this to return the list of prohibited functions.
    // The returned ArrayRef should ideally point to data with program lifetime
    // (like a static array) since matchers may hold references.
    virtual ArrayRef<StringRef> getProhibitedFunctions() const = 0;

    // Subclasses must override this to return the custom diagnostic message.
    // The function name is provided so subclasses can tailor the message.
    virtual std::string getDiagnosticMessage(StringRef FunctionName) const = 0;

    // Subclasses may override this to allow specific overloads of a prohibited
    // function (e.g. the 2-argument locale variant). Return true to suppress
    // the diagnostic for this declaration.
    virtual bool isAllowedDecl(const FunctionDecl* FD) const { return false; }
};

} // namespace sdc
} // namespace tidy
} // namespace clang
