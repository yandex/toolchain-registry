#include "uneeded_temporary_string_check.h"

#include <llvm/ADT/DenseMap.h>
#include <clang/AST/ASTContext.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Lex/Lexer.h>

#include <cassert>  // no util :(

using namespace clang::ast_matchers;

namespace clang::tidy::arcadia {

namespace {

    // ========== String types and info for them ==========

    const std::initializer_list<StringRef> StringViewLikeClasses = {
        "::TBasicStringBuf",
        "::std::basic_string_view",
    };

    const std::initializer_list<StringRef> StringLikeClasses = {
        "::TBasicString",
        "::std::basic_string",
    };

    using ReplaceableConstructorCheck = bool(const CXXConstructorDecl&);

    bool TStringReplaceableWithView(const CXXConstructorDecl& ctor) {
        switch (ctor.getNumParams()) {
        case 1: {
            StringRef paramName = ctor.getParamDecl(0)->getName();
            // Type checks would be more robust, but are harder to write, especially for dependent paramerer types.
            if (
                paramName == "rt"  // TBasicString(::NDetail::TReserveTag rt)
                || paramName == "uninitialized" // TBasicString(TUninitialized uninitialized)
                || paramName == "c"  // TBasicString(TExplicitType<TCharType> c), TBasicString(const reference& c)
            ) {
                return false;
            }
            return true;
        } case 2:
            if (ctor.getParamDecl(0)->getName() == "n") {  // TBasicString(size_t n, TCharType c)
                return false;
            }
            return true;
        default:
            return true;
        }
    }

    bool StdStringReplaceableWithView(const CXXConstructorDecl& ctor) {
        auto nParams = ctor.getNumParams();
        if (nParams >= 1 && nParams <= 3) {
            StringRef paramName = ctor.getParamDecl(0)->getName();
            if (paramName == "__n"  // basic_string(size_type __n, _CharT __c),   // basic_string(size_type __n, _CharT __c, const _Allocator& __a)
                || paramName == "__il"  // basic_string(initializer_list<_CharT> __il), basic_string(initializer_list<_CharT> __il, const _Allocator& __a)
            ) {
                return false;
            }
        }
        return true;
    }

    // Keys are unqualified names - decls don't store full names.
    // Currently unqualified names of StringLikeClasses are unique, so this should be ok.
    const llvm::DenseMap<StringRef, ReplaceableConstructorCheck*> ReplaceableConstructorChecks = {
        {"TBasicString", TStringReplaceableWithView},
        {"basic_string", StdStringReplaceableWithView},
    };

    // ========== Matchers ==========

    AST_MATCHER_P(ImplicitCastExpr, writtenSubExpr, clang::ast_matchers::internal::Matcher<Expr>, InnerMatcher) {  // NOLINT(readability-identifier-naming)
        // ignoringParentCasts doesn't ignore CXXBindTemporaryExpr, ignoringImplicit doesn't ignore CXXFunctionalCastExpr
        return InnerMatcher.matches(*Node.getSubExprAsWritten(), Finder, Builder);
    }

    template<typename T>
    auto cxxRecordDeclWithAnyName(T&& names) {  // NOLINT(readability-identifier-naming)
        return cxxRecordDecl(hasAnyName(std::forward<decltype(names)>(names)));
    }

    // ========== Other helper functions and constants ==========

    auto CreateReplacement(SourceRange range, SourceRange replacementRange, const SourceManager& SM, const LangOptions& LangOpts) {
        return FixItHint::CreateReplacement(
            range,
            Lexer::getSourceText(CharSourceRange::getTokenRange(replacementRange), SM, LangOpts)
        );
    }

    bool IsNonConstReferenceType(QualType ParamType) {
       return ParamType->isReferenceType() && !ParamType.getNonReferenceType().isConstQualified();
    }

    const StringRef UnneededTempStringDiag = "Unneeded temporary string-like object, a string view is sufficient here";
    const StringRef UnneededTempStringWithAllocatorDiag = "Likely unneeded temporary string-like object. "
                                                          "A string view is sufficient here, but the temporary string uses a custom allocator "
                                                          "(may have side effects)";  // Also it's harder to auto-fix
    const StringRef UnneededTempStringCopyMoveDiag = "Unneeded copy/move of a string-like object, a string view is sufficient here";
}  // namespacex

    UnneededTemporaryStringCheck::UnneededTemporaryStringCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
        , DiagnoseNonConvertingConstructors_(Options.get("DiagnoseNonConvertingConstructors", false))
        , DiagnoseMutatingConstructors_(Options.get("DiagnoseMutatingConstructors", false))
        , DiagnoseImplicitCopyMoveConstructors_(Options.get("DiagnoseImplicitCopyMoveConstructors", false))
    {}

    void UnneededTemporaryStringCheck::storeOptions(ClangTidyOptions::OptionMap &Opts) {
        Options.store(Opts, "DiagnoseNonConvertingConstructors", DiagnoseNonConvertingConstructors_);
        Options.store(Opts, "DiagnoseMutatingConstructors", DiagnoseMutatingConstructors_);
        Options.store(Opts, "DiagnoseImplicitCopyMoveConstructors", DiagnoseImplicitCopyMoveConstructors_);
    }

