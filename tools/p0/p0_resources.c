/*
 * What the program holds, measured through the real product: after it has
 * started and settled, private bytes, commit, threads, handles and the CPU it
 * has used; then the four on-demand features (# D L F) once and again, the time
 * from the key to the screen that answers it, and what is held after each.
 * The figures are written as key=value lines to artifacts\resources.txt and to
 * the log; nothing is asserted about them (they are for comparison between
 * builds), except that every feature gives its answer. Included by
 * p0_conpty_host.c after p0_openterm.c, whose gate_* helpers it uses.
 */

/* Under the address sanitizer freed memory is held back (a quarantine of up to
 * 256 MB) and the process starts with tens of megabytes of its own: the bound
 * on memory means nothing there, the counts of threads and handles still do. */
#if defined(__SANITIZE_ADDRESS__)
#	define RES_SANITIZED 1
#elif defined(__has_feature)
#	if __has_feature(address_sanitizer)
#		define RES_SANITIZED 1
#	endif
#endif
#ifndef RES_SANITIZED
#	define RES_SANITIZED 0
#endif

typedef struct
{
	double private_bytes, commit, working_set, handles, threads, cpu_ms;
} ResSample;

static size_t res_threads(DWORD pid)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te;
	size_t n = 0;

	if (INVALID_HANDLE_VALUE == snap) {
		return 0;
	}
	te.dwSize = sizeof te;
	for (BOOL ok = Thread32First(snap, &te); ok;
	        ok = Thread32Next(snap, &te)) {
		n += te.th32OwnerProcessID == pid;
	}
	CloseHandle(snap);
	return n;
}

/* How many processes have `pid` as their parent: the sandboxed picture decoder
 * is one, and exists only while the picture worker does. */
static size_t res_children(DWORD pid)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W pe;
	size_t n = 0;

	if (INVALID_HANDLE_VALUE == snap) {
		return 0;
	}
	pe.dwSize = sizeof pe;
	for (BOOL ok = Process32FirstW(snap, &pe); ok;
	        ok = Process32NextW(snap, &pe)) {
		n += pe.th32ParentProcessID == pid;
	}
	CloseHandle(snap);
	return n;
}

/* The private bytes of those children (0 where the system refuses to say). */
static double res_children_bytes(DWORD pid)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W pe;
	double bytes = 0;

	if (INVALID_HANDLE_VALUE == snap) {
		return 0;
	}
	pe.dwSize = sizeof pe;
	for (BOOL ok = Process32FirstW(snap, &pe); ok;
	        ok = Process32NextW(snap, &pe)) {
		HANDLE h;
		PROCESS_MEMORY_COUNTERS_EX m;

		if (pe.th32ParentProcessID != pid) {
			continue;
		}
		h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
		        pe.th32ProcessID);
		memset(&m, 0, sizeof m);
		m.cb = sizeof m;
		if (h &&
		        K32GetProcessMemoryInfo(
		                h, (PROCESS_MEMORY_COUNTERS *)&m, sizeof m)) {
			bytes += (double)m.PrivateUsage;
		}
		if (h) {
			CloseHandle(h);
		}
	}
	CloseHandle(snap);
	return bytes;
}

static bool res_sample(Gate *g, ResSample *s)
{
	HANDLE h = g->pr.p.pi.hProcess;
	PROCESS_MEMORY_COUNTERS_EX m;
	DWORD handles = 0;
	FILETIME created, exited, kernel, user;

	memset(&m, 0, sizeof m);
	m.cb = sizeof m;
	if (!K32GetProcessMemoryInfo(
	            h, (PROCESS_MEMORY_COUNTERS *)&m, sizeof m) ||
	        !GetProcessHandleCount(h, &handles) ||
	        !GetProcessTimes(h, &created, &exited, &kernel, &user)) {
		return false;
	}
	s->private_bytes = (double)m.PrivateUsage;
	s->commit = (double)m.PagefileUsage;
	s->working_set = (double)m.WorkingSetSize;
	s->handles = (double)handles;
	s->threads = (double)res_threads(GetProcessId(h));
	s->cpu_ms = (double)(((uint64_t)kernel.dwHighDateTime << 32 |
	                             kernel.dwLowDateTime) +
	                    ((uint64_t)user.dwHighDateTime << 32 |
	                            user.dwLowDateTime)) /
	        10000.0;
	return true;
}

/* The report: key=value lines, kept in memory and written once at the end,
 * so that nothing is made in the owned area but the evidence. */
