[English](../en/security.md) | [日本語](../ja/security.md)

# Security policy

## Supported versions

This policy covers the development preview in this source snapshot; it designates
no stable version as supported. The preview has known limitations, with no
response-time or security guarantee.
See [safety](safety.md) for known limitations and [manual validation](manual-validation.md)
for the remaining real-environment checks.

## Reporting

Report privately through the repository's Security tab (**Report a vulnerability**,
GitHub Private Vulnerability Reporting). No email address or other contact is
published. If the private-reporting option is unavailable, open a public issue
that only asks for a private channel, without any details
of the problem.
Do not put credentials, private filenames, documents or exploit details
in a public issue. Use minimal disposable reproductions and redact diagnostic paths.
No response time is promised.

## Scope

File-operation integrity, recovery, metadata, parsing, terminal output and
decoder isolation are in scope. A passing automated test does not certify
power-loss durability or remove documented races. Preserve evidence and originals;
do not repair the problem by deleting unreviewed state or weakening sandbox gates.
