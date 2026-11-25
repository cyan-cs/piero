#ifndef WF_EVIM_H
#define WF_EVIM_H

/*
 * Vim's Normal mode beyond single keys: an operator (d, y, c) followed by a
 * motion, Visual mode (v, V), named registers ("a) and macros (qa ... q, @a).
 * The keymap binds only the first key of each; what follows is read here, so
 * the bindings can be changed like any other while the motions stay fixed
 * (they are listed in the help text of the operator).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wf_ekeymap.h"
#include "wf_esession.h"

#define WF_VIM_REGS 26
#define WF_VIM_MACRO_MAX 512 /* events kept in one recording */
#define WF_VIM_DOT_MAX 512   /* events kept for "." */

typedef struct
{
	char wait; /* 0, or what the next key is: 'o' (a motion), '"', 'q', '@', 'g' */
	char op;         /* 'd', 'y' or 'c' while a motion is awaited */
	unsigned count;  /* the count typed before the operator (0: none) */
	unsigned count2; /* the count typed after it */
	bool visual, vline;
	int reg; /* the register chosen with ", or -1 */
	struct
	{
		char *text;
		size_t n;
		bool lines;
	} regs[WF_VIM_REGS];
	int recording; /* the register being recorded into, or -1 */
	struct
	{
		WfEvent *ev;
		size_t n;
	} macro[WF_VIM_REGS];
	bool playing;
	bool macro_over; /* the recording hit WF_VIM_MACRO_MAX: later keys were dropped */
	bool append; /* the register was named in capitals ("A): add to it */
	bool reg_fresh; /* " and its letter were just typed: keep the choice once */
	/* the last change, as the keys that made it, for "." */
	WfEvent *dot;
	size_t dot_n;
	WfEvent *cand; /* the keys of the command being typed */
	size_t cand_n;
	uint64_t cand_gen;
	bool cand_ok;
	bool replaying;
	bool dot_skip; /* this command is not a change to repeat (undo, "." itself) */
} WfVim;

struct WfEditSession;

void wf_vim_init(WfVim *v);
void wf_vim_free(WfVim *v);
/* A paste takes the register chosen with " instead of the clip, once. */
bool wf_vim_paste_register(struct WfEditSession *s);

/* A command of this file: true when `cmd` is one of the new Vim commands. */
bool wf_vim_is_cmd(WfEdCmd cmd);
/* Runs it. `count` is what was typed before it (0: none). */
WfEsResult wf_vim_exec(struct WfEditSession *s, WfEdCmd cmd, unsigned count);
/* Gives the key that follows an operator, a register or a macro name to the
 * state that waits for it. true: the event was consumed; *res says how. */
bool wf_vim_follow(struct WfEditSession *s, const WfEvent *ev, WfEsResult *res);
/* Called with every event while a macro is recorded. */
void wf_vim_record(struct WfEditSession *s, const WfEvent *ev);
/* Motions made while in Visual mode extend the selection. */
bool wf_vim_extends(const WfVim *v);
/* "." repeats the last change: every key is offered before and after it is
 * handled, and a command that ends back in Normal mode having changed the text
 * becomes the one to repeat. */
void wf_vim_dot_before(struct WfEditSession *s, const WfEvent *ev);
void wf_vim_dot_after(struct WfEditSession *s);
/* Esc: leaves Visual mode and drops a half-typed command. */
void wf_vim_cancel(WfVim *v);

#endif
