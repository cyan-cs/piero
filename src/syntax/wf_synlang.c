#include "wf_synlang.h"

#include <string.h>

#include "wf_syntax_abi.h"

/* Keep each extension in one row only: wf_synlang_for_name stops at the first
 * row that has it, and the test of the table says none is shared. */
static const WfSynLang langs[WF_SYNLANG_COUNT] = {
        {"json", "tree_sitter_json", ".json;.geojson;.webmanifest", NULL, false,
                false},
        {"jsonc", "tree_sitter_jsonc", ".jsonc;.code-workspace",
                "tsconfig.json;jsconfig.json;settings.json;launch.json;"
                "tasks.json;extensions.json",
                false, false},
        {"yaml", "tree_sitter_yaml", ".yaml;.yml", NULL, false, false},
        {"toml", "tree_sitter_toml", ".toml", "cargo.lock;pipfile", false,
                false},
        {"ini", "tree_sitter_ini", ".ini;.cfg;.inf;.desktop;.url",
                ".editorconfig;.gitconfig;.gitmodules", false, false},
        {"xml", "tree_sitter_xml",
                ".xml;.xsd;.xsl;.xslt;.svg;.xaml;.csproj;.vcxproj;.props;"
                ".targets;.plist;.config;.manifest;.resx;.nuspec;.rss;"
                ".atom;.wsdl",
                NULL, false, false},
        {"markdown", "tree_sitter_markdown", ".md;.markdown", NULL, false,
                false},
        {"markdown_inline", "tree_sitter_markdown_inline", NULL, NULL, false,
                true},
        {"powershell", "tree_sitter_powershell", ".ps1;.psm1;.psd1", NULL,
                false, false},
        {"bash", "tree_sitter_bash", ".sh;.bash",
                ".bashrc;.bash_profile;.bash_aliases;.profile;pkgbuild", false,
                false},
        {"dotenv", NULL, ".env",
                ".env;.env.local;.env.example;.env.development;"
                ".env.production;.env.test",
                true, false},
};

const WfSynLang *wf_synlang_all(size_t *n)
{
	if (n) {
		*n = WF_SYNLANG_COUNT;
	}
	return langs;
}

const WfSynLang *wf_synlang_by_id(const char *id)
{
	if (!id) {
		return NULL;
	}
	for (size_t i = 0; i < WF_SYNLANG_COUNT; i++) {
		if (!strcmp(langs[i].id, id)) {
			return &langs[i];
		}
	}
	return NULL;
}

/* True when `word` (n bytes, lower case) is one of the ';'-separated items. */
static bool in_list(const char *list, const char *word, size_t n)
{
	while (list && *list) {
		const char *end = strchr(list, ';');
		size_t len = end ? (size_t)(end - list) : strlen(list);

		if (len == n && !memcmp(list, word, n)) {
			return true;
		}
		list = end ? end + 1 : NULL;
	}
	return false;
}

const char *wf_syn_status_name(uint32_t status)
{
	static const char *const names[] = {"ok", "no-grammar",
	        "unknown-language", "path", "trust", "manifest", "load",
	        "symbol", "abi", "query", "limit", "timeout", "no-memory",
	        "bad-argument", "internal"};

	return status < sizeof names / sizeof names[0] ? names[status]
	                                               : "unknown-status";
}

const WfSynLang *wf_synlang_for_name(const wchar_t *leaf)
{
	char low[64];
	size_t n = 0, dot = (size_t)-1;

	if (!leaf) {
		return NULL;
	}
	for (; leaf[n]; n++) {
		wchar_t c = leaf[n];

		if (n + 1 >= sizeof low || c > 0x7e) {
			return NULL; /* long or not ASCII: no official name is */
		}
		low[n] = (char)(c >= L'A' && c <= L'Z' ? c + 32 : c);
		if ('.' == low[n]) {
			dot = n;
		}
	}
	low[n] = '\0';
	for (size_t i = 0; i < WF_SYNLANG_COUNT; i++) {
		if (in_list(langs[i].names, low, n)) {
			return &langs[i];
		}
	}
	if ((size_t)-1 != dot && dot + 1 < n) {
		for (size_t i = 0; i < WF_SYNLANG_COUNT; i++) {
			if (in_list(langs[i].exts, low + dot, n - dot)) {
				return &langs[i];
			}
		}
	}
	return NULL;
}
