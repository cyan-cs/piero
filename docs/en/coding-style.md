[English](../en/coding-style.md) | [日本語](../ja/coding-style.md)

# Coding style

The rules, with MUST / SHOULD / MAY, are in [CODING_RULES.md](../CODING_RULES.md). This page is the short form and does not add to them.

## Language, format and names

Build as ISO C17 without C++. Keep existing platform ABI annotations and
compiler-specific diagnostic intrinsics isolated; ordinary implementation code
uses C17. Make Windows UTF-16/ABI assumptions explicit. Keep invariants, ownership
and control flow clear, and avoid abstractions that only hide a few lines.
`.clang-format` is authoritative: LLVM base, 80 columns, tabs for indentation,
width 8, custom braces and preserved include order. `wf_` names functions,
`Wf` names types, `WF_` names constants. Follow neighboring code for local names.

## Ownership and memory safety

State whether each pointer is borrowed, copied, owned or transferred; bound
arrays/queues and distinguish capacity from initialized length. Check overflow
before allocation/conversion. Release each resource 1 time, including partial
initialization. Never free a live worker's memory after a stop timeout.
Avoid duplicate state and unnecessary allocations; caches are observations,
not authorization. Keep bytes, UTF-16 units, display cells and pixels distinct.

## Win32 and errors

Use wide APIs for paths. Include `windows.h` before dependent SDK headers.
Capture `GetLastError`/HRESULT at the failing call before cleanup. Preserve
failure semantics, partial-success receipts and uncertain publication states.
Handle ownership/identity/ADS/reparse checks must not be replaced by path guesses.
Prefer one cleanup path when it clarifies ownership, without giant generic helpers.

## Comments, performance and testing

Use English comments for reasons, ownership, invariants and API contracts.
Avoid narrating obvious code. Follow existing local `//` and contract `/* */` usage.
Measure before changing an algorithm; avoid unnecessary I/O and event-loop polling.
Do not trade integrity checks for benchmarks. Use meaningful regression/fault tests
for safety changes; keep fixtures owned and disposable. Run relevant tests before
broader regression and record environmental FAIL/SKIP/NOT RUN honestly.
