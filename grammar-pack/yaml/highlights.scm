; YAML. A later pattern wins: the keys come after the scalars.
(comment) @comment
[
  (double_quote_scalar)
  (single_quote_scalar)
  (block_scalar)
  (string_scalar)
] @string
[(integer_scalar) (float_scalar)] @number
(boolean_scalar) @boolean
(null_scalar) @null
[(anchor_name) (alias_name)] @variable
(tag) @type
[(yaml_directive) (tag_directive) (reserved_directive)] @preproc

(block_mapping_pair
  key: (flow_node
    [(double_quote_scalar) (single_quote_scalar)] @key))
(block_mapping_pair
  key: (flow_node
    (plain_scalar
      (string_scalar) @key)))
(flow_mapping
  (_
    key: (flow_node
      [(double_quote_scalar) (single_quote_scalar)] @key)))
(flow_mapping
  (_
    key: (flow_node
      (plain_scalar
        (string_scalar) @key))))

["," "-" ":" ">" "?" "|"] @punctuation
["[" "]" "{" "}"] @punctuation
["*" "&" "---" "..."] @operator