typedef struct
{
	char text[4096];
	size_t n;
} ResReport;

static void res_add(ResReport *r, const char *fmt, ...)
{
	va_list ap;
	int k;

	va_start(ap, fmt);
	k = vsnprintf(r->text + r->n, sizeof r->text - r->n, fmt, ap);
	va_end(ap);
	if (k > 0 && r->n + (size_t)k < sizeof r->text) {
		r->n += (size_t)k;
	}
}

static void res_line(ResReport *r, const char *label, const ResSample *s)
{
	res_add(r,
	        "%s private_bytes=%.0f commit=%.0f working_set=%.0f "
	        "threads=%.0f "
	        "handles=%.0f cpu_ms=%.1f\n",
	        label, s->private_bytes, s->commit, s->working_set, s->threads,
	        s->handles, s->cpu_ms);
}

/* The time from the key to the screen that holds `needle`; -1 when it never
 * came. */
static double res_key_to_screen(
        Gate *g, const char *keys, const char *needle, DWORD ms)
{
	double t0 = now_ms();

	pty_send(&g->pr.p, keys, strlen(keys));
	if (!product_wait_ms(
	            &g->pr.p, needle, g->pr.screen, sizeof g->pr.screen, ms)) {
		return -1.0;
	}
	return now_ms() - t0;
}

/* A picture and an archive in the folder (they sort after the text files, so
 * the keys of the other checks land as before), and the idle time of the heavy
 * modules cut to 3 s: the program reads PIERO_IDLE_RELEASE_MS at start. */
static bool res_idle_fixtures(Gate *g)
{
	unsigned char bmp[54 + 8 * 24];
	wchar_t zip[1100], from[1100];

#ifndef WF_TEST_ARCHIVE_DIR
	(void)bmp;
	(void)zip;
	(void)from;
	ev_check(g->pr.e, "archive_fixture", 0, "tests/archive", "not built in",
	        0);
	return false;
#else
	if (!product_file_bytes(
	            &g->pr.lab, L"browse\\zz.bmp", bmp, product_bmp(bmp)) ||
	        !gate_path(g, L"browse\\zz.zip", zip)) {
		return false;
	}
	MultiByteToWideChar(
	        CP_UTF8, 0, WF_TEST_ARCHIVE_DIR "/basic.zip", -1, from, 1100);
	for (wchar_t *c = from; *c; c++) {
		if (L'/' == *c) {
			*c = L'\\';
		}
	}
	return CopyFileW(from, zip, TRUE) &&
	        p0_area_track(g->pr.lab.area, L"browse\\zz.zip", 0) &&
	        SetEnvironmentVariableW(L"PIERO_IDLE_RELEASE_MS", L"3000");
#endif
}

/* DR-DRAFT-037. A picture on screen keeps the decoder (and its process); when it
 * has been off the screen for the idle time the worker and the process are gone,
 * and the picture is shown again by a worker made for it. The archive is the
 * same: the index kept while it is on screen, indexed again after the idle time,
 * the folder listed as before. */
