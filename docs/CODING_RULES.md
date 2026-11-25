# piero coding rules

This is the single source of truth for how C is written in piero. Other documents link
here and do not repeat it. [`.clang-format`](../.clang-format) decides layout; this file
decides everything else.

**MUST** is a safety, correctness or compatibility rule: a change that breaks it is
rejected. **SHOULD** is the default: deviate only with a reason that can be written in
the commit or the comment. **MAY** is allowed when the stated condition holds.

The aim is the plainest code that is correct, fast enough and safe: the right data
structure, one clear invariant per piece of state, short control flow, strict ownership.
Advanced C is used where it makes that true, not to show that it can be.

## 1. Adopting a change

These come before every other rule.

1. A change MUST say what it improves: a concrete defect fixed, state or special cases
   removed, an ownership or contract made clear, a measured speed-up, or a named
   undefined behaviour, race or data loss removed. Shorter code, fewer branches or
   "modern" syntax are not improvements by themselves.
2. Leaving good code alone is a result. A change whose benefit is not shown MUST NOT be
   made, and a change whose new complexity outweighs its benefit MUST be undone.
3. A change for speed SHOULD be measured before and after on the same input, build and
   machine, several runs, with the spread. A difference inside the noise is not an
   improvement. What was not measured MUST be written as "not measured".
4. A refactoring MUST NOT change what a user can observe: public API and ABI, file
   safety, error classes and failure contracts, configuration and session formats, keys,
   cancellation, crash recovery. A fix that does change behaviour is recorded as a fix.
5. Touch only what the change needs. Layout-only edits are not mixed with a change.
6. After a change, ask: new invariant, hidden owner, longer cache lifetime, new
   dependence on a compiler or CPU, new concurrency risk, harder to test? If the answer is
   yes and the gain is small, keep the old code.

## 2. Language and toolchain

- Code MUST be ISO C17 that builds with clang-cl and MSVC. C23 features and compiler
  extensions MUST NOT be used in portable code; platform annotations
  (`__declspec`, intrinsics) are isolated where they are needed.
  One exception: a compile-time check that only refuses wrong code and changes no
  behaviour MAY use a compiler extension in `src/base/wf_macro.h` when it is fenced by
  `#if defined(__clang__)` and the other compilers get a conforming definition
  (`WF_ARRAY_LEN` refuses a pointer where an array is meant). The check is not
  removed to satisfy this rule: that would lose it.
- Names: `wf_` functions, `Wf` types, `WF_` constants and macros. Local names follow the
  file. `.clang-format` is authoritative and MUST NOT be changed without a stated reason.
- Sizes, counts and indexes are `size_t`; differences of pointers `ptrdiff_t`; stored or
  transmitted widths are fixed-width types. Bytes, UTF-16 units, display cells and
  pixels are different units and MUST NOT be mixed in one variable or expression.
- A header MUST be self-contained. `windows.h` comes before SDK headers that depend on it.

## 3. Types and state

- Fields that describe one thing MUST be one struct, not parallel arrays indexed by one
  number. Parallel arrays (SoA) MAY stay where an access pattern measured or plainly
  shows a layout benefit, and the reason is a comment.
- States that exclude each other SHOULD be an `enum`, not several `bool`s that can
  contradict. Facts that hold independently stay `bool`. Independent options MAY be
  bit flags: unsigned type, named masks, no bit count that depends on `int` width.
- A value with a "none" encoded as a special size or index (zero means one) SHOULD
  be given the ordinary representation, or the special case documented where it is
  declared.
- A tagged union MAY be used for values of different kinds; the tag and the data are
  changed by one function so they cannot disagree.
- An incomplete type MAY hide a module's state when callers must not touch it. Simple
  data MUST NOT be hidden behind an allocation and a pointer without a reason.
- A strong type (a struct around an integer) MAY be used where two different values are
  easily confused and the confusion is dangerous (`WfFileId` against a generation). It
  MUST NOT be applied to every integer.
- `const` SHOULD mark every read-only reference. It says nothing about ownership or
  thread safety.
- `restrict` MAY be used only where non-aliasing is guaranteed by the caller's contract.
- `_Static_assert` SHOULD check the invariants that the compiler can check and a reader
  would otherwise have to trust: sizes of wire formats, table lengths, enum counts.
- Duplicate state MUST NOT be kept. A value that can be derived cheaply SHOULD be derived.

## 4. Control flow

- Failure conditions SHOULD return or `continue` first so that the normal path is not
  nested. With resources held, a single cleanup path is preferred to repeated releases.
