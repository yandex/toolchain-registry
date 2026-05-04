#pragma once

#include "bridge_header.h"
#include "clang/AST/DeclCXX.h"
#include <map>
#include <string>

namespace clang {
    namespace tidy {
        namespace sdc {

            // Special member functions shall be provided appropriately
            //
            // This rule enforces proper definition and usage of special member functions:
            // - Classes must belong to one of three categories: Unmovable, Move-only, or Copy-enabled
            // - Customized copy/move operations require a customized destructor
            // - Customized destructors must have non-empty bodies
            // - Proper inheritance patterns for base classes
            // - All out-of-class special member definitions must be in the same file
            class SdcSpecialMemberFunctionsCheck: public ClangTidyCheck {
            public:
                SdcSpecialMemberFunctionsCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                // Enum for special member function states
                enum class MemberState {
                    Implicit,  // Compiler-generated, not user-declared
                    Defaulted, // Explicitly = default
                    Deleted,   // Explicitly = delete
                    Customized // User-provided with a body (not defaulted)
                };

                // Enum for class categories
                enum class ClassCategory {
                    Unmovable,   // Not copyable, not movable
                    MoveOnly,    // Movable but not copyable
                    CopyEnabled, // Both copyable and movable
                    Invalid      // Invalid combination
                };

                // Enum for manager types (when class has customized destructor)
                enum class ManagerType {
                    None,   // No customized destructor
                    Scoped, // Unmovable + customized destructor
                    Unique, // Move-only + customized destructor
                    General // Copy-enabled + customized destructor
                };

                // Structure to hold information about a class's special members
                struct SpecialMemberInfo {
                    const CXXRecordDecl* ClassDecl = nullptr;

                    // States of each special member
                    MemberState Destructor = MemberState::Implicit;
                    MemberState CopyConstructor = MemberState::Implicit;
                    MemberState CopyAssignment = MemberState::Implicit;
                    MemberState MoveConstructor = MemberState::Implicit;
                    MemberState MoveAssignment = MemberState::Implicit;

                    // Pointers to actual declarations (if they exist)
                    const CXXDestructorDecl* DtorDecl = nullptr;
                    const CXXConstructorDecl* CopyCtorDecl = nullptr;
                    const CXXMethodDecl* CopyAssignDecl = nullptr;
                    const CXXConstructorDecl* MoveCtorDecl = nullptr;
                    const CXXMethodDecl* MoveAssignDecl = nullptr;

                    // Computed properties
                    ClassCategory Category = ClassCategory::Invalid;
                    ManagerType Manager = ManagerType::None;
                    bool IsUsedAsPublicBase = false;

                    // File locations for out-of-class definitions (empty if inline/not defined)
                    std::string DtorFile;
                    std::string CopyCtorFile;
                    std::string CopyAssignFile;
                    std::string MoveCtorFile;
                    std::string MoveAssignFile;
                };

                // Analysis methods
                SpecialMemberInfo analyzeClass(const CXXRecordDecl* ClassDecl);

                MemberState getDestructorState(const CXXRecordDecl* ClassDecl,
                                               const CXXDestructorDecl** OutDecl);
                MemberState getCopyConstructorState(const CXXRecordDecl* ClassDecl,
                                                    const CXXConstructorDecl** OutDecl);
                MemberState getCopyAssignmentState(const CXXRecordDecl* ClassDecl,
                                                   const CXXMethodDecl** OutDecl);
                MemberState getMoveConstructorState(const CXXRecordDecl* ClassDecl,
                                                    const CXXConstructorDecl** OutDecl);
                MemberState getMoveAssignmentState(const CXXRecordDecl* ClassDecl,
                                                   const CXXMethodDecl** OutDecl);

                ClassCategory determineCategory(const SpecialMemberInfo& Info);
                ManagerType determineManagerType(const SpecialMemberInfo& Info);

                bool isCopyConstructible(const SpecialMemberInfo& Info);
                bool isMoveConstructible(const SpecialMemberInfo& Info);
                bool isCopyAssignable(const SpecialMemberInfo& Info);
                bool isMoveAssignable(const SpecialMemberInfo& Info);

                bool isDestructorNonEmpty(const CXXDestructorDecl* Dtor);
                bool isUsedAsPublicBase(const CXXRecordDecl* ClassDecl,
                                        const ast_matchers::MatchFinder::MatchResult& Result);

                // Validation methods
                void checkCategoryValidity(const SpecialMemberInfo& Info);
                void checkCustomizedCopyMoveRequiresCustomizedDtor(const SpecialMemberInfo& Info);
                void checkCustomizedDestructorIsNonEmpty(const SpecialMemberInfo& Info);
                void checkManagerTypeRequirements(const SpecialMemberInfo& Info);
                void checkInheritanceRequirements(const SpecialMemberInfo& Info,
                                                  const ast_matchers::MatchFinder::MatchResult& Result);
                void checkFileLocations(const SpecialMemberInfo& Info);

                // Helper methods for inheritance checking
                bool isAggregate(const CXXRecordDecl* ClassDecl);
                bool hasPublicVirtualDestructor(const SpecialMemberInfo& Info);
                bool hasProtectedDestructor(const SpecialMemberInfo& Info);

                // Helper methods for file location checking
                std::string getDefinitionFile(const FunctionDecl* Decl);
                bool isOutOfClassDefinition(const FunctionDecl* Decl);

                // Storage for all analyzed classes (for cross-class analysis)
                std::map<const CXXRecordDecl*, SpecialMemberInfo> AnalyzedClasses;

                // Callback after all AST traversal is done
                void onEndOfTranslationUnit() override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
