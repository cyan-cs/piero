#ifndef WF_SYNRUNS_H
#define WF_SYNRUNS_H

#include <stdint.h>

#include "wf_syntax_abi.h"

/* One class byte per text byte (0 = plain) into runs of one class; plain
 * stretches are not listed. More than `max_runs` runs: WF_SYN_E_LIMIT. The
 * array is malloc'd (NULL when there is none) and the caller frees it. */
uint32_t wf_syn_runs_build(const uint8_t *cls,
        uint32_t len,
        uint32_t max_runs,
        WfSynRun **runs,
        uint32_t *nruns);

/* The class of byte `at` in sorted runs, 0 (plain) when none covers it. */
uint32_t wf_syn_runs_class_at(const WfSynRun *runs, uint32_t n, uint32_t at);

/* Fills out[0..len) with the classes of the bytes [from, from + len) of the
 * text, from sorted runs. */
void wf_syn_runs_fill(const WfSynRun *runs,
        uint32_t n,
        uint32_t from,
        uint32_t len,
        uint8_t *out);

#endif
