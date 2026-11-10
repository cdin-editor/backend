# Registry release metadata

Backend's historical tracked Component Release Metadata under
`registry/releases/` is retained as a backward-compatible static Catalog input.
New versioned Component Release Metadata is generated at release time and
uploaded as a GitHub Release Asset according to accepted Spec ADR-0028; do not
add another tracked per-version JSON file. The existing 1.2.0 and 1.2.1
metadata bytes remain immutable.

## Existing source identity

- Component: backend/default
- Package version: 1.2.0
- Source revision: c0a88090460929796bd7cd731966f04115256910
- Catalog status: active
- Component dependencies: Kernel >=1.2.1 <2.0.0; Runtime >=1.0.0-rc.1 <2.0.0
- Provides: org.cdin.runtime-host-api@1.0.0 and org.cdin.frontend-protocol@1.0.0
- Requires: org.cdin.kernel-abi >=1.1.0 <2.0.0

Do not edit the published 1.2.0 document or its Catalog entry in place.

## Backend 1.2.1 artifact candidate

The 1.2.1 release changes delivery, not component ownership or the public ABI:

- Component package: backend/default
- Artifact kind/layout: native-development-package / cmake-prefix-v1
- Linux x86_64: gnu-glibc-2.39 (Ubuntu 24.04)
- Linux arm64: gnu-glibc-2.39 (Ubuntu 24.04 ARM)
- macOS x86_64 and arm64: macos-15
- Windows x86_64: windows-ucrt64

Each artifact records its exact source revision, release URL, filename, byte
count, SHA-256, build recipe, toolchain and runner provenance.

The new metadata removes the Kernel component dependency edge and declares
org.cdin.kernel-abi >=1.1.0 <2.0.0 as an external environment requirement.
Runtime remains a Registry-managed component dependency
(>=1.0.0-rc.2 <2.0.0).

The required non-component environment contracts are:

- org.cdin.kernel-abi, version 1.1.0
- org.m-mdy-m.log-c-api, version 0.1.0
- org.lua.puc-lua-c-api, version 5.4.0 (PUC Lua 5.4)

The CDIN Product Definitions must declare all three environment contracts.
The Composition Root provisions the exact Kernel package and the independently
released logger SDK (each with its own version pin and prefix), plus the
matching PUC Lua 5.4 development package. The Kernel package's own static link
closure also requires the independently versioned logger SDK; this does not
collapse the logger's separate Registry contract or version identity.
Backend continues to provide Runtime Host API 1.0.0 and Frontend Protocol
1.0.0. It does not depend on CDIN or Registry.

## Immutable publication procedure

1. Merge the Backend package implementation and create a new, never-before-used
   SemVer tag on the exact tested commit. Do not retag or mutate the historical
   1.2.1 release or its metadata.
2. The tag-triggered Pipeline workflow builds and verifies all five native
   packages. Every target publishes its archive, SHA-256 sidecar and
   `*.artifact.json` descriptor from the same source commit.
3. After all matrix targets pass, Pipeline downloads every target bundle and
   invokes the Backend-owned `asset-preparation-command` once. The generator
   resolves `<tag>^{commit}`, compares it to the exact source SHA exposed by
   Pipeline, recursively collects the five target descriptors and validates
   every archive filename, URI, target tuple, byte count, digest and checksum
   sidecar. It emits schema 1.2.x with the exact ADR-0028 `publication`
   identity: provider, repository URI, tag, full tag commit and canonical
   metadata asset filename.
4. The metadata output is written under a `release-build-*` directory inside
   the downloaded release asset root. Pipeline's generic assembler includes
   it in the same GitHub Release as `backend-<version>.json` and calculates
   the aggregate `SHA256SUMS` after the metadata bytes exist. The JSON is a
   generated Release Asset, not a committed file under `registry/releases/`.
5. Verify the published Release inventory contains exactly one canonical
   metadata asset, and check its GitHub API digest and size against the
   downloaded bytes. Registry ADR-0028 discovery must verify the publication
   identity before this release can become eligible for dynamic resolution.
6. Only after Registry discovery and Manifest replay are implemented and
   tested should a separate Catalog migration select this dynamic Release
   Source. Until then, keep the existing static Catalog entries unchanged.

The generator requires explicit `--output`; it intentionally has no default
path into the source tree. It refuses to overwrite an existing output, and it
requires the output basename and containing directory to match the canonical
asset convention and Pipeline's release-asset collection contract.

A successful component release does not automatically change the Catalog or
CDIN Product Definitions. Registry remains a generic delivery-plane tool and
never runs Backend build or install scripts.
