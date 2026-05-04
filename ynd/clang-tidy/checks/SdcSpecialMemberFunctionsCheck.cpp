#include "SdcSpecialMemberFunctionsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <map>
#include <string>
#include <vector>

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcSpecialMemberFunctionsCheck::SdcSpecialMemberFunctionsCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcSpecialMemberFunctionsCheck::registerMatchers(MatchFinder* Finder) {
                // Match all class/struct declarations
                Finder->addMatcher(
                    cxxRecordDecl(
                        isDefinition(),
                        unless(isImplicit()),
                        unless(isExpansionInSystemHeader()))
                        .bind("class"),
                    this);
            }

            void SdcSpecialMemberFunctionsCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* ClassDecl = Result.Nodes.getNodeAs<CXXRecordDecl>("class");

                if (!ClassDecl || !ClassDecl->isCompleteDefinition()) {
                    return;
                }

                // Skip lambda classes
                if (ClassDecl->isLambda()) {
                    return;
                }

                // Analyze the class
                SpecialMemberInfo Info = analyzeClass(ClassDecl);

                // Store using canonical declaration for consistent lookup
                const CXXRecordDecl* CanonicalDecl = ClassDecl->getCanonicalDecl();
                AnalyzedClasses[CanonicalDecl] = Info;

                // Run validation checks that don't require cross-class analysis
                checkCategoryValidity(Info);
                checkCustomizedCopyMoveRequiresCustomizedDtor(Info);
                checkCustomizedDestructorIsNonEmpty(Info);
                checkManagerTypeRequirements(Info);
                // Note: File location checking is not needed - ODR already enforces
                // that all definitions must be in a single translation unit

                // Check immediate inheritance requirement: public virtual dtor must be unmovable
                if (Info.DtorDecl && Info.DtorDecl->getAccess() == AS_public &&
                    Info.DtorDecl->isVirtual()) {
                    if (Info.Category != ClassCategory::Unmovable) {
                        diag(Info.ClassDecl->getLocation(),
                             "class with public virtual destructor must be unmovable");
                    }
                }

                // Note: Inheritance checking for "used as public base" will be done in onEndOfTranslationUnit
                // after all classes have been analyzed
            }

            SdcSpecialMemberFunctionsCheck::SpecialMemberInfo
            SdcSpecialMemberFunctionsCheck::analyzeClass(const CXXRecordDecl* ClassDecl) {
                SpecialMemberInfo Info;
                Info.ClassDecl = ClassDecl;

                // Analyze each special member
                Info.Destructor = getDestructorState(ClassDecl, &Info.DtorDecl);
                Info.CopyConstructor = getCopyConstructorState(ClassDecl, &Info.CopyCtorDecl);
                Info.CopyAssignment = getCopyAssignmentState(ClassDecl, &Info.CopyAssignDecl);
                Info.MoveConstructor = getMoveConstructorState(ClassDecl, &Info.MoveCtorDecl);
                Info.MoveAssignment = getMoveAssignmentState(ClassDecl, &Info.MoveAssignDecl);

                // Determine category and manager type
                Info.Category = determineCategory(Info);
                Info.Manager = determineManagerType(Info);

                // Track file locations for out-of-class definitions
                if (Info.DtorDecl && isOutOfClassDefinition(Info.DtorDecl)) {
                    Info.DtorFile = getDefinitionFile(Info.DtorDecl);
                }
                if (Info.CopyCtorDecl && isOutOfClassDefinition(Info.CopyCtorDecl)) {
                    Info.CopyCtorFile = getDefinitionFile(Info.CopyCtorDecl);
                }
                if (Info.CopyAssignDecl && isOutOfClassDefinition(Info.CopyAssignDecl)) {
                    Info.CopyAssignFile = getDefinitionFile(Info.CopyAssignDecl);
                }
                if (Info.MoveCtorDecl && isOutOfClassDefinition(Info.MoveCtorDecl)) {
                    Info.MoveCtorFile = getDefinitionFile(Info.MoveCtorDecl);
                }
                if (Info.MoveAssignDecl && isOutOfClassDefinition(Info.MoveAssignDecl)) {
                    Info.MoveAssignFile = getDefinitionFile(Info.MoveAssignDecl);
                }

                return Info;
            }

            SdcSpecialMemberFunctionsCheck::MemberState
            SdcSpecialMemberFunctionsCheck::getDestructorState(
                const CXXRecordDecl* ClassDecl, const CXXDestructorDecl** OutDecl) {
                const CXXDestructorDecl* Dtor = ClassDecl->getDestructor();

                if (OutDecl) {
                    *OutDecl = Dtor;
                }

                if (!Dtor) {
                    return MemberState::Implicit;
                }

                // If the destructor is implicit (not declared by user), it's implicit
                if (!Dtor->isUserProvided() && !Dtor->isDefaulted() && !Dtor->isDeleted()) {
                    return MemberState::Implicit;
                }

                if (Dtor->isDeleted()) {
                    return MemberState::Deleted;
                }

                if (Dtor->isDefaulted()) {
                    return MemberState::Defaulted;
                }

                // User-provided and not defaulted
                return MemberState::Customized;
            }

            SdcSpecialMemberFunctionsCheck::MemberState
            SdcSpecialMemberFunctionsCheck::getCopyConstructorState(
                const CXXRecordDecl* ClassDecl, const CXXConstructorDecl** OutDecl) {
                const CXXConstructorDecl* CopyCtor = nullptr;

                for (const auto* Ctor : ClassDecl->ctors()) {
                    if (Ctor->isCopyConstructor()) {
                        CopyCtor = Ctor;
                        break;
                    }
                }

                if (OutDecl) {
                    *OutDecl = CopyCtor;
                }

                if (!CopyCtor) {
                    return MemberState::Implicit;
                }

                // Check if it's implicit (compiler-generated)
                if (!CopyCtor->isUserProvided() && !CopyCtor->isDefaulted() && !CopyCtor->isDeleted()) {
                    return MemberState::Implicit;
                }

                if (CopyCtor->isDeleted()) {
                    return MemberState::Deleted;
                }

                if (CopyCtor->isDefaulted()) {
                    return MemberState::Defaulted;
                }

                return MemberState::Customized;
            }

            SdcSpecialMemberFunctionsCheck::MemberState
            SdcSpecialMemberFunctionsCheck::getCopyAssignmentState(
                const CXXRecordDecl* ClassDecl, const CXXMethodDecl** OutDecl) {
                const CXXMethodDecl* CopyAssign = nullptr;

                for (const auto* Method : ClassDecl->methods()) {
                    if (Method->isCopyAssignmentOperator()) {
                        CopyAssign = Method;
                        break;
                    }
                }

                if (OutDecl) {
                    *OutDecl = CopyAssign;
                }

                if (!CopyAssign) {
                    return MemberState::Implicit;
                }

                // Check if it's implicit (compiler-generated)
                if (!CopyAssign->isUserProvided() && !CopyAssign->isDefaulted() && !CopyAssign->isDeleted()) {
                    return MemberState::Implicit;
                }

                if (CopyAssign->isDeleted()) {
                    return MemberState::Deleted;
                }

                if (CopyAssign->isDefaulted()) {
                    return MemberState::Defaulted;
                }

                return MemberState::Customized;
            }

            SdcSpecialMemberFunctionsCheck::MemberState
            SdcSpecialMemberFunctionsCheck::getMoveConstructorState(
                const CXXRecordDecl* ClassDecl, const CXXConstructorDecl** OutDecl) {
                const CXXConstructorDecl* MoveCtor = nullptr;

                for (const auto* Ctor : ClassDecl->ctors()) {
                    if (Ctor->isMoveConstructor()) {
                        MoveCtor = Ctor;
                        break;
                    }
                }

                if (OutDecl) {
                    *OutDecl = MoveCtor;
                }

                if (!MoveCtor) {
                    return MemberState::Implicit;
                }

                // Check if it's implicit (compiler-generated)
                if (!MoveCtor->isUserProvided() && !MoveCtor->isDefaulted() && !MoveCtor->isDeleted()) {
                    return MemberState::Implicit;
                }

                if (MoveCtor->isDeleted()) {
                    return MemberState::Deleted;
                }

                if (MoveCtor->isDefaulted()) {
                    return MemberState::Defaulted;
                }

                return MemberState::Customized;
            }

            SdcSpecialMemberFunctionsCheck::MemberState
            SdcSpecialMemberFunctionsCheck::getMoveAssignmentState(
                const CXXRecordDecl* ClassDecl, const CXXMethodDecl** OutDecl) {
                const CXXMethodDecl* MoveAssign = nullptr;

                for (const auto* Method : ClassDecl->methods()) {
                    if (Method->isMoveAssignmentOperator()) {
                        MoveAssign = Method;
                        break;
                    }
                }

                if (OutDecl) {
                    *OutDecl = MoveAssign;
                }

                if (!MoveAssign) {
                    return MemberState::Implicit;
                }

                // Check if it's implicit (compiler-generated)
                if (!MoveAssign->isUserProvided() && !MoveAssign->isDefaulted() && !MoveAssign->isDeleted()) {
                    return MemberState::Implicit;
                }

                if (MoveAssign->isDeleted()) {
                    return MemberState::Deleted;
                }

                if (MoveAssign->isDefaulted()) {
                    return MemberState::Defaulted;
                }

                return MemberState::Customized;
            }

            bool SdcSpecialMemberFunctionsCheck::isCopyConstructible(
                const SpecialMemberInfo& Info) {
                // Copy-constructible if copy constructor is not deleted
                return Info.CopyConstructor != MemberState::Deleted;
            }

            bool SdcSpecialMemberFunctionsCheck::isMoveConstructible(
                const SpecialMemberInfo& Info) {
                // Move-constructible if move constructor exists and is not deleted
                // OR if copy constructor exists and move is implicit/deleted (falls back to copy)

                // If move is explicitly available (customized or defaulted), use it
                if (Info.MoveConstructor == MemberState::Customized ||
                    Info.MoveConstructor == MemberState::Defaulted) {
                    return true;
                }

                // If move is implicit, check if copy is available as fallback
                // (C++ allows using copy constructor when move is not explicitly deleted)
                if (Info.MoveConstructor == MemberState::Implicit) {
                    return isCopyConstructible(Info);
                }

                // If move is explicitly deleted, the class is not move-constructible
                // even if copy constructor exists
                if (Info.MoveConstructor == MemberState::Deleted) {
                    return false;
                }

                return false;
            }

            bool SdcSpecialMemberFunctionsCheck::isCopyAssignable(
                const SpecialMemberInfo& Info) {
                return Info.CopyAssignment != MemberState::Deleted;
            }

            bool SdcSpecialMemberFunctionsCheck::isMoveAssignable(
                const SpecialMemberInfo& Info) {
                // Move-assignable if move assignment exists and is not deleted
                // OR if copy assignment exists and move is implicit/deleted (falls back to copy)

                // If move is explicitly available (customized or defaulted), use it
                if (Info.MoveAssignment == MemberState::Customized ||
                    Info.MoveAssignment == MemberState::Defaulted) {
                    return true;
                }

                // If move is implicit, check if copy is available as fallback
                // (C++ allows using copy assignment when move is not explicitly deleted)
                if (Info.MoveAssignment == MemberState::Implicit) {
                    return isCopyAssignable(Info);
                }

                // If move is explicitly deleted, the class is not move-assignable
                // even if copy assignment exists
                if (Info.MoveAssignment == MemberState::Deleted) {
                    return false;
                }

                return false;
            }

            SdcSpecialMemberFunctionsCheck::ClassCategory
            SdcSpecialMemberFunctionsCheck::determineCategory(
                const SpecialMemberInfo& Info) {
                bool copyCtor = isCopyConstructible(Info);
                bool moveCtor = isMoveConstructible(Info);
                bool copyAssign = isCopyAssignable(Info);
                bool moveAssign = isMoveAssignable(Info);

                // Unmovable: not copy-constructible, not move-constructible,
                //            not copy-assignable, not move-assignable
                if (!copyCtor && !moveCtor && !copyAssign && !moveAssign) {
                    return ClassCategory::Unmovable;
                }

                // Move-only: move-constructible, but not copy-constructible and not copy-assignable
                // Can be optionally move-assignable
                if (moveCtor && !copyCtor && !copyAssign) {
                    return ClassCategory::MoveOnly;
                }

                // Copy-enabled: copy-constructible and move-constructible
                // Can optionally be copy-assignable and move-assignable
                if (copyCtor && moveCtor) {
                    // Must have consistent assignment operators
                    // Either both assignable or both not assignable
                    // OR copy-assignable only (without move-assignable)
                    if ((copyAssign && moveAssign) || (!copyAssign && !moveAssign)) {
                        return ClassCategory::CopyEnabled;
                    }
                    // Copy-assignable but not move-assignable is also valid
                    if (copyAssign && !moveAssign) {
                        return ClassCategory::CopyEnabled;
                    }
                }

                // Special case: classes with customized destructors that are copy-constructible
                // but not move-constructible are allowed (valid resource management pattern)
                if (Info.Destructor == MemberState::Customized && copyCtor && !moveCtor) {
                    return ClassCategory::CopyEnabled;
                }

                // All other combinations are invalid
                return ClassCategory::Invalid;
            }

            SdcSpecialMemberFunctionsCheck::ManagerType
            SdcSpecialMemberFunctionsCheck::determineManagerType(
                const SpecialMemberInfo& Info) {
                if (Info.Destructor != MemberState::Customized) {
                    return ManagerType::None;
                }

                switch (Info.Category) {
                    case ClassCategory::Unmovable:
                        return ManagerType::Scoped;
                    case ClassCategory::MoveOnly:
                        return ManagerType::Unique;
                    case ClassCategory::CopyEnabled:
                        return ManagerType::General;
                    default:
                        return ManagerType::None;
                }
            }

            bool SdcSpecialMemberFunctionsCheck::isDestructorNonEmpty(
                const CXXDestructorDecl* Dtor) {
                if (!Dtor || !Dtor->hasBody()) {
                    return false;
                }

                const Stmt* Body = Dtor->getBody();
                if (!Body) {
                    return false;
                }

                const auto* CompoundBody = dyn_cast<CompoundStmt>(Body);
                if (!CompoundBody) {
                    // If it's not a compound statement, it's something else (unlikely for dtor)
                    return true;
                }

                // Check if body is empty or contains only null statements
                for (const Stmt* S : CompoundBody->body()) {
                    if (!S) {
                        continue;
                    }

                    // Null statement is just a semicolon
                    if (isa<NullStmt>(S)) {
                        continue;
                    }

                    // Found a non-null statement
                    return true;
                }

                // Body is empty or contains only null statements
                return false;
            }

            bool SdcSpecialMemberFunctionsCheck::isUsedAsPublicBase(
                const CXXRecordDecl* ClassDecl,
                const MatchFinder::MatchResult& Result) {
                // TODO: This requires analyzing derived classes
                // For now, we'll search the AST context
                // This is a simplified implementation

                return false; // Placeholder for Phase 4
            }

            void SdcSpecialMemberFunctionsCheck::checkCategoryValidity(
                const SpecialMemberInfo& Info) {
                if (Info.Category == ClassCategory::Invalid) {
                    diag(Info.ClassDecl->getLocation(),
                         "class has invalid combination of copy/move constructibility and assignability; "
                         "must be Unmovable, Move-only, or Copy-enabled");
                }
            }

            void SdcSpecialMemberFunctionsCheck::checkCustomizedCopyMoveRequiresCustomizedDtor(
                const SpecialMemberInfo& Info) {
                // Check if any copy or move operation is customized
                bool hasCustomizedCopyMove =
                    Info.CopyConstructor == MemberState::Customized ||
                    Info.CopyAssignment == MemberState::Customized ||
                    Info.MoveConstructor == MemberState::Customized ||
                    Info.MoveAssignment == MemberState::Customized;

                if (hasCustomizedCopyMove && Info.Destructor != MemberState::Customized) {
                    diag(Info.ClassDecl->getLocation(),
                         "class has customized copy or move operations but no customized destructor");

                    // Add notes showing which operations are customized
                    if (Info.CopyConstructor == MemberState::Customized && Info.CopyCtorDecl) {
                        diag(Info.CopyCtorDecl->getLocation(), "customized copy constructor here",
                             DiagnosticIDs::Note);
                    }
                    if (Info.CopyAssignment == MemberState::Customized && Info.CopyAssignDecl) {
                        diag(Info.CopyAssignDecl->getLocation(), "customized copy assignment here",
                             DiagnosticIDs::Note);
                    }
                    if (Info.MoveConstructor == MemberState::Customized && Info.MoveCtorDecl) {
                        diag(Info.MoveCtorDecl->getLocation(), "customized move constructor here",
                             DiagnosticIDs::Note);
                    }
                    if (Info.MoveAssignment == MemberState::Customized && Info.MoveAssignDecl) {
                        diag(Info.MoveAssignDecl->getLocation(), "customized move assignment here",
                             DiagnosticIDs::Note);
                    }
                }
            }

            void SdcSpecialMemberFunctionsCheck::checkCustomizedDestructorIsNonEmpty(
                const SpecialMemberInfo& Info) {
                if (Info.Destructor != MemberState::Customized) {
                    return;
                }

                if (!Info.DtorDecl) {
                    return;
                }

                if (!isDestructorNonEmpty(Info.DtorDecl)) {
                    diag(Info.ClassDecl->getLocation(),
                         "class has customized destructor with empty body; "
                         "customized destructor must contain at least one non-null statement");
                    diag(Info.DtorDecl->getLocation(),
                         "destructor defined here",
                         DiagnosticIDs::Note);
                }
            }

            void SdcSpecialMemberFunctionsCheck::checkManagerTypeRequirements(
                const SpecialMemberInfo& Info) {
                switch (Info.Manager) {
                    case ManagerType::None:
                        // No customized destructor, no manager requirements
                        return;

                    case ManagerType::Scoped:
                        // Scoped manager: unmovable with customized destructor
                        // No additional requirements beyond being unmovable
                        return;

                    case ManagerType::Unique:
                        // Unique manager: move-only with customized destructor
                        // Must have customized move constructor
                        if (Info.MoveConstructor != MemberState::Customized) {
                            diag(Info.ClassDecl->getLocation(),
                                 "unique manager (move-only class with customized destructor) "
                                 "must have customized move constructor");
                        }

                        // If move-assignable, must have customized move assignment
                        if (isMoveAssignable(Info) && Info.MoveAssignment != MemberState::Customized) {
                            diag(Info.ClassDecl->getLocation(),
                                 "unique manager that is move-assignable must have "
                                 "customized move assignment operator");
                        }
                        return;

                    case ManagerType::General:
                        // General manager: copy-enabled with customized destructor
                        // Must have customized copy constructor
                        if (Info.CopyConstructor != MemberState::Customized) {
                            diag(Info.ClassDecl->getLocation(),
                                 "general manager (copy-enabled class with customized destructor) "
                                 "must have customized copy constructor");
                        }

                        // Move constructor must be either customized or not declared
                        if (Info.MoveConstructor == MemberState::Defaulted) {
                            diag(Info.ClassDecl->getLocation(),
                                 "general manager move constructor must be either customized or not declared "
                                 "(not defaulted)");
                        }

                        // If copy-assignable, must have customized copy assignment
                        if (isCopyAssignable(Info) && Info.CopyAssignment != MemberState::Customized) {
                            diag(Info.ClassDecl->getLocation(),
                                 "general manager that is copy-assignable must have "
                                 "customized copy assignment operator");
                        }

                        // If copy-assignable, move operations must both be customized or both not declared
                        if (isCopyAssignable(Info)) {
                            bool moveCtorDeclared = Info.MoveConstructor != MemberState::Implicit;
                            bool moveAssignDeclared = Info.MoveAssignment != MemberState::Implicit;

                            if (moveCtorDeclared || moveAssignDeclared) {
                                // At least one is declared, both must be customized
                                if (Info.MoveConstructor != MemberState::Customized ||
                                    Info.MoveAssignment != MemberState::Customized) {
                                    diag(Info.ClassDecl->getLocation(),
                                         "general manager that is copy-assignable must have move operations "
                                         "either both customized or both not declared");
                                }
                            }
                        }
                        return;
                }
            }

            void SdcSpecialMemberFunctionsCheck::checkInheritanceRequirements(
                const SpecialMemberInfo& Info,
                const MatchFinder::MatchResult& Result) {
                // Check if this class is used as a public base
                // This is complex and requires analyzing derived classes
                // For Phase 1, we'll implement a basic check

                if (!Info.DtorDecl) {
                    return;
                }

                // Check if this class has derived classes
                // We need to iterate through all classes and check their bases
                // This is a simplified implementation for Phase 1

                // Check destructor accessibility and virtual-ness
                bool hasPublicDtor = Info.DtorDecl->getAccess() == AS_public;
                bool hasProtectedDtor = Info.DtorDecl->getAccess() == AS_protected;
                bool isVirtualDtor = Info.DtorDecl->isVirtual();

                // If destructor is public and virtual, class must be unmovable
                if (hasPublicDtor && isVirtualDtor) {
                    if (Info.Category != ClassCategory::Unmovable) {
                        diag(Info.ClassDecl->getLocation(),
                             "class with public virtual destructor must be unmovable");
                    }
                }
            }

            void SdcSpecialMemberFunctionsCheck::onEndOfTranslationUnit() {
                // Second pass: identify which classes are used as public bases
                for (auto& Entry : AnalyzedClasses) {
                    const CXXRecordDecl* DerivedClass = Entry.first;

                    // Check all base classes of this derived class
                    for (const auto& Base : DerivedClass->bases()) {
                        // Check if this is a public base
                        if (Base.getAccessSpecifier() != AS_public) {
                            continue;
                        }

                        // Get the base class declaration
                        const Type* BaseType = Base.getType().getTypePtr();
                        if (!BaseType) {
                            continue;
                        }

                        const CXXRecordDecl* BaseClass = BaseType->getAsCXXRecordDecl();
                        if (!BaseClass) {
                            continue;
                        }

                        // Get canonical declaration for consistent lookup
                        BaseClass = BaseClass->getCanonicalDecl();

                        // Mark this base class as being used as a public base
                        auto It = AnalyzedClasses.find(BaseClass);
                        if (It != AnalyzedClasses.end()) {
                            It->second.IsUsedAsPublicBase = true;
                        }
                    }
                }

                // Third pass: validate inheritance requirements for classes used as public bases
                for (const auto& Entry : AnalyzedClasses) {
                    const SpecialMemberInfo& Info = Entry.second;

                    if (!Info.IsUsedAsPublicBase) {
                        continue;
                    }

                    // Exception: Aggregates can be used as public base classes
                    if (isAggregate(Info.ClassDecl)) {
                        continue;
                    }

                    // Check if the class satisfies one of the two requirements:
                    // 1. Unmovable with public virtual destructor
                    // 2. Protected non-virtual destructor

                    bool satisfiesRequirement1 = false;
                    bool satisfiesRequirement2 = false;

                    if (hasPublicVirtualDestructor(Info)) {
                        // Must be unmovable
                        if (Info.Category == ClassCategory::Unmovable) {
                            satisfiesRequirement1 = true;
                        }
                    }

                    if (hasProtectedDestructor(Info)) {
                        satisfiesRequirement2 = true;
                    }

                    if (!satisfiesRequirement1 && !satisfiesRequirement2) {
                        diag(Info.ClassDecl->getLocation(),
                             "class used as public base must have either a public virtual destructor "
                             "(and be unmovable) or a protected destructor");
                    }
                }

                // Clear the map for next translation unit
                AnalyzedClasses.clear();
            }

            bool SdcSpecialMemberFunctionsCheck::isAggregate(
                const CXXRecordDecl* ClassDecl) {
                if (!ClassDecl) {
                    return false;
                }

                // According to rule: "An aggregate, which cannot have a user-declared destructor,
                // may be used as a public base class"
                // So we only exempt aggregates that do NOT have a user-declared destructor

                if (!ClassDecl->isAggregate()) {
                    return false;
                }

                // Check if it has a user-declared destructor
                const CXXDestructorDecl* Dtor = ClassDecl->getDestructor();
                if (Dtor && (Dtor->isUserProvided() || Dtor->isDefaulted() || Dtor->isDeleted())) {
                    // Has a user-declared destructor, so NOT exempt
                    return false;
                }

                return true;
            }

            bool SdcSpecialMemberFunctionsCheck::hasPublicVirtualDestructor(
                const SpecialMemberInfo& Info) {
                if (!Info.DtorDecl) {
                    return false;
                }

                return Info.DtorDecl->getAccess() == AS_public &&
                       Info.DtorDecl->isVirtual();
            }

            bool SdcSpecialMemberFunctionsCheck::hasProtectedDestructor(
                const SpecialMemberInfo& Info) {
                if (!Info.DtorDecl) {
                    return false;
                }

                return Info.DtorDecl->getAccess() == AS_protected;
            }

            bool SdcSpecialMemberFunctionsCheck::isOutOfClassDefinition(
                const FunctionDecl* Decl) {
                if (!Decl) {
                    return false;
                }

                // Check if the function has a body (is defined)
                if (!Decl->hasBody()) {
                    return false;
                }

                // Check if it's defined inline (definition is in the class body)
                // A function is out-of-class if it's not defined inline
                return !Decl->isInlined() || Decl->isOutOfLine();
            }

            std::string SdcSpecialMemberFunctionsCheck::getDefinitionFile(
                const FunctionDecl* Decl) {
                if (!Decl) {
                    return "";
                }

                // Get the definition (body) location
                const FunctionDecl* Definition = Decl->getDefinition();
                if (!Definition) {
                    return "";
                }

                SourceLocation Loc = Definition->getLocation();
                if (Loc.isInvalid()) {
                    return "";
                }

                // Get the file name from the source manager
                const SourceManager& SM = Definition->getASTContext().getSourceManager();

                if (SM.getFilename(Loc).empty()) {
                    return "";
                }

                return SM.getFilename(Loc).str();
            }

            void SdcSpecialMemberFunctionsCheck::checkFileLocations(
                const SpecialMemberInfo& Info) {
                // Collect all out-of-class definition files
                std::vector<std::pair<std::string, std::string>> fileLocations;

                if (!Info.DtorFile.empty()) {
                    fileLocations.push_back({"destructor", Info.DtorFile});
                }
                if (!Info.CopyCtorFile.empty()) {
                    fileLocations.push_back({"copy constructor", Info.CopyCtorFile});
                }
                if (!Info.CopyAssignFile.empty()) {
                    fileLocations.push_back({"copy assignment operator", Info.CopyAssignFile});
                }
                if (!Info.MoveCtorFile.empty()) {
                    fileLocations.push_back({"move constructor", Info.MoveCtorFile});
                }
                if (!Info.MoveAssignFile.empty()) {
                    fileLocations.push_back({"move assignment operator", Info.MoveAssignFile});
                }

                // If there are less than 2 out-of-class definitions, no violation
                if (fileLocations.size() < 2) {
                    return;
                }

                // Check if all files are the same
                std::string firstFile = fileLocations[0].second;
                bool allSame = true;

                for (size_t i = 1; i < fileLocations.size(); ++i) {
                    if (fileLocations[i].second != firstFile) {
                        allSame = false;
                        break;
                    }
                }

                if (!allSame) {
                    // Found a violation - special member definitions in different files
                    diag(Info.ClassDecl->getLocation(),
                         "all out-of-class definitions of special member functions "
                         "must be in the same file");

                    // Add notes showing where each definition is
                    for (const auto& Location : fileLocations) {
                        diag(Info.ClassDecl->getLocation(),
                             "%0 defined in %1",
                             DiagnosticIDs::Note)
                            << Location.first << Location.second;
                    }
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
