; PowerShell. A later pattern wins: the keywords and operators come last.
(comment) @comment
(string_literal) @string
[(integer_literal) (real_literal)] @number
(variable) @variable
(type_spec) @type
(command command_name: (command_name) @function)
(function_statement (function_name) @function)
(invokation_expression (member_name) @function)
(member_access (member_name) @key)
(command_invokation_operator) @operator

[
  "param" "dynamicparam" "begin" "process" "end" "if" "elseif" "else"
  "switch" "foreach" "for" "while" "do" "until" "function" "filter"
  "workflow" "break" "continue" "throw" "return" "exit" "trap" "try"
  "catch" "finally" "data" "inlinescript" "parallel" "sequence"
] @keyword

[
  "-as" "-ccontains" "-ceq" "-cge" "-cgt" "-cle" "-clike" "-clt" "-cmatch"
  "-cne" "-cnotcontains" "-cnotlike" "-cnotmatch" "-contains" "-creplace"
  "-csplit" "-eq" "-ge" "-gt" "-icontains" "-ieq" "-ige" "-igt" "-ile"
  "-ilike" "-ilt" "-imatch" "-in" "-ine" "-inotcontains" "-inotlike"
  "-inotmatch" "-ireplace" "-is" "-isnot" "-isplit" "-join" "-le" "-like"
  "-lt" "-match" "-ne" "-notcontains" "-notin" "-notlike" "-notmatch"
  "-replace" "-shl" "-shr" "-split" "-and" "-or" "-xor" "-band" "-bor"
  "-bxor" "-not" "+" "-" "/" "%" "*" ".."
] @operator

";" @punctuation
