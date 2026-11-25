; Bash / sh. A later pattern wins: keywords and operators come last.
(comment) @comment
[(string) (raw_string) (ansi_c_string) (heredoc_body) (heredoc_start)] @string
(number) @number
(file_descriptor) @number
(command_name) @function
(function_definition name: (word) @function)
(variable_name) @variable
(special_variable_name) @variable
(test_operator) @operator

[
  "case" "do" "done" "elif" "else" "esac" "export" "fi" "for" "function"
  "if" "in" "select" "then" "unset" "until" "while" "declare" "local"
  "readonly" "typeset"
] @keyword

["$" "&&" "||" ">" ">>" "<" "|" "&" "!" "=" "==" "!=" "=~"] @operator
[";" ";;" "(" ")" "{" "}" "[" "]" "[[" "]]"] @punctuation
