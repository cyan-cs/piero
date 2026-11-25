/*
 * SHA-256 on request (#) through the real product: a folder is refused, a file's
 * digest appears on the status line and in the details window, the files are as
 * they were (DR-DRAFT-027). Included by p0_conpty_host.c after p0_state_gate.c and
 * p0_dirsize.c, whose helpers (gate_*, dirsize_row) it uses.
 */

#define HASH_ABC                                                               \
	"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

static int scn_hash(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	char *before = NULL, *after = NULL;
	wchar_t a[1100];
	bool ok = false;
	int fails = e->fails;

	if (!g || !gate_wanted("hash")) {
		free(g);
		return 0;
	}
	if (!gate_open(g, e, "hash") || !gate_mkdir(g, L"browse\\sub") ||
	        !gate_path(g, L"browse\\a.txt", a) || !gate_write(a, "abc") ||
	        !gate_write(g->cfg, DIRSIZE_CONFIG) || !gate_launch(g)) {
		goto done;
	}
	ok = dirsize_row(g, "a.txt", NULL, 8000);
	ev_check(e, "hash_list_is_shown", 0, "sub/ and a.txt listed",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	before = gate_tree_text(g->browse);
	/* the cursor is on the folder: it has no hash */
	pty_send(&g->pr.p, "#", 1);
	ok = product_wait_ms(&g->pr.p, "A folder has no SHA-256", g->pr.screen,
	        sizeof g->pr.screen, 5000);
	ev_check(e, "hash_folder_is_refused", 0, "A folder has no SHA-256",
	        ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "j", 1);
	pty_send(&g->pr.p, "#", 1);
	ok = product_wait_ms(&g->pr.p, "SHA-256 ba7816bf8f01cfea", g->pr.screen,
	        sizeof g->pr.screen, 10000);
	product_snapshot(&g->pr, "hash_status");
	ev_check(e, "hash_digest_is_shown", 0, "SHA-256 ba7816bf8f01cfea...",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	/* the preview pane of the text file has it, whole, on its last row */
	ok = product_wait_ms(&g->pr.p, "SHA-256 " HASH_ABC, g->pr.screen,
	        sizeof g->pr.screen, 5000);
	ev_check(e, "hash_digest_is_in_the_pane", 0, "SHA-256 " HASH_ABC,
	        ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "i", 1);
	ok = ok &&
	        product_wait_ms(&g->pr.p, HASH_ABC, g->pr.screen,
	                sizeof g->pr.screen, 8000);
	product_snapshot(&g->pr, "hash_details");
	ev_check(e, "hash_digest_is_in_the_details", 0, HASH_ABC,
	        ok ? "observed" : "missing", ok);
	pty_send(&g->pr.p, "\x1b", 1);
	after = gate_tree_text(g->browse);
	ok = before && after && !strcmp(before, after);
	ev_check(e, "hash_changes_no_file", 0, "tree as before",
	        ok ? "unchanged" : "CHANGED", ok);
	ok = ok && gate_quit(g);
done:
	free(before);
	free(after);
	return gate_first_run_end(g, e, ok, fails) ? (g_gate_skips ? 77 : 0)
	                                           : 1;
}