    void UnneededTemporaryStringCheck::registerMatchers(MatchFinder* finder) {
        const auto stringCtor = cxxConstructExpr(
            hasDeclaration(cxxConstructorDecl(
                ofClass(cxxRecordDeclWithAnyName(StringLikeClasses).bind("string_class"))
            ))
        ).bind("string_ctor");

        auto matcher = implicitCastExpr(
            hasImplicitDestinationType(
               hasUnqualifiedDesugaredType(recordType(
                    hasDeclaration(cxxRecordDeclWithAnyName(StringViewLikeClasses).bind("string_view_class"))
                ))
            ),
            writtenSubExpr(anyOf(stringCtor, cxxFunctionalCastExpr(has(ignoringImplicit(stringCtor)))))
        ).bind("implicit_cast");

        finder->addMatcher(matcher, this);
    }

    void UnneededTemporaryStringCheck::CheckImplicitCast(const ast_matchers::MatchFinder::MatchResult& result, const ImplicitCastExpr& cast) {
        const auto& stringConstructor = *result.Nodes.getNodeAs<CXXConstructExpr>("string_ctor");
        const auto& stringClassDecl = *result.Nodes.getNodeAs<CXXRecordDecl>("string_class");
        auto it = ReplaceableConstructorChecks.find(stringClassDecl.getName());
        assert(it != ReplaceableConstructorChecks.end());
        if (!it->second(*stringConstructor.getConstructor())) {  // string cannot be replaced with a TStringBuf / std::string_view
            return;
        }

        auto nArgs = stringConstructor.getNumArgs();
        auto nRealArgs = nArgs;
        while (nRealArgs) {
            if (!isa<CXXDefaultArgExpr>(stringConstructor.getArg(nRealArgs - 1))) {
                break;
            }
            --nRealArgs;
        }
        if (stringClassDecl.getName() == "basic_string" && nRealArgs > 0) {
            // Check custom allocator
            if (stringConstructor.getConstructor()->getParamDecl(nRealArgs - 1)->getName() == "__a") {  // __alloc is used only in c++23 ctors
                diag(stringConstructor.getBeginLoc(), UnneededTempStringWithAllocatorDiag);
                return;
            }
        }
        if (!DiagnoseMutatingConstructors_ && nRealArgs > 0 && IsNonConstReferenceType(stringConstructor.getConstructor()->getParamDecl(0)->getType())) {
            // Don't diagnose move-ctors, because they modify the original string.
            // State of the moved-from string is unspecified, but the user may rely on a specific implementation.
            // Also, replacing TStringBuf(TString(std::move(x))) with TStringBuf(std::move(x)) is not what we really want - move should also be removed.
            return;
        }

        if (nRealArgs != 1) {
            // Not a converting ctor - cannot use implicit conversion, must replace string-like class with target class.
            // No fix-it, because in general it's impossible / very hard to get the correct name of the stringview-like class
            // (e.g. TCaseInsensitiveStringBuf, TBasicStringBuf<char, TCustomTraits>, ...)
            if (DiagnoseNonConvertingConstructors_) {
                diag(stringConstructor.getBeginLoc(), UnneededTempStringDiag);
            }
            return;
        }

        // Converting ctor or copy/move ctor
        const auto* ctorArgDecl = stringConstructor.getConstructor()->getParamDecl(0);
        const auto* argAsWritten = stringConstructor.getArg(0)->IgnoreImplicitAsWritten();

        // Fix-it
        auto removeCtor = [&]() {
            return CreateReplacement(stringConstructor.getSourceRange(), argAsWritten->getSourceRange(), *result.SourceManager, getLangOpts());
        };

        if (const auto* argDeclType = ctorArgDecl->getType().getNonReferenceType()->getAsCXXRecordDecl(); argDeclType == &stringClassDecl) {
            // Copy/move constructor. Can be safely removed only if arg is of the same type. Otherwise there's an implicit cast, let the user fix it.
            if (argAsWritten->getType().getNonReferenceType()->getAsCXXRecordDecl() != argDeclType) {
                if (DiagnoseImplicitCopyMoveConstructors_) {
                    // Copy of an implicitly-casted string reference (e.g. TString(TFsPath)).
                    // The arg may not be directly convertible to string_view-like type (e.g. we'd need to add .GetPath() for TFsPath)
                    diag(stringConstructor.getBeginLoc(), UnneededTempStringDiag);
                    diag(argAsWritten->getBeginLoc(), "Argument is implicitly casted to a string-like type, which is then copied/moved", DiagnosticIDs::Note);
                }
                return;
            }
            diag(stringConstructor.getBeginLoc(), UnneededTempStringCopyMoveDiag) << removeCtor();
        } else {
            // Other converting constructor (const char*, string_view-like, conversion between string types).
            // Assuming string_view-like types can be constructed from any of these.
            diag(stringConstructor.getBeginLoc(), UnneededTempStringDiag) << removeCtor();
        }
    }

    void UnneededTemporaryStringCheck::check(const MatchFinder::MatchResult& result) {
        if (const auto* implicitCast = result.Nodes.getNodeAs<ImplicitCastExpr>("implicit_cast")) {
            return CheckImplicitCast(result, *implicitCast);
        }
        // TODO check templates / overloads (e.g. generated protobuf methods like set_* / add_*)
    }

}  // namespace clang::tidy::arcadia
