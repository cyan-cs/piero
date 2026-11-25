/*
 * CSV and TSV as aligned columns (DR-DRAFT-027): the delimiter by name, the parser
 * (quotes, doubled quotes, breaks inside quotes, CRLF, ragged records, wide
 * characters), the limits (200 records, 64 columns, 32 cells a cell), and the
 * preview rewritten from a decoded file (BOM'd UTF-16, tabs kept for a TSV, a
 * semicolon file, non-text left alone).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preview/wf_table.h"
#include "wf_test.h"

static char *render(const char *in, char delim, size_t *rows, bool *cut)
{
	WfBuf out;
	char *copy;

	wf_buf_init(&out);
	CHECK(WF_OK == wf_table_render(in, strlen(in), delim, &out, rows, cut));
	copy = _strdup(wf_buf_cstr(&out));
	wf_buf_free(&out);
	return copy;
}

static bool renders_as(const char *in, char delim, const char *want)
{
	size_t rows;
	bool cut;
	char *got = render(in, delim, &rows, &cut);
	bool same = got && !strcmp(got, want);

	if (!same) {
		printf("  got:  [%s]\n  want: [%s]\n", got ? got : "(null)",
		        want);
	}
	free(got);
	return same;
}

static void delimiter_by_name(void)
{
	CHECK(',' == wf_preview_table_delim(L"a.csv"));
	CHECK(',' == wf_preview_table_delim(L"C:\\x\\DATA.CSV"));
	CHECK('\t' == wf_preview_table_delim(L"a.tsv"));
	CHECK('\t' == wf_preview_table_delim(L"a.Tab"));
	CHECK(0 == wf_preview_table_delim(L"a.txt"));
	CHECK(0 == wf_preview_table_delim(L"csv"));
	CHECK(0 == wf_preview_table_delim(L"a.csv.bak"));
	CHECK(0 == wf_preview_table_delim(L""));
	CHECK(0 == wf_preview_table_delim(NULL));
}

static void simple_tables(void)
{
	CHECK(renders_as("name,age\nAl,3\nBobby,40", ',',
	        "name  | age\n------+----\nAl    | 3\nBobby | 40"));
	CHECK(renders_as("a\tb\n1\t22", '\t', "a | b\n--+---\n1 | 22"));
	CHECK(renders_as("a;b\n1;22", ';', "a | b\n--+---\n1 | 22"));
	/* CRLF is one break */
	CHECK(renders_as("a,b\r\n1,2\r\n", ',', "a | b\n--+--\n1 | 2"));
	/* empty records are skipped; so is the end of the text */
	CHECK(renders_as("a,b\n\n\n1,2\n\n", ',', "a | b\n--+--\n1 | 2"));
	/* a record that ends in empty cells ends where its text ends */
	CHECK(renders_as("a,b\n1,\n", ',', "a | b\n--+--\n1"));
	/* one column */
	CHECK(renders_as("x\ny", ',', "x\n-\ny"));
}

static void quoting(void)
{
	CHECK(renders_as("a,b,c\n\"x,y\",\"say \"\"hi\"\"\",3", ',',
	        "a   | b        | c\n----+----------+--\nx,y | say \"hi\" | "
	        "3"));
	/* a break inside quotes is a space, and the record goes on */
	CHECK(renders_as("a,b\n\"x\ny\",2", ',', "a   | b\n----+--\nx y | 2"));
	/* an unclosed quote ends at the end of the text */
	CHECK(renders_as("a,b\n1,\"b\nc", ',', "a | b\n--+----\n1 | b c"));
	/* a quote in the middle of a field is a character */
	CHECK(renders_as("a,b\nx\"y,2", ',', "a   | b\n----+--\nx\"y | 2"));
	/* an empty quoted field is still a field (the record is not empty) */
	CHECK(renders_as("a,b\n\"\",x", ',', "a | b\n--+--\n  | x"));
}

