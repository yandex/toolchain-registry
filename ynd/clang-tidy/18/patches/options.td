//===----------------------------------------------------------------------===//
// Arcadia checkers.
//===----------------------------------------------------------------------===//

def Arcadia : Package<"arcadia">;

let ParentPackage = Arcadia in {

def PossibleNullptr : Checker<"PossibleNullptr">,
  HelpText<"Check for any non-checked ptr dereference.">,
  CheckerOptions<[
    CmdLineOption<String,
                  "IncludeSrcRe",
                  "Regexpr for include sources",
                  ".*",
                  Released>,
    CmdLineOption<String,
                  "ExcludeSrcRe",
                  "Regexpr for exclude sources",
                  "",
                  Released>
  ]>,
  Documentation<HasDocumentation>;

} // end arcadia
