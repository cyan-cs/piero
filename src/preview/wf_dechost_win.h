#ifndef WF_DECHOST_WIN_H
#define WF_DECHOST_WIN_H

#include <windows.h>

/*
 * The decoder host: the body of the sandboxed process. It runs a loop of
 * "read a request from `in`, read the file from the handle the request names,
 * decode it, write the reply to `out`" until `in` reaches end of file.
 *
 * It trusts nothing on its input: the request header is validated before any
 * allocation, the file is read to exactly the size the request states, and a
 * request that breaks the protocol ends the process (the app starts a new
 * host; that is cheaper than guessing what a confused peer meant). A damaged
 * FILE is not a protocol error: it gets a reply with a status.
 *
 * Returns 0 when `in` closed between requests, otherwise a small code for the
 * way it stopped.
 */
int wf_dechost_main(HANDLE in, HANDLE out);

/*
 * If the command line is the decoder host's ("--decoder-host <in> <out>",
 * handle values in hex, as inherited), runs it and stores its exit code in
 * `*code`, returning 1. Otherwise returns 0 and touches nothing, so the
 * normal program can start.
 */
int wf_dechost_from_args(int argc, wchar_t **argv, int *code);

#define WF_DECHOST_ARG L"--decoder-host"

#endif
