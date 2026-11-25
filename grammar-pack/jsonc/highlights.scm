; JSONC: the same shapes as JSON, with comments. Painted in order: a later
; pattern and a deeper node win, so the key comes after the string.
(string) @string
(pair key: (string) @key)
(escape_sequence) @escape
(number) @number
[(true) (false)] @boolean
(null) @null
(comment) @comment
["," ":"] @punctuation
["[" "]" "{" "}"] @punctuation