static void shapes(void)
{
	/* ragged: the widest record sets the columns; no trailing blanks */
	CHECK(renders_as("a,b,c\n1\n1,2,3,4", ',',
	        "a | b | c\n--+---+---+--\n1\n1 | 2 | 3 | 4"));
	/* wide characters count as two cells */
	CHECK(renders_as(
	        "\xe5\x90\x8d\xe5\x89\x8d,x\n\xe5\xb1\xb1\xe7\x94\xb0\xe5"
	        "\xa4\xaa\xe9\x83\x8e,y",
	        ',',
	        "\xe5\x90\x8d\xe5\x89\x8d     | "
	        "x\n---------+--\n\xe5\xb1\xb1\xe7"
	        "\x94\xb0\xe5\xa4\xaa\xe9\x83\x8e | y"));
	/* nothing */
	{
		size_t rows = 9;
		bool cut = true;
		char *s = render("", ',', &rows, &cut);

		CHECK(0 == rows && !cut && !strcmp(s, ""));
		free(s);
		s = render("\n\n\r\n", ',', &rows, &cut);
		CHECK(0 == rows && !strcmp(s, ""));
		free(s);
	}
}

static char *many_rows(size_t n, size_t blanks_after)
{
	size_t cap = n * 12 + blanks_after + 64, at = 0;
	char *s = malloc(cap);

	for (size_t i = 0; i < n; i++) {
		at += (size_t)snprintf(s + at, cap - at, "r%zu,v\n", i);
	}
	for (size_t i = 0; i < blanks_after; i++) {
		s[at++] = '\n';
	}
	s[at] = 0;
	return s;
}

static void limits(void)
{
	size_t rows;
	bool cut;
	char *in, *out;

	in = many_rows(250, 0);
	out = render(in, ',', &rows, &cut);
	CHECK(200 == rows && cut);
	CHECK(NULL != strstr(out, "r199"));
	CHECK(NULL == strstr(out, "r200"));
	free(in);
	free(out);

	in = many_rows(200, 5); /* exactly 200, then only blank lines */
	out = render(in, ',', &rows, &cut);
	CHECK(200 == rows && !cut);
	CHECK(NULL != strstr(out, "r199"));
	free(in);
	free(out);

	in = many_rows(201, 0);
	out = render(in, ',', &rows, &cut);
	CHECK(200 == rows && cut);
	free(in);
	free(out);

	/* 70 columns: 64 are drawn, and the cut is said */
	{
		char row[400];
		size_t at = 0;

		for (int c = 0; c < 70; c++) {
			at += (size_t)snprintf(row + at, sizeof row - at,
			        c ? ",c%d" : "c%d", c);
		}
		out = render(row, ',', &rows, &cut);
		CHECK(1 == rows && cut);
		CHECK(NULL != strstr(out, "c63"));
		CHECK(NULL == strstr(out, "c64"));
		free(out);
	}

	/* a long cell is cut to 32 cells with "..." */
	{
		char text[200];
		size_t len;

		memset(text, 'x', 100);
		memcpy(text + 100, ",y\n", 4);
		out = render(text, ',', &rows, &cut);
		len = strcspn(out, "\n");
		CHECK(NULL != strstr(out, "..."));
		CHECK(strstr(out, " | y") &&
		        (size_t)(strstr(out, " | y") - out) == 32);
		CHECK(len == 32 + 4);
		free(out);
	}
}

static const char *line_of(const WfPreview *p, size_t i)
{
	return p->text.data + p->lines[i];
}

