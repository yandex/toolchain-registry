#include "SdcNoObjectPointerToIntegralCastCheck.h"

#include "SdcCastUtils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/TypeLoc.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace {

                bool isObjectOrVoidPointer(QualType Type) {
                    return !cast_utils::isFunctionOrMemberPointer(Type) &&
                           (cast_utils::isObjectPointer(Type) || cast_utils::isVoidPointer(Type));
                }

                bool isIntegralDestination(QualType Type) {
                    QualType Canonical = cast_utils::canonicalUnqualified(Type);
                    // Only flag concrete integral destinations.  Dependent types
                    // (template parameters, expressions involving them) are skipped
                    // here — each concrete instantiation is checked separately by
                    // clang-tidy with fully resolved types, so no violations are
                    // missed.  Trying to warn on dependent types causes FPs: e.g.
                    // `static_cast<T*>(void_ptr)` has a dependent destination that
                    // can never be integral (it's always a pointer), but the old
                    // isDependentType() path couldn't see that.
                    return Canonical->isIntegerType();
                }

                std::string normalizedWrittenType(const ExplicitCastExpr* Cast,
                                                  const SourceManager& SM,
                                                  const LangOptions& LangOpts) {
                    TypeSourceInfo* TypeInfo = Cast->getTypeInfoAsWritten();
                    if (!TypeInfo) {
                        return {};
                    }

                    SourceRange TypeRange = TypeInfo->getTypeLoc().getSourceRange();
                    CharSourceRange Range = CharSourceRange::getTokenRange(
                        SM.getSpellingLoc(TypeRange.getBegin()),
                        SM.getSpellingLoc(TypeRange.getEnd()));
                    StringRef Text = Lexer::getSourceText(Range, SM, LangOpts);
                    std::string Normalized;
                    Normalized.reserve(Text.size());
                    for (char C : Text) {
                        if (!std::isspace(static_cast<unsigned char>(C))) {
                            Normalized += C;
                        }
                    }
                    return Normalized;
                }

                bool explicitlyNamesStdIntegerPointerType(const ExplicitCastExpr* Cast,
                                                          const SourceManager& SM,
                                                          const LangOptions& LangOpts) {
                    std::string TypeText = normalizedWrittenType(Cast, SM, LangOpts);
                    return TypeText == "std::uintptr_t" || TypeText == "std::intptr_t" ||
                           TypeText == "::std::uintptr_t" || TypeText == "::std::intptr_t";
                }

            } // namespace

            SdcNoObjectPointerToIntegralCastCheck::SdcNoObjectPointerToIntegralCastCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoObjectPointerToIntegralCastCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    explicitCastExpr(unless(isExpansionInSystemHeader())).bind("cast"),
                    this);
            }

            void SdcNoObjectPointerToIntegralCastCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Cast = Result.Nodes.getNodeAs<ExplicitCastExpr>("cast");
                if (!Cast) {
                    return;
                }

                QualType From = Cast->getSubExpr()->IgnoreParenImpCasts()->getType();
                QualType To = Cast->getTypeAsWritten();
                if (!isObjectOrVoidPointer(From) || !isIntegralDestination(To)) {
                    return;
                }

                if (explicitlyNamesStdIntegerPointerType(Cast, *Result.SourceManager,
                                                         Result.Context->getLangOpts())) {
                    return;
                }

                diag(Cast->getBeginLoc(),
                     "cast from object pointer %0 to integral type %1 is not "
                     "permitted; use an explicit cast to std::uintptr_t or "
                     "std::intptr_t instead")
                    << From << To;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
