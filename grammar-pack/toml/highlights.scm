; TOML. A later pattern wins: table names come after plain keys.
(comment) @comment
(string) @string
(escape_sequence) @escape
[(integer) (float)] @number
(boolean) @boolean
[
  (offset_date_time)
  (local_date_time)
  (local_date)
  (local_time)
] @constant

(bare_key) @key
(quoted_key) @key

(table (bare_key) @heading)
(table (quoted_key) @heading)
(table (dotted_key (bare_key) @heading))
(table (dotted_key (quoted_key) @heading))
(table_array_element (bare_key) @heading)
(table_array_element (quoted_key) @heading)
(table_array_element (dotted_key (bare_key) @heading))
(table_array_element (dotted_key (quoted_key) @heading))

"=" @operator
["." ","] @punctuation
["[" "]" "[[" "]]" "{" "}"] @punctuation
