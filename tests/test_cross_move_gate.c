/*
 * What the product may run, in one table (wf_copyjob_allowed). Copy and
 * cross-volume Move take every collision choice; Recycle takes none; nothing
 * outside the enumerations is allowed. The residual risk of the three is in
 * DR-DRAFT-015; this test pins the table so a change to it is deliberate.
 */
#include "xfer/wf_copyjob.h"
#include "wf_test.h"

int main(void)
{
	static const WfCollide all[] = {WF_COLLIDE_STOP, WF_COLLIDE_SKIP,
	        WF_COLLIDE_OVERWRITE, WF_COLLIDE_RENAME};

	for (size_t i = 0; i < sizeof all / sizeof *all; i++) {
		CHECK(wf_copyjob_allowed(WF_CJ_COPY, all[i]));
		CHECK(wf_copyjob_allowed(WF_CJ_MOVE_ACROSS, all[i]));
		CHECK((WF_COLLIDE_STOP == all[i]) ==
		        wf_copyjob_allowed(WF_CJ_RECYCLE, all[i]));
	}
	CHECK(!wf_copyjob_allowed((WfCopyKind)99, WF_COLLIDE_STOP));
	CHECK(!wf_copyjob_allowed(WF_CJ_COPY, (WfCollide)99));
	CHECK(!wf_copyjob_allowed(WF_CJ_MOVE_ACROSS, (WfCollide)99));
	CHECK(!wf_copyjob_allowed(WF_CJ_RECYCLE, (WfCollide)99));

	return TEST_RESULT();
}
