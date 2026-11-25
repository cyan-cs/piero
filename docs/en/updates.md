[English](../en/updates.md) | [日本語](../ja/updates.md)

# Updates

The first normal session asks whether stable-release checks may contact GitHub.
The default is Not now. The choice is saved through the normal configuration
writer; automatic checking starts only after successful persistence. A missing,
malformed or future configuration never grants automatic network permission.

Settings > Updates offers automatic checks, Check now, the current version and
the latest result. Change the draft and save it with Ctrl+S. A manual check works
even when automatic checks are off. First-run choices and saved settings use the
same keys:

```ini
[updates]
enabled=false
consent_recorded=true
```

Checks run on a separate worker and do not wait on the UI thread. Automatic
attempts, including failures, are recorded under the user's registry state and
limited to once per 24 hours. Clock rollback or unreadable state defers automatic
checks; manual checks are explicit. No credentials, cookies or file contents are
sent. The only endpoint is
`https://api.github.com/repos/cyan-cs/piero/releases/latest`; redirects are refused.
Requests have connection/read timeouts and a 64 KiB response limit.

Only a strictly parsed newer stable semantic version is announced. Drafts,
prereleases, malformed/oversized replies and older versions are not offered.
Offline, private/no-release and rate-limit responses do not prevent startup.
Disabling checks prevents new automatic requests; an already in-flight request
is canceled between bounded operations. No background polling loop is used.

Downloads and installation are manual. Release metadata or a hash from the same
server is not independent proof of publisher identity. Signed-update verification,
safe binary replacement and rollback are not implemented, so this version neither
downloads nor executes update assets. Inspect the maintainer's
[official Releases](https://github.com/cyan-cs/piero/releases) and signing status.