- `goto` MAY be used for error handling, shared cleanup, releasing several resources in
  reverse order and removing deep nesting. Jumps MUST go forward, MUST NOT skip an
  initialisation that later code relies on. One exit path is named for the path
  (`cleanup`, `done`); several are named for what each releases (`out_close_dst`).
  A backward `goto` MUST NOT be used to build a loop; use `for` or `while`.
- A condition MUST NOT be constant, repeat a fact already checked, or be unreachable
  (`|| true`, a flag set and never read). Remove the state that needed the test, rather
  than the test alone.
- Calls made for their effect and joined in one condition MUST NOT use `||` or `&&`,
  which skip the rest; call each and combine the results with `|=`.
- Split a function by responsibility, invariant, ownership, state change or failure
  handling, not by line count. Moving complexity into another function without
  reducing it is not a refactoring. A long function that reads straight through is fine.
- An abstraction MUST remove real duplication or clarify a responsibility. A wrapper used
  once, an interface with one implementation, an extension point for a future need, and
  a function pointer where a `switch` is enough MUST NOT be added.
- A function pointer MAY be used for a dispatch table, a callback or a back end, with the
  callee, type, ownership of the context and error propagation stated.

## 5. Memory and ownership

- Every pointer's role MUST be known: owned, borrowed, copied or transferred; who frees
  it; who owns it when the call fails. A comment states it where it is not obvious.
- Sizes MUST be checked for overflow before allocation and arithmetic. Capacity and
  initialised length are different. Every queue, list and buffer that input can grow has
  a limit.
- A resource MUST be released exactly once, including after partial initialisation. Memory
  of a worker that did not stop in time MUST NOT be freed under it.
- A function that frees an object its create function allocated MUST accept `NULL`, as
  `free` does. A function that releases the contents of a struct the caller owns (an
  `init`/`free` pair) MUST leave it empty, so that calling it again is safe; whether it
  accepts `NULL` is written at its declaration, and a declaration that says it does MUST
  keep the promise.
- Do the cheap test before the allocation: a length, a limit or a full queue is decided
  from numbers, before anything is built to be thrown away.
- When the count is known, allocate once. Growing one element at a time MUST NOT be used;
  grow geometrically. Removing many elements from an array is one pass with a read and a
  write position, and keeps the order whenever order has a meaning.
- A function that only needs to look at a list SHOULD be given or build a borrowed view
  (pointer and length) and copy only what it keeps.
- A buffer MAY be reused when who initialises it and when it becomes invalid are stated;
  if reuse needs elaborate bookkeeping, allocate.
- An arena or pool MAY be used when allocation is measured to be the cost, or when one
  lifetime for many objects makes the design simpler. It MUST have an owner, a lifetime,
  a capacity limit, alignment, overflow checks and failure handling, and its effect MUST
  be measured.
- Pointer arithmetic MAY be used for scanning, parsing, serialising and runs of memory.
  It MUST stay inside one array (one past the end allowed, never dereferenced), never on
  `NULL`, respect alignment and aliasing, never use a pointer after `realloc`, and keep
  Unicode boundaries. Pointer arithmetic is not assumed faster than indexing.
- Macros MAY be used for header guards, conditional compilation, constants, X-macros and
  bit masks. A function or `static inline` is preferred for everything else. A macro MUST
  NOT evaluate an argument twice, hide `return`, `goto` or an ownership change, or depend
  on a caller's local name. A statement macro is `do { ... } while (0)`.
- `static inline` MAY be used for small typed helpers; forced inlining MUST NOT be used
  without a measurement.
- An intrusive list MAY be used when node lifetime is clear, changes are frequent and an
  allocation is worth avoiding; a few elements use an array.

## 6. Errors

- A failure MUST NOT be treated as success and an error MUST NOT be swallowed. Where
  safety cannot be shown, the code fails closed.
- Causes stay distinct until they are shown: out of memory (`WF_E_NOMEM`), out of range
  (`WF_E_RANGE`), access denied, cancelled, worker failure. A `bool` MUST NOT carry a
  failure whose reason matters; carry `WfStatus`.
- The Win32 error (`GetLastError`, HRESULT) is read at the failing call, before cleanup
  can overwrite it.
- Input from a file, a pipe or another process MUST be checked for what the writer never
  produces (a raw NUL, a length past the end, a repeated record).

## 7. Windows

These are contracts and are never traded for speed.

