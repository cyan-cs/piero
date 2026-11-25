/* The old-journal analyser on synthetic data only. No real journal is read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "wf_test.h"
#include "xfer/wf_copyjournal_legacy.h"

#define ID1 "00000000deadbeef:0123456789abcdef0123456789abcdef"
#define ID2 "00000000deadbeef:fedcba9876543210fedcba9876543210"
#define P1 "C:\\dest\\.wfcopy-0000000000000001.tmp"
#define P2 "C:\\dest\\.wfcopy-0000000000000002.tmp"

static WfLegacyJournalReport analyze(const char *text)
{
	WfLegacyJournalReport r;

	CHECK(wf_cj_legacy_analyze(text, strlen(text), &r));
	return r;
}

/* A long journal costs n log n, not n squared: 60000 temporaries, every third
 * without its "gone" record, and an intent for each. The counts are exact. */
static void long_journal(void)
{
	enum
	{
		N = 60000
	};
	size_t cap = (size_t)N * 3 * 120, at = 0;
	char *buf = malloc(cap);
	WfLegacyJournalReport r;
	ULONGLONG t0;

	CHECK(NULL != buf);
	for (int k = 0; k < N; k++) {
		char id[64];

		snprintf(id, sizeof id, "%016x:%032x", k, k);
		at += (size_t)snprintf(buf + at, cap - at,
		        "I 100 1d - C:\\dest\\f%d.tmp\n"
		        "C 100 1d %s C:\\dest\\f%d.tmp\n",
		        k, id, k);
		if (k % 3) {
			at += (size_t)snprintf(
			        buf + at, cap - at, "G 100 1d %s -\n", id);
		}
	}
	t0 = GetTickCount64();
	CHECK(wf_cj_legacy_analyze(buf, at, &r));
	printf("info: %d temporaries analysed in %llu ms\n", N,
	        (unsigned long long)(GetTickCount64() - t0));
	CHECK(0 == r.malformed && 0 == r.unresolved_intents);
	CHECK((size_t)(N / 3) == r.unresolved_created);
	CHECK(!r.complete_proven);
	free(buf);
}

int main(void)
{
	WfLegacyJournalReport r;

	/* empty: nothing was recorded, and nothing is "proven" about a history */
	r = analyze("");
	CHECK(r.empty && !r.complete_proven && 0 == r.lines);

	/* a closed history: intent, created, verified, gone */
	r = analyze("I 100 1d 0 -\n"
	            "I 100 1d - " P1 "\n"
	            "C 100 1d " ID1 " " P1 "\n"
	            "V 100 1d " ID1 " -\n"
	            "G 100 1d " ID1 " -\n");
	/* the first line has no real path: "-" as a path, and it never closes */
	CHECK(r.intents == 2 && r.created == 1 && r.gone == 1);

	r = analyze("I 100 1d - " P1 "\n"
	            "C 100 1d " ID1 " " P1 "\n"
	            "V 100 1d " ID1 " -\n"
	            "G 100 1d " ID1 " -\n");
	CHECK(0 == r.malformed && 0 == r.unresolved_created &&
	        0 == r.unresolved_intents && r.complete_proven);

	/* a created temporary that no G closes: not proven, however old */
	r = analyze("I 100 1d - " P1 "\n"
	            "C 100 1d " ID1 " " P1 "\n"
	            "V 100 1d " ID1 " -\n");
	CHECK(1 == r.unresolved_created && !r.complete_proven);

	/* an intent without its creation: the file may or may not exist */
	r = analyze("I 100 1d - " P1 "\n");
	CHECK(1 == r.unresolved_intents && !r.complete_proven);

	/* a G only resolves EARLIER records with its id, and only that id */
	r = analyze("G 100 1d " ID1 " -\n"
	            "I 100 1d - " P1 "\n"
	            "C 100 1d " ID1 " " P1 "\n");
	CHECK(1 == r.unresolved_created && !r.complete_proven);
	r = analyze("I 100 1d - " P1 "\n"
	            "C 100 1d " ID1 " " P1 "\n"
	            "G 100 1d " ID2 " -\n");
	CHECK(1 == r.unresolved_created && !r.complete_proven);

	/* two temporaries, one closed */
	r = analyze("I 100 1d - " P1 "\n"
	            "C 100 1d " ID1 " " P1 "\n"
	            "I 100 1d - " P2 "\n"
	            "C 100 1d " ID2 " " P2 "\n"
	            "G 100 1d " ID1 " -\n");
	CHECK(1 == r.unresolved_created && !r.complete_proven);

	/* damage is never read as success */
	r = analyze("I 100 1d - " P1 "\nnot a record\n");
	CHECK(1 == r.malformed && !r.complete_proven);
	r = analyze("I 100 1d - " P1 "\r\nC 100 1d " ID1 " " P1 "\r\n"
	            "G 100 1d " ID1 " -\r\n");
	CHECK(0 == r.malformed && r.complete_proven); /* CRLF tolerated */
	r = analyze("I 100 1d - " P1 "\n"
	            "C 100 1d " ID1 " " P1 "\n"
	            "G 100 1d " ID1 " -"); /* cut off: no newline */
	CHECK(r.unterminated_tail && !r.complete_proven);
	r = analyze("X 100 1d - " P1 "\n");
	CHECK(1 == r.malformed);
	r = analyze("C zz 1d " ID1 " " P1 "\n");
	CHECK(1 == r.malformed); /* the pid is decimal */
	r = analyze("C 100 1g " ID1 " " P1 "\n");
	CHECK(1 == r.malformed); /* the start time is hex */
	r = analyze("C 100 1d\n");
	CHECK(1 == r.malformed);

	/* an overwrite: the old file is set aside (A), its drop is announced
	 * (D) and its disappearance recorded (G): the records close */
	r = analyze("A 100 1d " ID1 " C:\\dest\\f.txt|C:\\dest\\f.txt.aside\n"
	            "D 100 1d " ID1 " -\n"
	            "G 100 1d " ID1 " -\n");
	CHECK(1 == r.asides && 1 == r.discards && 1 == r.gone &&
	        0 == r.malformed && 0 == r.unresolved_asides &&
	        r.complete_proven);
	/* the discard alone states an intent, not a result */
	r = analyze("A 100 1d " ID1 " C:\\dest\\f.txt|C:\\dest\\f.txt.aside\n"
	            "D 100 1d " ID1 " -\n");
	CHECK(1 == r.unresolved_asides && !r.complete_proven);
	/* an A without its aside, or with two separators, is not a record */
	r = analyze("A 100 1d " ID1 " C:\\dest\\f.txt\n");
	CHECK(1 == r.malformed && !r.complete_proven);
	r = analyze("A 100 1d " ID1 " a|b|c\n");
	CHECK(1 == r.malformed);
	/* V, G and D carry only an id */
	r = analyze("G 100 1d " ID1 " C:\\dest\\f.txt\n");
	CHECK(1 == r.malformed);
	/* a G for a different file does not close the aside */
	r = analyze("A 100 1d " ID1 " C:\\a|C:\\b\n"
	            "G 100 1d " ID2 " -\n");
	CHECK(1 == r.unresolved_asides && !r.complete_proven);

	/* a header-less file of the current format is not an old journal, and
	 * its lines do not parse as one */
	r = analyze("PIERO-COPY-RECEIPTS 2\n");
	CHECK(1 == r.malformed && !r.complete_proven);

	long_journal();

	CHECK(!wf_cj_legacy_analyze(NULL, 1, &r));
	CHECK(!wf_cj_legacy_analyze("x", 1, NULL));
	return TEST_RESULT();
}
