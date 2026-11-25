; Markdown, the block grammar (headings, fences, lists, quotes). The inline
; markup (emphasis, code spans, links) is markdown_inline's, run over the
; `inline` nodes by piero-syntax.dll. A later pattern wins.
(atx_heading (inline) @heading)
(setext_heading (paragraph) @heading)
[
  (atx_h1_marker)
  (atx_h2_marker)
  (atx_h3_marker)
  (atx_h4_marker)
  (atx_h5_marker)
  (atx_h6_marker)
  (setext_h1_underline)
  (setext_h2_underline)
] @heading

[(indented_code_block) (fenced_code_block)] @markup
(fenced_code_block_delimiter) @punctuation
(info_string (language) @type)
(link_title) @string
(link_destination) @link
(link_label) @link

[
  (list_marker_plus)
  (list_marker_minus)
  (list_marker_star)
  (list_marker_dot)
  (list_marker_parenthesis)
  (thematic_break)
  (block_quote_marker)
] @punctuation

(backslash_escape) @escape
