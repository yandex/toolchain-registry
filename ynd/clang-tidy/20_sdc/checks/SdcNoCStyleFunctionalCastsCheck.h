#pragma once

#include "SdcCodeSelection.h"
#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            class SdcNoCStyleFunctionalCastsCheck: public ClangTidyCheck {
            public:
                SdcNoCStyleFunctionalCastsCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                bool getDiagnosticLocations(SourceLocation CastLocation,
                                            const SourceManager& SM,
                                            SourceLocation& PrimaryLocation,
                                            SourceLocation& ExpansionLocation);
                AnalysisInstanceTracker AnalysisInstances;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