- Paths MUST use the wide APIs. File identity (`FileId`), reparse-point handling,
  alternate streams and path checks MUST NOT be replaced by guesses from a path string,
  and the checks are made on the handle that is used (TOCTOU).
- The journal and crash-recovery contracts, safe cancellation and handle ownership MUST
  be kept. Failed or cancelled saves MUST retain the live unsaved buffer.
  Crash recovery is best-effort and MUST NOT be described as lossless.
- Optional DLLs loaded into the process MUST use a full path that is not a link or
  network path, match the hash pinned by the build, and stay open from hashing through
  loading. This rule covers the syntax host and grammar DLLs. External system tools
  run in separate processes and follow their own executable-selection contracts.
  A load of a file that may not be an image MUST suppress the system's "Bad Image"
  box (`SetThreadErrorMode(SEM_FAILCRITICALERRORS)` around it): the box waits for a
  click, and a process with no desktop waits for ever.

## 8. Asynchronous work

- Replaceable observation requests carry a generation: the newest answer is delivered
  whether it succeeds or fails, and stale observations are dropped. An accepted
  file-changing operation MUST retain its completion or partial-result receipt; a newer
  request MUST NOT silently replace it. Cancellation, delivery and result ownership
  are defined, and a stop that times out leaves live worker memory alone.
- Work moved off the UI thread MUST NOT come back to it as a fallback when the worker
  cannot be had; the user is told instead.
- Threads are few. More threads, or more CPU, is not a goal: responsiveness, throughput,
  memory, idle CPU and power are.
- Several calls whose results are combined are each called (see section 4).

## 9. Performance

- The best optimisation is not doing the work: an unneeded computation, check, scan,
  sort, allocation, copy, string conversion, parse, Win32 call, disk read or repaint, or
  a worker that did not need to start. Remove it at the algorithm and data structure level
  before looking at instructions.
- Work that repeats SHOULD be done once for the input that makes it repeat (fold a filter
  once; keep a line's end state as the next line's start; keep match positions while text
  and pattern are the same). A cache MUST be keyed by every input, including a generation
  that changes with the data, and is never an authorisation.
- Data layout follows the access pattern, ownership and lifetime: contiguous where it is
  walked, hot and cold apart when that helps, no pointer chasing on a hot path, row order
  for C arrays. AoS or SoA is not a rule.
- Branch hints, branch-free rewrites, `-O3`, manual unrolling and the like MUST NOT be
  used without a profile that shows the gain; the compiler's instruction selection,
  inlining and simple strength reduction are not redone by hand.
- SIMD intrinsics MAY be used only after comparing with scalar and auto-vectorised code,
  with CPU feature detection and a fallback. Undefined behaviour and unguaranteed
  contracts MUST NOT be introduced for a compiler hint.
- A measurement records machine, input, build, number of runs and spread, and time,
  allocations, peak memory as relevant. An improvement that costs memory or UI
  responsiveness says so.

## 10. Comments

- Comments say why: an ownership rule, an invariant, a failure path, a Windows
  constraint, an API contract. They do not narrate the code, mark sections for their own
  sake, hold a TODO with no owner, or tell that a tool wrote the code.
- A comment is evidence, not proof: the code may no longer do what it says. When a change
  depends on a comment, check the implementation; when they differ, the comment is
  what is corrected.
- Comments are in English and follow the file's `//` and `/* */` use.

## 11. Tests

- A fix MUST come with a test that fails without it, or the reason it cannot be tested is
  written. A fault-injection test says what every refusal point must leave: an error with
  nothing allocated, or the finished result; if a change needs fewer allocations, the test
  is rewritten for both outcomes and MUST NOT be loosened.
- A safety change SHOULD have a mutant that the test kills. A mutation counts only when the
  test exits with failure and prints `FAIL`; anchors ignore layout.
- Tests MUST NOT be disabled or weakened to pass. What was not run, was skipped or failed
  for the environment is reported as such, and nothing that was not run on a real Windows
  machine is called verified on one.
- Run the related tests first, then the whole suite. Build with clang-cl (Debug and
  Release) and MSVC Release; run the sanitizers where they link.

## 12. Review checklist

Before a change: the owner of each pointer, the unit of each number, the worst case of each
loop; the contract it touches (identity, reparse point, journal, trust) and that it is kept;
where its regression test goes and which mutant must fail without it; what is measured and
what is "not measured".

After: fewer special cases and less state, or the reason it was left; no new hidden owner or
invariant; error causes still distinct; `.clang-format` applied to the changed lines;
related tests and mutants run.
