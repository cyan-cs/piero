; INI / CFG as the grammar reads it: [sections], name = value, comments. Values
; stay plain: INI has no types, and the dialects differ.
(comment) @comment
(section_name) @heading
(setting_name) @key
"=" @operator
["[" "]"] @punctuation
