#include "SdcPreprocessorExpressions.h"
#include "SdcCodeSelection.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/LiteralSupport.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include <set>
#include <vector>

namespace clang::tidy::sdc {
namespace {
struct WrittenToken { std::string Text; SourceLocation Loc; tok::TokenKind Kind; };
using Tokens = std::vector<WrittenToken>;
WrittenToken written(const Token &T, Preprocessor &PP) {
    return {Lexer::getSpelling(T, PP.getSourceManager(), PP.getLangOpts()), T.getLocation(), T.getKind()};
}
Tokens expand(Tokens Input, Preprocessor &PP, std::set<std::string> Disabled = {}) {
    Tokens Output;
    for (size_t I = 0; I < Input.size(); ++I) {
        auto T = Input[I];
        if (T.Text == "defined") {
            size_t J = I+1;
            bool Paren = J < Input.size() && Input[J].Text == "(";
            if (Paren) ++J;
            if (J >= Input.size()) return {};
            bool Defined = PP.getMacroInfo(PP.getIdentifierInfo(Input[J].Text)) != nullptr;
            I = J + (Paren && J+1 < Input.size() && Input[J+1].Text == ")" ? 1 : 0);
            Output.push_back({Defined ? "1" : "0", T.Loc, tok::numeric_constant});
            continue;
        }
        if (T.Text == "__LINE__") {
            Output.push_back({std::to_string(PP.getSourceManager().getPresumedLineNumber(T.Loc)), T.Loc, tok::numeric_constant});
            continue;
        }
        const auto *MI = (T.Kind == tok::identifier || T.Kind == tok::raw_identifier) ?
            PP.getMacroInfo(PP.getIdentifierInfo(T.Text)) : nullptr;
        if (!MI || MI->isBuiltinMacro() || Disabled.count(T.Text)) { Output.push_back(T); continue; }
        std::vector<Tokens> Args;
        if (MI->isFunctionLike()) {
            if (I+1 >= Input.size() || Input[I+1].Text != "(") { Output.push_back(T); continue; }
            size_t J = I+2; unsigned Depth = 0; Args.emplace_back();
            for (; J < Input.size(); ++J) {
                if (Input[J].Text == ")" && !Depth) break;
                if (Input[J].Text == "(" ) ++Depth;
                else if (Input[J].Text == ")") --Depth;
                if (Input[J].Text == "," && !Depth && (!MI->isVariadic() || Args.size() < MI->getNumParams()))
                    Args.emplace_back();
                else Args.back().push_back(Input[J]);
            }
            if (J == Input.size()) return {};
            I = J;
        }
        Tokens Body;
        for (const auto &BT : MI->tokens()) Body.push_back(written(BT, PP));
        Tokens Substituted;
        for (size_t J = 0; J < Body.size(); ++J) {
            auto B = Body[J];
            int Param = MI->isFunctionLike() ? MI->getParameterNum(PP.getIdentifierInfo(B.Text)) : -1;
            if (Param >= 0 && size_t(Param) < Args.size()) {
                bool Paste = (J && Body[J-1].Text == "##") || (J+1 < Body.size() && Body[J+1].Text == "##");
                Tokens A = Paste ? Args[Param] : expand(Args[Param], PP, Disabled);
                Substituted.insert(Substituted.end(), A.begin(), A.end());
            } else Substituted.push_back(B);
        }
        // Valid numeric/identifier token pastes are enough for pp-expressions;
        // stringification produces a non-integral operand rejected by Clang.
        for (size_t J = 1; J+1 < Substituted.size();) {
            if (Substituted[J].Text != "##") { ++J; continue; }
            Substituted[J-1].Text += Substituted[J+1].Text;
            Substituted[J-1].Kind = std::isdigit(static_cast<unsigned char>(Substituted[J-1].Text[0])) ? tok::numeric_constant : tok::identifier;
            Substituted.erase(Substituted.begin()+J, Substituted.begin()+J+2);
        }
        auto NestedDisabled = Disabled; NestedDisabled.insert(T.Text);
        auto Replacement = expand(Substituted, PP, std::move(NestedDisabled));
        Output.insert(Output.end(), Replacement.begin(), Replacement.end());
    }
    return Output;
}
int rank(StringRef Op) {
    if (Op == "*" || Op == "/" || Op == "%") return 13;
    if (Op == "+" || Op == "-") return 12;
    if (Op == "<<" || Op == ">>") return 11;
    if (Op == "<" || Op == ">" || Op == "<=" || Op == ">=") return 10;
    if (Op == "==" || Op == "!=" || Op == "not_eq") return 9;
    if (Op == "&" || Op == "bitand") return 8;
    if (Op == "^" || Op == "xor") return 7;
    if (Op == "|" || Op == "bitor") return 6;
    if (Op == "&&" || Op == "and") return 5;
    if (Op == "||" || Op == "or") return 4;
    if (Op == "?") return 3;
    if (Op == ",") return 0;
    return -1;
}
struct Expression {
    WrittenToken Token;
    int Rank = 14;
    bool Parenthesized = false;
    llvm::APSInt Value;
    bool Known = true;
    std::unique_ptr<Expression> Left, Right, Third;
    explicit Expression(WrittenToken Token, unsigned Width) : Token(std::move(Token)), Value(Width, false) {}
};
class Parser {
public:
    Parser(Tokens Input, Preprocessor &PP) : Input(std::move(Input)), PP(PP), Width(PP.getTargetInfo().getIntMaxTWidth()) {}
    std::unique_ptr<Expression> parse(int Min = 0) {
        auto Left = primary();
        while (Left && Index < Input.size()) {
            int Rank = rank(Input[Index].Text);
            if (Rank < Min) break;
            auto Root = std::make_unique<Expression>(Input[Index++], Width); Root->Rank = Rank;
            Root->Left = std::move(Left);
            if (Rank == 3) {
                Root->Right = parse();
                if (Index == Input.size() || Input[Index++].Text != ":") return nullptr;
                Root->Third = parse(3);
                if (!Root->Third) return nullptr;
            } else Root->Right = parse(Rank+1);
            if (!Root->Right) return nullptr;
            Left = std::move(Root);
        }
        return Left;
    }
    bool complete() const { return Index == Input.size(); }
private:
    std::unique_ptr<Expression> primary() {
        if (Index >= Input.size()) return nullptr;
        auto T = Input[Index++];
        if (T.Text == "(") {
            auto E = parse();
            if (!E || Index == Input.size() || Input[Index++].Text != ")") return nullptr;
            E->Parenthesized = true; return E;
        }
        auto E = std::make_unique<Expression>(T, Width);
        if (T.Text == "+" || T.Text == "-" || T.Text == "!" || T.Text == "~" || T.Text == "not" || T.Text == "compl") {
            E->Left = primary(); if (!E->Left) return nullptr;
        } else if (T.Kind == tok::numeric_constant) {
            NumericLiteralParser N(T.Text, T.Loc, PP.getSourceManager(), PP.getLangOpts(), PP.getTargetInfo(), PP.getDiagnostics());
            if (N.hadError || !N.isIntegerLiteral()) return nullptr;
            llvm::APInt V(Width, 0);
            if (N.GetIntegerValue(V)) return nullptr;
            E->Value = llvm::APSInt(V, N.isUnsigned || V.isNegative());
        } else if ((T.Kind == tok::char_constant || T.Kind == tok::wide_char_constant || T.Kind == tok::utf8_char_constant || T.Kind == tok::utf16_char_constant || T.Kind == tok::utf32_char_constant)) {
            CharLiteralParser N(T.Text.data(), T.Text.data()+T.Text.size(), T.Loc, PP, T.Kind);
            if (N.hadError()) return nullptr;
            E->Value = llvm::APSInt(llvm::APInt(Width, N.getValue()), false);
        } else if (T.Text == "true") E->Value = 1;
        else if (Index < Input.size() && Input[Index].Text == "(") {
            // Frontend feature-test builtins are opaque values. We still
            // analyze independent arithmetic in the rest of the expression.
            unsigned Depth = 0;
            do {
                if (Input[Index].Text == "(") ++Depth;
                if (Input[Index].Text == ")") --Depth;
                ++Index;
            } while (Depth && Index < Input.size());
            if (Depth) return nullptr;
            E->Known = false;
        }
        return E;
    }
    Tokens Input;
    Preprocessor &PP;
    unsigned Width;
    size_t Index = 0;
};
class ExpressionCallbacks final : public PPCallbacks {
public:
    ExpressionCallbacks(ClangTidyCheck &Check, Preprocessor &PP, bool Parentheses)
        : Check(Check), PP(PP), Parentheses(Parentheses) {}
    void If(SourceLocation, SourceRange R, ConditionValueKind V) override { inspect(R,V); }
    void Elif(SourceLocation, SourceRange R, ConditionValueKind V, SourceLocation) override { inspect(R,V); }
private:
    void warn(Expression &E, StringRef Message) {
        if (isWrittenInAnalyzedSource(E.Token.Loc, PP.getSourceManager())) Check.diag(E.Token.Loc, Message);
    }
    void evaluate(Expression &E, bool Evaluated) {
        if (!E.Left) return;
        evaluate(*E.Left, Evaluated);
        auto &A = *E.Left;
        if (!E.Right) {
            E.Value = A.Value; E.Known = A.Known;
            if (E.Token.Text == "-") {
                if (!Parentheses && Evaluated && A.Known && A.Value.isUnsigned() && A.Value != 0)
                    warn(E, "unsigned arithmetic with constant operands should not wrap");
                E.Value = -A.Value;
            } else if (E.Token.Text == "~" || E.Token.Text == "compl") E.Value = ~A.Value;
            else if (E.Token.Text == "!" || E.Token.Text == "not") { E.Value = A.Value == 0; E.Value.setIsUnsigned(false); }
            return;
        }
        if (Parentheses && E.Rank >= 3) {
            auto Needs = [&](const Expression &Child) { return !Child.Parenthesized && Child.Rank != 14 && Child.Rank > E.Rank; };
            if (Needs(A) || Needs(*E.Right) || (E.Third && Needs(*E.Third)))
                warn(E, "parenthesize operands to make preprocessing expression grouping explicit");
        }
        bool RightEvaluated = Evaluated;
        if (A.Known && ((E.Rank == 5 && A.Value == 0) || (E.Rank == 4 && A.Value != 0))) RightEvaluated = false;
        if (E.Third) RightEvaluated = Evaluated && (!A.Known || A.Value != 0);
        evaluate(*E.Right, RightEvaluated);
        auto &B = *E.Right;
        if (E.Third) {
            evaluate(*E.Third, Evaluated && (!A.Known || A.Value == 0));
            E.Known = A.Known && (A.Value != 0 ? B.Known : E.Third->Known);
            E.Value = A.Value != 0 ? B.Value : E.Third->Value;
            E.Value.setIsUnsigned(B.Value.isUnsigned() || E.Third->Value.isUnsigned());
            return;
        }
        E.Known = A.Known && B.Known;
        if (!E.Known) return;
        llvm::APSInt AV = A.Value, BV = B.Value;
        bool Unsigned = AV.isUnsigned() || BV.isUnsigned();
        AV.setIsUnsigned(Unsigned); BV.setIsUnsigned(Unsigned);
        E.Value.setIsUnsigned(Unsigned);
        bool Wrap = false;
        auto Op = StringRef(E.Token.Text);
        if (Op == "+") E.Value = llvm::APSInt(AV.uadd_ov(BV, Wrap), Unsigned);
        else if (Op == "-") E.Value = llvm::APSInt(AV.usub_ov(BV, Wrap), Unsigned);
        else if (Op == "*") E.Value = llvm::APSInt(AV.umul_ov(BV, Wrap), Unsigned);
        else if (Op == "/" || Op == "%") {
            if (BV == 0) { E.Known = false; return; }
            E.Value = Op == "/" ? AV / BV : AV % BV;
        } else if (Op == "<<" || Op == ">>") {
            auto Shift = BV.getLimitedValue();
            if (Shift >= AV.getBitWidth()) { E.Known = false; return; }
            if (Op == "<<" && A.Value.isUnsigned())
                Wrap = AV.getActiveBits() + Shift > AV.getBitWidth();
            E.Value = Op == "<<" ? AV << Shift : AV >> Shift;
            E.Value.setIsUnsigned(A.Value.isUnsigned());
        } else if (E.Rank == 8) E.Value = AV & BV;
        else if (E.Rank == 7) E.Value = AV ^ BV;
        else if (E.Rank == 6) E.Value = AV | BV;
        else if (E.Rank == 0) E.Value = B.Value;
        else {
            bool V = false;
            if (Op == "<") V = AV < BV;
            else if (Op == ">") V = AV > BV;
            else if (Op == "<=") V = AV <= BV;
            else if (Op == ">=") V = AV >= BV;
            else if (Op == "==") V = AV == BV;
            else if (Op == "!=" || Op == "not_eq") V = AV != BV;
            else if (E.Rank == 5) V = AV != 0 && BV != 0;
            else if (E.Rank == 4) V = AV != 0 || BV != 0;
            E.Value = V; E.Value.setIsUnsigned(false);
        }
        if (!Parentheses && Evaluated && Unsigned && Wrap)
            warn(E, "unsigned arithmetic with constant operands should not wrap");
    }
    void inspect(SourceRange R, ConditionValueKind V) {
        if (V == CVK_NotEvaluated) return;
        auto &SM = PP.getSourceManager();
        auto Begin = SM.getFileLoc(SM.getExpansionRange(R.getBegin()).getBegin());
        auto End = SM.getFileLoc(SM.getExpansionRange(R.getEnd()).getEnd());
        if (Begin.isInvalid() || End.isInvalid() || SM.getFileID(Begin) != SM.getFileID(End)) return;
        bool Invalid = false;
        auto Text = SM.getBufferData(SM.getFileID(Begin), &Invalid);
        if (Invalid) return;
        Lexer Lex(SM.getLocForStartOfFile(SM.getFileID(Begin)), PP.getLangOpts(), Text.begin(),
                  Text.begin()+SM.getFileOffset(Begin), Text.end());
        Tokens Input; Token T;
        while (!Lex.LexFromRawLexer(T)) {
            if (SM.isBeforeInTranslationUnit(End, T.getLocation())) break;
            Input.push_back(written(T, PP));
        }
        Parser P(expand(std::move(Input), PP), PP);
        auto Root = P.parse();
        if (Root && P.complete()) evaluate(*Root, true);
    }
    ClangTidyCheck &Check;
    Preprocessor &PP;
    bool Parentheses;
};
} // namespace
void registerExpressionPreprocessing(ClangTidyCheck &Check, Preprocessor &PP, bool Parentheses) {
    PP.addPPCallbacks(std::make_unique<ExpressionCallbacks>(Check, PP, Parentheses));
}
} // namespace clang::tidy::sdc