static bool res_idle_release(Gate *g, const ResSample *idle, ResReport *rep)
{
	ResSample s, off;
	DWORD pid = GetProcessId(g->pr.p.pi.hProcess);
	bool in_view, gone, again, kept, archive_again, steady, big_ok;

	/* to the top (the list does not wrap), then down to zz.bmp */
	pty_send(&g->pr.p, "kkkkkkkkkkkk", 12);
	Sleep(300);
	pty_send(&g->pr.p, "jjjjjj", 6);
	if (!product_wait_ms(&g->pr.p, "8 x 8", g->pr.screen,
	            sizeof g->pr.screen, 30000)) {
		product_snapshot(&g->pr, "resources_picture_not_shown");
		ev_check(g->pr.e, "resources_picture_is_shown", 0, "8 x 8",
		        "missing", 0);
		return false;
	}
	Sleep(6000); /* twice the idle time, with the picture on screen */
	kept = res_sample(g, &s) && res_children(pid) >= 1 &&
	        NULL != strstr(g->pr.screen, "8 x 8");
	res_line(rep, "picture_on_screen_after_idle_time", &s);
	res_add(rep, "decoder_process_private_bytes=%.0f\n",
	        res_children_bytes(pid));
	in_view = kept;
	pty_send(&g->pr.p, "k",
	        1); /* a text file: the picture is off the screen */
	Sleep(6000);
	gone = res_sample(g, &s) && 0 == res_children(pid) &&
	        s.threads <= idle->threads + 1;
	res_line(rep, "picture_off_screen_after_idle_time", &s);
	off = s;
	pty_send(&g->pr.p, "j", 1);
	again = product_wait_ms(
	        &g->pr.p, "8 x 8", g->pr.screen, sizeof g->pr.screen, 30000);
	ev_check(g->pr.e,
	        "resources_picture_is_kept_on_screen_and_released_off_it", 0,
	        "the decoder process stays while the picture is shown, is gone "
	        "after the idle time off screen, and the picture comes back",
	        in_view && gone && again ? "observed" : "WRONG",
	        in_view && gone && again);
	if (!(in_view && gone && again)) {
		return false;
	}
	/* once more: what the first release left (libraries the first decode
	 * loaded stay) is not added to by the second */
	pty_send(&g->pr.p, "k", 1);
	Sleep(6000);
	steady = res_sample(g, &s) && 0 == res_children(pid) &&
	        s.threads <= off.threads && s.handles <= off.handles + 2;
	res_line(rep, "picture_off_screen_second_time", &s);
	pty_send(&g->pr.p, "j", 1);
	steady = steady &&
	        product_wait_ms(&g->pr.p, "8 x 8", g->pr.screen,
	                sizeof g->pr.screen, 30000);
	ev_check(g->pr.e, "resources_picture_cycles_do_not_accumulate", 0,
	        "after a second release: no decoder process, threads and "
	        "handles "
	        "(within 2) as after the first",
	        steady ? "observed" : "WRONG", steady);
	if (!steady) {
		return false;
	}
	/* the archive: the cursor on it lists what is inside (the index is made) */
	pty_send(&g->pr.p, "j", 1);
	archive_again = product_wait_ms(&g->pr.p, "README.md", g->pr.screen,
	        sizeof g->pr.screen, 20000);
	pty_send(&g->pr.p, "\r", 1);
	archive_again = archive_again &&
	        product_wait_ms(&g->pr.p, "ARCHIVE (read-only)", g->pr.screen,
	                sizeof g->pr.screen, 10000);
	Sleep(6000); /* inside it: the index is in use the whole time */
	pty_send(&g->pr.p, "j", 1);
	archive_again = archive_again &&
	        product_wait_ms(&g->pr.p, "main.c", g->pr.screen,
	                sizeof g->pr.screen, 5000);
	pty_send(&g->pr.p, "h", 1);
	archive_again = archive_again &&
	        product_wait_ms(&g->pr.p, "NORMAL", g->pr.screen,
	                sizeof g->pr.screen, 10000);
	pty_send(&g->pr.p, "kk", 2); /* a text file: off the archive */
	Sleep(6000);                 /* the index is released */
	pty_send(&g->pr.p, "jj", 2);
	archive_again = archive_again &&
	        product_wait_ms(&g->pr.p, "README.md", g->pr.screen,
	                sizeof g->pr.screen, 20000);
	res_sample(g, &s);
	res_line(rep, "archive_listed_again_after_release", &s);
	ev_check(g->pr.e, "resources_archive_is_listed_again_after_release", 0,
	        "the archive lists inside before and after the idle time, and "
	        "stays usable while it is on screen",
	        archive_again ? "observed" : "WRONG", archive_again);
	/* The picture worker is gone (the cursor has stood on the archive for the
	 * idle time): the next picture makes it again, and its answer has to come
	 * in by itself, with no other key. The loop waits on the new worker's event
	 * (fill_lazy_wait_handles); a decode that outlasted the loading timer
	 * could otherwise wait for the idle release, 3 s later. This catches that
	 * only when the decode is slow, and it is not slow here (about 0.1 s), so it
	 * is a check that the picture comes back promptly, not a proof of the wait. */
	{
		double t0 = now_ms();

		pty_send(&g->pr.p, "k", 1);
		big_ok = product_wait_ms(&g->pr.p, "8 x 8", g->pr.screen,
		        sizeof g->pr.screen, 20000);
		t0 = now_ms() - t0;
		res_add(rep, "picture_after_release_ms=%.0f\n", t0);
		big_ok = big_ok && t0 < 2000.0;
		ev_check(g->pr.e,
		        "resources_picture_comes_back_promptly_after_release",
		        0, "the picture is shown within 2 s of one key",
		        big_ok ? "observed" : "WRONG", big_ok);
	}
	return archive_again && big_ok;
}

