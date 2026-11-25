; JSON. Painted in order: a later pattern and a deeper node win, so the key
; (a string too) comes after the string.
(string) @string
(pair key: (string) @key)
(escape_sequence) @escape
(number) @number
[(true) (false)] @boolean
(null) @null
(comment) @comment
["," ":"] @punctuation
["[" "]" "{" "}"] @punctuation
