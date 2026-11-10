# Registry release metadata

Backend publishes immutable Component Release Metadata in registry/releases/.
The existing backend-1.2.0.json identity is immutable and remains the active
Catalog selection while the artifact package candidate is prepared.

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

1. Merge the Backend packaging implementation and create the exact v1.2.1
   tag on the tested commit.
2. The tag-triggered Pipeline builds the five native packages and the source
   archive. Each native runner verifies the extracted package and links the
   external CMake consumer without Backend source paths.
3. Collect all five *.artifact.json descriptors, their archives and SHA-256
   sidecars into one directory.
4. Generate the component metadata from those real assets:

~~~sh
python3 scripts/generate-release-metadata.py --tag v1.2.1 --artifact-dir dist
~~~

The generator rejects missing or duplicate targets, source revision mismatch,
unexpected release URLs, byte-count or digest mismatch, and absent checksum
sidecars. It creates registry/releases/backend-1.2.1.json without placeholders.

1. Validate the document against Registry Data Contract 1.3.0 and the Spec
   Component Release Metadata schema. Publish it as a new immutable identity
   and add a Catalog entry containing the SHA-256 of the exact JSON bytes.
2. Update CDIN TUI/GUI-SDL Product Definitions to select Backend 1.2.1 and
   declare the Kernel ABI, logger C API and Lua 5.4 environment contracts. The
   Composition Root provisions the exact Kernel and logger binary packages
   from their independent dependency pins.
   Resolve, prepare, verify and replay the Manifest before claiming
   artifact-first installation.

A successful component release does not automatically change the Catalog or
CDIN Product Definitions. Registry remains a generic delivery-plane tool and
never runs Backend build or install scripts.
