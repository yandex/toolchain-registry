#include "SdcAssertConstantCheck.h"

#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcAssertConstantCheck::SdcAssertConstantCheck(StringRef Name,
                                                ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

// ─── PP-level helpers ──────────────────────────────────────────────────────

namespace {

// Returns true for the two allowed exception forms:
//   assert( false )
//   assert( false && "any string literal" )
// Works on pre-expanded argument tokens.
static bool isExceptionTokens(const Token* T) {
    // Skip leading parens so assert((false)) is also exempt.
    while (T->is(tok::l_paren)) ++T;
    if (!T->is(tok::kw_false)) return false;
    ++T;
    while (T->is(tok::r_paren)) ++T;
    if (T->is(tok::eof)) return true;
    if (!T->is(tok::ampamp)) return false;
    ++T;
    while (T->is(tok::l_paren)) ++T;
    if (!T->is(tok::string_literal)) return false;
    ++T;
    while (T->is(tok::r_paren)) ++T;
    return T->is(tok::eof);
}

// Returns true when all tokens in the sequence are syntactically constant:
// literals, constant keywords, arithmetic/comparison operators, parentheses.
// Any tok::identifier causes the sequence to be treated as potentially
// runtime (it could be a constexpr name, which is handled by the AST pass).
static bool isConstantTokenSeq(const Token* T) {
    for (; !T->is(tok::eof); ++T) {
        switch (T->getKind()) {
        case tok::kw_true:    case tok::kw_false:  case tok::kw_nullptr:
        case tok::kw_sizeof:  case tok::kw_alignof: case tok::kw_noexcept:
        case tok::kw_void:    case tok::kw_bool:
        case tok::kw_char:    case tok::kw_short:   case tok::kw_int:
        case tok::kw_long:    case tok::kw_float:   case tok::kw_double:
        case tok::kw_unsigned: case tok::kw_signed:
        case tok::numeric_constant:
        case tok::char_constant:    case tok::wide_char_constant:
        case tok::utf8_char_constant: case tok::utf16_char_constant:
        case tok::utf32_char_constant:
        case tok::exclaim:   case tok::tilde:
        case tok::plus:      case tok::minus:
        case tok::star:      case tok::slash:      case tok::percent:
        case tok::amp:       case tok::pipe:       case tok::caret:
        case tok::lessless:  case tok::greatergreater:
        case tok::equalequal:    case tok::exclaimequal:
        case tok::less:          case tok::greater:
        case tok::lessequal:     case tok::greaterequal:
        case tok::ampamp:        case tok::pipepipe:
        case tok::question:  case tok::colon:
        case tok::l_paren:   case tok::r_paren:
            break;
        default:
            return false;
        }
    }
    return true;
}

// ─── AST-level helpers ─────────────────────────────────────────────────────

// Returns true for assert(false) and assert(false && "…") in AST form.
static bool isExceptionAst(const Expr* E) {
    E = E->IgnoreParenImpCasts();
    if (const auto* BL = dyn_cast<CXXBoolLiteralExpr>(E))
        return !BL->getValue();
    if (const auto* BO = dyn_cast<BinaryOperator>(E)) {
        if (BO->getOpcode() != BO_LAnd) return false;
        const auto* LHS =
            dyn_cast<CXXBoolLiteralExpr>(BO->getLHS()->IgnoreParenImpCasts());
        if (!LHS || LHS->getValue()) return false;
        return isa<StringLiteral>(BO->getRHS()->IgnoreParenImpCasts());
    }
    return false;
}

// ─── PP callback ───────────────────────────────────────────────────────────

class AssertCallbacks : public PPCallbacks {
    SdcAssertConstantCheck& Check;
    const SourceManager& SM;
    Preprocessor& PP;

public:
    AssertCallbacks(SdcAssertConstantCheck& C, const SourceManager& SM,
                    Preprocessor& PP)
        : Check(C), SM(SM), PP(PP) {}

