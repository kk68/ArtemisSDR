# Security Policy

## Scope

ArtemisSDR is a hobby / amateur-radio software project maintained by a
single individual (Kosta Kanchev, K0KOZ). It runs on a Windows PC,
communicates with an owned SunSDR2 DX radio over the local network, and
writes small amounts of state to the user's `%AppData%` folder.

Security issues that are in scope:

- Remote code execution via a malformed UDP packet delivered from a
  network adversary (the radio itself, or another machine on the LAN
  pretending to be the radio)
- Privilege escalation from a normal user to SYSTEM via the installer
  or running application
- Buffer overflows / memory-corruption bugs in the native C code
  (`sunsdr.c`, `sunsdr.h`) that are reachable via network input
- Accidental leakage of user secrets (API keys, callsigns, location
  data) through log files, the `ErrorLog.txt`, or network transmissions
- Supply-chain problems with the MSI itself (e.g. tampered asset
  published to the GitHub release page)

Out of scope for "security" (please file as regular issues):

- Bugs, crashes, RF-path problems, audio glitches, DSP weirdness — file
  normally at https://github.com/kk68/ArtemisSDR/issues
- Performance issues, UI complaints, feature requests
- The well-known "Unknown Publisher" warning at install time — this is
  expected for unsigned hobby software; see README "Privacy & network
  activity" section and the release notes

## Supported versions

ArtemisSDR follows rolling releases. Only the latest tagged release
on https://github.com/kk68/ArtemisSDR/releases is supported. Older
releases are kept for historical reference and will not receive
security fixes.

| Version | Supported |
| --- | --- |
| Latest release (v1.0.x) | Yes |
| Anything older | No — please upgrade |

## Reporting a vulnerability

**Please do not open a public GitHub issue for security-sensitive
reports.** Two preferred channels:

1. **GitHub Private Vulnerability Report** (preferred):
   https://github.com/kk68/ArtemisSDR/security/advisories/new
   This creates a private conversation between you and the maintainer
   and gives you control over timing and public disclosure.

2. **Email**: K0KOZ@philibe.com
   Subject line: `[ArtemisSDR Security]` followed by a short summary.
   If you need an encrypted channel, say so in the first message and I
   will send a PGP public key.

Please include:

- A clear description of the issue and why you believe it is a
  security problem
- Steps to reproduce, if you have them
- The ArtemisSDR version (visible in the About dialog or the title
  bar)
- Your Windows version and radio model
- Any relevant log output or captured packet data

## Response expectations

This is a one-person project done on hobby time. Realistic timing:

- Acknowledgement of receipt: within 5 business days
- Initial triage and impact assessment: within 2 weeks
- Fix in a tagged release: varies with severity and complexity;
  critical network-exposed RCE will be prioritised

I will not pay bug bounties (the project has no commercial model), but
I will credit reporters in the release notes of the fix if they want
public acknowledgement.

## Dependencies and upstream

ArtemisSDR inherits substantial code from upstream Thetis, WDSP,
PowerSDR, and a set of NuGet / native third-party libraries (see
`THIRD_PARTY_LICENSES.md`). If a vulnerability originates in upstream
code, I will coordinate with the upstream project where practical and
credit accordingly.

## What this policy does not promise

- No SLA. This is amateur-radio software maintained for fun.
- No warranty of any kind — see `LICENSE` (GPL v2, §11 and §12).
- No code-signing certificate on the MSI. Users verify downloads via
  the SHA-256 hash published with each GitHub release.

Thank you for helping keep ArtemisSDR and its users safe.

73!
Kosta Kanchev, K0KOZ