static void preview_text(void)
{
	WfPreview *p = NULL;
	const char *csv = "id,name\n1,Al\n22,\"B, C\"\n";

	CHECK(WF_OK == wf_preview_decode(csv, strlen(csv), 0, &p));
	CHECK(WF_PREVIEW_TEXT == p->kind);
	CHECK(WF_OK == wf_preview_tableize(p, ','));
	CHECK(4 == p->nlines);
	CHECK(!strcmp(line_of(p, 0), "id | name"));
	CHECK(!strcmp(line_of(p, 1), "---+-----"));
	CHECK(!strcmp(line_of(p, 2), "1  | Al"));
	CHECK(!strcmp(line_of(p, 3), "22 | B, C"));
	CHECK(9 == p->max_cells);
	CHECK(!p->truncated);
	wf_preview_free(p);

	/* a TSV needs its tabs: plain decoding turned them into spaces */
	{
		const char *tsv = "id\tname\n1\tAl\n";

		CHECK(WF_OK ==
		        wf_preview_decode_opt(tsv, strlen(tsv), 0, true, &p));
		CHECK(WF_OK == wf_preview_tableize(p, '\t'));
		CHECK(3 == p->nlines);
		CHECK(!strcmp(line_of(p, 0), "id | name"));
		CHECK(!strcmp(line_of(p, 2), "1  | Al"));
		wf_preview_free(p);

		CHECK(WF_OK == wf_preview_decode(tsv, strlen(tsv), 0, &p));
		CHECK(WF_OK == wf_preview_tableize(p, '\t'));
		CHECK(!strcmp(
		        line_of(p, 0), "id    name")); /* one cell: no table */
		wf_preview_free(p);
	}

	/* a semicolon file named .csv */
	{
		const char *semi = "a;b\n1;2\n";

		CHECK(WF_OK == wf_preview_decode(semi, strlen(semi), 0, &p));
		CHECK(WF_OK == wf_preview_tableize(p, ','));
		CHECK(!strcmp(line_of(p, 0), "a | b"));
		wf_preview_free(p);
	}

	/* a semicolon file whose quoted cells hold commas, also over a line break */
	{
		const char *semi =
		        "name;\"city, country\"\nAlice;\"Tokyo, Japan\"\n";
		const char *broken = "\"a,\nb,\";c\n1;2\n";

		CHECK(WF_OK == wf_preview_decode(semi, strlen(semi), 0, &p));
		CHECK(WF_OK == wf_preview_tableize(p, ','));
		CHECK(!strcmp(line_of(p, 0), "name  | city, country"));
		wf_preview_free(p);
		CHECK(WF_OK ==
		        wf_preview_decode(broken, strlen(broken), 0, &p));
		CHECK(WF_OK == wf_preview_tableize(p, ','));
		CHECK(strstr(line_of(p, 0), " | c") !=
		        NULL); /* two cells: ';' */
		wf_preview_free(p);
	}

	/* UTF-16 LE with a BOM */
	{
		const unsigned char le[] = {0xff, 0xfe, 'a', 0, ',', 0, 'b', 0,
		        '\n', 0, '1', 0, ',', 0, '2', 0};

		CHECK(WF_OK == wf_preview_decode(le, sizeof le, 0, &p));
		CHECK(WF_OK == wf_preview_tableize(p, ','));
		CHECK(3 == p->nlines);
		CHECK(!strcmp(line_of(p, 2), "1 | 2"));
		wf_preview_free(p);
	}

	/* rows left out mark the preview truncated */
	{
		char *big = many_rows(300, 0);

		CHECK(WF_OK == wf_preview_decode(big, strlen(big), 0, &p));
		CHECK(!p->truncated);
		CHECK(WF_OK == wf_preview_tableize(p, ','));
		CHECK(p->truncated);
		CHECK(201 == p->nlines); /* 200 records and the rule */
		wf_preview_free(p);
		free(big);
	}

	/* nothing to draw, or not text: the preview stays as it was */
	{
		CHECK(WF_OK == wf_preview_decode("\n\n", 2, 0, &p));
		{
			size_t n = p->nlines, len = p->text.len;

			CHECK(WF_OK == wf_preview_tableize(p, ','));
			CHECK(n == p->nlines && len == p->text.len);
		}
		wf_preview_free(p);
		p = wf_preview_notice(WF_PREVIEW_BINARY, "Binary content");
		CHECK(WF_OK == wf_preview_tableize(p, ','));
		CHECK(WF_PREVIEW_BINARY == p->kind);
		wf_preview_free(p);
	}
	CHECK(WF_E_INVAL == wf_preview_tableize(NULL, ','));
}

int main(void)
{
	delimiter_by_name();
	simple_tables();
	quoting();
	shapes();
	limits();
	preview_text();
	return TEST_RESULT();
}