    void MacroExpands(const Token& MacroNameTok, const MacroDefinition&,
                      SourceRange, const MacroArgs* Args) override {
        if (!MacroNameTok.getIdentifierInfo() ||
            MacroNameTok.getIdentifierInfo()->getName() != "assert")
            return;
        if (!Args || Args->getNumMacroArguments() == 0) return;
        if (SM.isInSystemHeader(MacroNameTok.getLocation())) return;

        // Pre-expand the argument so that object-like macros expanding to
        // literals (e.g. #define ZERO 0) are resolved before token inspection.
        const std::vector<Token>& PreExp =
            const_cast<MacroArgs*>(Args)->getPreExpArgument(0, PP);
        if (PreExp.empty() || PreExp[0].is(tok::eof)) return;

        const Token* Toks = PreExp.data();

        // Apply the exception before any other check.
        if (isExceptionTokens(Toks)) return;

        if (isConstantTokenSeq(Toks)) {
            // Constant detected at the token level — warn immediately and
            // mark the location so the AST pass skips it.
            Check.warnConstantToken(MacroNameTok.getLocation(),
                                    Args->getUnexpArgument(0)->getLocation());
            return;
        }

        // Argument contains identifiers — may be constexpr names.
        // Record the unexpanded arg range so the AST pass can evaluate them.
        const Token* UnexpToks = Args->getUnexpArgument(0);
        SourceLocation ArgBegin = UnexpToks->getLocation();
        SourceLocation ArgEnd;
        for (const Token* T = UnexpToks; !T->is(tok::eof); ++T)
            ArgEnd = T->getLocation();

        Check.recordAssert(MacroNameTok.getLocation(), ArgBegin, ArgEnd);
    }
};

} // namespace

// ─── Check registration ────────────────────────────────────────────────────

void SdcAssertConstantCheck::recordAssert(SourceLocation AssertLoc,
                                           SourceLocation ArgBegin,
                                           SourceLocation ArgEnd) {
    AssertArgs.try_emplace(ArgBegin, AssertLoc, ArgEnd);
}

void SdcAssertConstantCheck::warnConstantToken(SourceLocation AssertLoc,
                                                SourceLocation ArgBegin) {
    Diagnosed.insert(ArgBegin);
    diag(AssertLoc, "assert shall not be used with a constant expression");
}

void SdcAssertConstantCheck::registerPPCallbacks(const SourceManager& SM,
                                                   Preprocessor* PP,
                                                   Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(std::make_unique<AssertCallbacks>(*this, SM, *PP));
}

void SdcAssertConstantCheck::registerMatchers(MatchFinder* Finder) {
    // Broad match — filtered tightly in check() by the recorded arg locations.
    Finder->addMatcher(
        expr(unless(isExpansionInSystemHeader())).bind("e"),
        this
    );
}

void SdcAssertConstantCheck::check(const MatchFinder::MatchResult& Result) {
    if (AssertArgs.empty()) return;

    const auto* E = Result.Nodes.getNodeAs<Expr>("e");
    const SourceManager& SM = *Result.SourceManager;

    // Resolve the spelling location so that non-macro expressions match the
    // file locations recorded by the PP callback.
    SourceLocation Begin = SM.getSpellingLoc(E->getBeginLoc());

    auto It = AssertArgs.find(Begin);
    if (It == AssertArgs.end()) return;

    SourceLocation AssertLoc = It->second.first;
    SourceLocation ArgEnd    = It->second.second;

    // Reject sub-expressions whose source range is smaller than the full arg.
    SourceLocation End = SM.getSpellingLoc(E->getEndLoc());
    if (End != ArgEnd) return;

    // Only process each assert call site once.
    if (!Diagnosed.insert(Begin).second) return;

    if (isExceptionAst(E)) return;
    if (E->isValueDependent() || E->isTypeDependent()) return;

    if (E->isCXX11ConstantExpr(*Result.Context))
        diag(AssertLoc, "assert shall not be used with a constant expression");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