static bool resources_case(Ev *e)
{
	Gate *g = calloc(1, sizeof *g);
	static ResReport rep;
	bool ok = false;
	int fails = e->fails;
	wchar_t a[1100], b[1100], c[1100];
	ResSample s, idle, after[4];
	double t_start;
	double first_hash, second_hash, first_size, second_size, first_lock,
	        second_lock, first_dupes, second_dupes;

	if (!g || !gate_wanted("resources")) {
		free(g);
		return true;
	}
	if (!gate_open(g, e, "resources") || !gate_mkdir(g, L"browse\\sub") ||
	        !gate_path(g, L"browse\\a.txt", a) || !gate_write(a, "abc") ||
	        !gate_path(g, L"browse\\b.txt", b) || !gate_write(b, "abcd") ||
	        !gate_path(g, L"browse\\c.txt", c) || !gate_write(c, "abcd") ||
	        !gate_write(g->cfg,
	                DIRSIZE_CONFIG "[preview]\r\nimages = true\r\n")) {
		goto done;
	}
	if (!res_idle_fixtures(g)) {
		goto done;
	}
	g->wait_for = "a.txt";
	t_start = now_ms();
	if (!gate_launch(g)) {
		goto done;
	}
	t_start = now_ms() - t_start;
	Sleep(1500); /* what starts late has started */
	rep.n = 0;
	rep.text[0] = 0;
	if (!res_sample(g, &s)) {
		goto done;
	}
	res_add(&rep, "start_to_listing_ms=%.0f\n", t_start);
	res_line(&rep, "idle", &s);
	idle = s;

	/* # : a folder is refused, a file gets its digest; then another file */
	pty_send(&g->pr.p, "j", 1);
	first_hash =
	        res_key_to_screen(g, "#", "SHA-256 ba7816bf8f01cfea", 10000);
	pty_send(&g->pr.p, "j", 1);
	second_hash =
	        res_key_to_screen(g, "#", "SHA-256 88d4266fd4e6338d", 10000);
	Sleep(300);
	ok = first_hash >= 0 && second_hash >= 0 && res_sample(g, &s);
	ev_check(e, "resources_hash_answers", 0, "both digests shown",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	res_add(&rep, "hash_first_ms=%.1f\nhash_second_ms=%.1f\n", first_hash,
	        second_hash);
	res_line(&rep, "after_hash", &s);
	after[0] = s;

	/* D : the folders of the listing get their sizes. The second time the
	 * folder holds something new, so that the answer is a change on screen. */
	{
		wchar_t x[1100];
		double t0 = now_ms();

		pty_send(&g->pr.p, "D", 1);
		ok = dirsize_row(g, "sub/", "0 B", 10000);
		first_size = ok ? now_ms() - t0 : -1.0;
		ok = ok && gate_path(g, L"browse\\sub\\x.txt", x) &&
		        gate_write(x, "123");
		Sleep(600); /* the watcher takes the new file in */
		t0 = now_ms();
		pty_send(&g->pr.p, "D", 1);
		ok = ok && dirsize_row(g, "sub/", "3 B", 10000);
		second_size = ok ? now_ms() - t0 : -1.0;
	}
	Sleep(300);
	ok = ok && res_sample(g, &s);
	ev_check(e, "resources_dirsize_answers", 0, "both sizes shown",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	res_add(&rep, "dirsize_first_ms=%.1f\ndirsize_second_ms=%.1f\n",
	        first_size, second_size);
	res_line(&rep, "after_dirsize", &s);
	after[1] = s;

	/* L : who has the file open (nobody does) ; Esc ; again */
	first_lock = res_key_to_screen(g, "L", "No program was found", 15000);
	pty_send(&g->pr.p, "\x1b", 1);
	Sleep(300);
	second_lock = res_key_to_screen(g, "L", "No program was found", 15000);
	pty_send(&g->pr.p, "\x1b", 1);
	Sleep(300);
	ok = first_lock >= 0 && second_lock >= 0 && res_sample(g, &s);
	ev_check(e, "resources_lock_answers", 0, "both answers shown",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	res_add(&rep, "lock_first_ms=%.1f\nlock_second_ms=%.1f\n", first_lock,
	        second_lock);
	res_line(&rep, "after_lock", &s);
	after[2] = s;

	/* F : duplicates below this folder (b.txt and c.txt are equal) ; Esc ; again */
	first_dupes = res_key_to_screen(g, "F", "finished", 20000);
	pty_send(&g->pr.p, "\x1b", 1);
	Sleep(300);
	second_dupes = res_key_to_screen(g, "F", "finished", 20000);
	pty_send(&g->pr.p, "\x1b", 1);
	Sleep(500);
	ok = first_dupes >= 0 && second_dupes >= 0 && res_sample(g, &s);
	ev_check(e, "resources_dupes_answer", 0, "both searches finished",
	        ok ? "observed" : "missing", ok);
	if (!ok) {
		goto done;
	}
	res_add(&rep, "dupes_first_ms=%.1f\ndupes_second_ms=%.1f\n",
	        first_dupes, second_dupes);
	res_line(&rep, "after_dupes", &s);
	after[3] = s;
	Sleep(1500);
	if (res_sample(g, &s)) {
		res_line(&rep, "settled_after_all", &s);
	}
	/* The workers of # D L F are made by the first request and stopped when
	 * they have nothing in flight and nothing to poll (DR-DRAFT-035 and 036):
	 * at rest the program has fewer threads than it had when all four were
	 * started with it (21 here, 15 now); after each feature, once it has
	 * answered, it has the threads it had at rest, and the same once all four
	 * have been used; and each answer came in at once, not at the next time
	 * the loop woke for another reason (which once took 4 s). */
	{
		double slowest = first_hash;
		double all[] = {second_hash, first_size, second_size,
		        first_lock, second_lock, first_dupes, second_dupes};
		bool released = true;

		for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
			slowest = all[i] > slowest ? all[i] : slowest;
		}
		for (size_t i = 0; i < 4; i++) {
			released = released &&
			        after[i].threads <= idle.threads + 1;
		}
		ok = idle.threads <= 17 && released &&
		        s.threads <= idle.threads + 1 && slowest < 2000.0;
		snprintf(g->tag, sizeof g->tag, "resources");
		ev_check(e,
		        "resources_workers_start_when_asked_and_stop_when_idle",
		        0,
		        "at rest 17 threads or fewer; back to the threads at "
		        "rest "
		        "after each of # D L F and after all; every answer "
		        "within "
		        "2 s",
		        ok ? "observed" : "WRONG", ok);
	}
	/* Used hard: the four keys in quick turns, many times over, with the
	 * windows opened and closed between. The workers are made and stopped
	 * again and again; when it is over the program is as it was (threads at
	 * the threads at rest, no handle kept turn after turn, memory within a
	 * bound that the allocator's own growth stays inside: about 1 MiB was
	 * seen for 60 turns, and the growth slows as the turns go on, as an
	 * allocator warming up does and a leak does not; the workers' own heap
	 * is checked to the byte by worker_lifecycle) and answers the next key. */
	{
		ResSample base, end;
		bool alive;

		Sleep(500);
		ok = res_sample(g, &base);
		for (int round = 0; ok && round < 60; round++) {
			pty_send(&g->pr.p, "D", 1);
			Sleep(25);
			pty_send(&g->pr.p, "#", 1);
			Sleep(25);
			pty_send(&g->pr.p, "L", 1);
			Sleep(40);
			pty_send(&g->pr.p, "\x1b", 1);
			Sleep(25);
			pty_send(&g->pr.p, "F", 1);
			Sleep(60);
			pty_send(&g->pr.p, "\x1b", 1);
			Sleep(25);
		}
		Sleep(2500); /* the last answers, the last stops */
		alive = product_wait_ms(&g->pr.p, "READY", g->pr.screen,
		        sizeof g->pr.screen, 5000);
		ok = ok && alive && res_sample(g, &end);
		res_line(&rep, "after_rapid_use", &end);
		ok = ok && end.threads <= idle.threads + 1 &&
		        end.handles <= base.handles + 6 &&
		        (RES_SANITIZED ||
		                end.private_bytes <=
		                        base.private_bytes + 3.0 * 1048576.0);
		ev_check(e, "resources_rapid_use_leaves_nothing_behind", 0,
		        "60 turns of D # L F: the threads at rest, handles "
		        "within 6 "
		        "and memory within 3 MiB of before, still answering",
		        ok ? "observed" : "WRONG", ok);
	}
	ok = res_idle_release(g, &idle, &rep);
	ok = gate_quit(g) && ok;
done:
	if (rep.n) {
		printf("%s", rep.text);
		ev_write_text(e, L"artifacts\\resources.txt", rep.text);
	}
	return gate_first_run_end(g, e, ok, fails);
}

static int scn_resources(Ev *e)
{
	int prerequisite = require_restart_manager(e);
	if (prerequisite) {
		return prerequisite;
	}
	return resources_case(e) ? (g_gate_skips ? 77 : 0) : 1;
}
