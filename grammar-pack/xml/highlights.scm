; XML. Tag names are keywords, attribute names are keys. A later pattern wins.
(Comment) @comment

(STag (Name) @keyword)
(ETag (Name) @keyword)
(EmptyElemTag (Name) @keyword)
(Attribute (Name) @key)
(Attribute (AttValue) @string)

(EntityRef) @constant
(CharRef) @constant
(PEReference) @constant

"xml" @keyword
["version" "encoding" "standalone"] @key
(VersionNum) @number
(EncName) @string
["yes" "no"] @boolean

(PI) @preproc
(PI (PITarget) @keyword)
(XmlModelPI "xml-model" @keyword)
(StyleSheetPI "xml-stylesheet" @keyword)
(PseudoAtt (Name) @key)
(PseudoAtt (PseudoAttValue) @string)

["DOCTYPE" "ELEMENT" "ATTLIST" "ENTITY" "NOTATION" "PUBLIC" "SYSTEM"] @keyword
(doctypedecl (Name) @type)
(PubidLiteral) @string
(SystemLiteral) @string
(EntityValue) @string
(DefaultDecl (AttValue) @string)
["#REQUIRED" "#IMPLIED" "#FIXED" "#PCDATA"] @type

(CDSect (CDStart) @markup)
(CDSect (CData) @string)
(CDSect "]]>" @markup)

["<?" "?>" "<!" "]]>" "<" ">" "</" "/>"] @punctuation
["(" ")" "[" "]"] @punctuation
["," "|"] @punctuation
["=" "*" "?" "+" "%"] @operator
