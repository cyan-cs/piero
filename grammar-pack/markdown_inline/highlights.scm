; Markdown inline markup, painted over the block grammar's colours. A later
; pattern wins.
(emphasis) @markup
(strong_emphasis) @markup
(code_span) @string
(link_title) @string
[(emphasis_delimiter) (code_span_delimiter)] @punctuation

[
  (link_text)
  (image_description)
  (link_label)
  (link_destination)
  (uri_autolink)
] @link

(backslash_escape) @escape
(hard_line_break) @escape

(image ["!" "[" "]" "(" ")"] @punctuation)
(inline_link ["[" "]" "(" ")"] @punctuation)
(shortcut_link ["[" "]"] @punctuation)
