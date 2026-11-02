# Security Policy

## Scope

The Backend is a native C11 library that coordinates the Kernel, Runtime,
operating-system services and Frontend connections. Security reports may
include memory-safety defects, unsafe process execution, path or file handling
bugs, resource-exhaustion issues, cross-thread lifetime errors, and violations
of the Kernel or Frontend Protocol boundaries.

## Reporting a Vulnerability

Please use GitHub's private vulnerability reporting for this repository:

<https://github.com/cdin-editor/backend/security/advisories/new>

Do not post security-sensitive details in a public issue or pull request. Include
the affected version or commit, operating system and architecture, build options,
reproduction steps, expected impact, and a minimal proof of concept when safe.

If private vulnerability reporting is unavailable for this repository, contact
the project owner through the contact method listed in the CDIN organization
profile before disclosing details publicly.

## Response and Disclosure

The maintainers will acknowledge a report when they are able to review it and
will coordinate a fix and disclosure with the reporter. No response-time SLA is
promised by this policy. Please allow a reasonable period for triage and repair
before public disclosure.

## Supported Versions

Security fixes are applied to the current development version. Older releases
may not receive fixes; users should upgrade to the latest supported release.
