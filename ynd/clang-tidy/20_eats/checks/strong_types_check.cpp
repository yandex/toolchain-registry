#include "strong_types_check.h"

#include "declaration_utils.h"

#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringExtras.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

    namespace {

        bool isStdString(QualType type) {
            const auto* record_ptr = type->getAsCXXRecordDecl();
            return record_ptr && record_ptr->isInStdNamespace() && record_ptr->getIdentifier() &&
                   record_ptr->getName() == "basic_string";
        }

        bool isPlainInteger(QualType type) {
            return type->isIntegerType() && !type->isBooleanType() && !type->isAnyCharacterType() &&
                   !type->isEnumeralType();
        }

        // Split snake_case and camelCase: 'kDefaultTimeout' gives 'k', 'Default', 'Timeout'.
        SmallVector<StringRef, 4> splitWords(StringRef name) {
            SmallVector<StringRef, 4> words;
            size_t wordStart = 0;
            for (size_t i = 0; i <= name.size(); ++i) {
                const bool atEnd = i == name.size();
                const bool atUnderscore = !atEnd && name[i] == '_';
                const bool atCamelHump = !atEnd && i > 0 && llvm::isUpper(name[i]) &&
                                         (llvm::isLower(name[i - 1]) || llvm::isDigit(name[i - 1]));
                if (atEnd || atUnderscore || atCamelHump) {
                    if (i > wordStart) {
                        words.push_back(name.slice(wordStart, i));
                    }
                    wordStart = atUnderscore ? i + 1 : i;
                }
            }
            return words;
        }

        std::optional<StringRef> strongTypeAdvice(StringRef name, QualType type) {
            const SmallVector<StringRef, 4> words = splitWords(name);
            if (words.empty()) {
                return std::nullopt;
            }
            const auto hasWord = [&words](std::initializer_list<StringRef> candidates) {
                return llvm::any_of(words, [&candidates](StringRef word) {
                    return llvm::any_of(candidates, [word](StringRef candidate) {
                        return word.equals_insensitive(candidate);
                    });
                });
            };

            type = type.getNonReferenceType().getCanonicalType();
            if (words.back().equals_insensitive("id") && (isPlainInteger(type) || isStdString(type))) {
                return StringRef("an identifier needs a strong typedef");
            }
            if (hasWord({"timeout", "delay"}) && (isPlainInteger(type) || type->isRealFloatingType())) {
                return StringRef("a duration needs a std::chrono type");
            }
            if (hasWord({"price", "amount"}) && type->isRealFloatingType()) {
                return StringRef("money needs a decimal type");
            }
            return std::nullopt;
        }

    } // namespace

    void StrongTypesCheck::registerMatchers(MatchFinder* finder_ptr) {
        finder_ptr->addMatcher(
            declaratorDecl(
                unless(isInstantiated()),
                anyOf(varDecl(unless(isImplicit())), fieldDecl(unless(hasParent(cxxRecordDecl(isLambda()))))))
                .bind("decl"),
            this);
    }

    void StrongTypesCheck::check(const MatchFinder::MatchResult& result) {
        const auto* decl_ptr = result.Nodes.getNodeAs<DeclaratorDecl>("decl");
        if (!decl_ptr->getIdentifier() || decl_ptr->getLocation().isMacroID() || decl_ptr->getType()->isDependentType()) {
            return;
        }
        if (const auto* parameter_ptr = dyn_cast<ParmVarDecl>(decl_ptr);
            parameter_ptr && !isOwnParameterName(*parameter_ptr)) {
            return;
        }
        const std::optional<StringRef> advice_opt = strongTypeAdvice(decl_ptr->getName(), decl_ptr->getType());
        if (!advice_opt.has_value()) {
            return;
        }
        diag(decl_ptr->getLocation(), "'%0' has a primitive type %1; %2")
            << decl_ptr->getName() << decl_ptr->getType() << advice_opt.value();
    }

} // namespace clang::tidy::eats
