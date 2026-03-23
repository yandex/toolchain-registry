#pragma once

#include "bridge_header.h"

namespace clang::tidy::arcadia {

/// Finds using namespace directives in header files.
///
/// The check prohibits using-directives in header files to prevent
/// namespace pollution in files that include the header.
///
/// \code
///   // Forbidden in .h files -- This pollutes the namespace.
///   using namespace foo;
/// \endcode
///
/// The check only applies to header files (.h, .hpp, .hxx, etc.).
/// Using namespace directives in .cpp files are allowed.
class UsingNamespaceInHeaderCheck : public ClangTidyCheck {
public:
    UsingNamespaceInHeaderCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
    {}

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
        return langOpts.CPlusPlus;
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& result) override;

private:
    static bool isStdLiteralsNamespace(const NamespaceDecl* NS);
    static bool isHeaderFile(StringRef filename);
};

}  // namespace clang::tidy::arcadia
