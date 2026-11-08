# Registry release metadata

Backend publishes its component Release Metadata in `registry/releases/`. The
current Catalog entry is `backend-1.2.0.json`, with package version
`1.2.0` and immutable source revision
`c0a88090460929796bd7cd731966f04115256910`.

- Component identity: `backend/default`
- Package version: `1.2.0`
- Catalog status: `active` (eligible for Registry resolution)
- Component dependencies:
  - `kernel/default`: `>=1.2.1 <2.0.0`
  - `runtime/default`: `>=1.0.0-rc.1 <2.0.0`
- Provided contracts:
  - `org.cdin.runtime-host-api` version `1.0.0`
  - `org.cdin.frontend-protocol` version `1.0.0`
- Required contract: `org.cdin.kernel-abi`, constraint
  `>=1.1.0 <2.0.0`

## Package and release status

The CDIN Product Definitions select Backend `1.2.0` as an active integration
package. This is a deliberate Registry-consumable source identity, not a claim
that a GitHub source archive has already been released. The repository has not
published a `v1.2.0` tag or GitHub Release; the Changelog therefore keeps the
work under `Unreleased`. The matching source/package version is still
`1.2.0` in CMake, the Registry Release Metadata and the CDIN Resolved
Manifest. The real CDIN Registry-backed build selected Backend `1.2.0` and
passed its native cross-platform and full-stack sanitizer checks in
[CDIN workflow run 42407924719](https://github.com/cdin-editor/cdin/actions/runs/42407924719).

The `1.1.0` note in the Unreleased history records the intermediate version
bump when the optional trusted extension resolver configuration was introduced.
Subsequent work has advanced the current package identity to `1.2.0`; it does
not mean the current Registry candidate or CMake package is still `1.1.0`.

Registry resolves the declared dependencies and contract relationships for
consuming products. Backend does not link to or execute Registry. Publish new
immutable metadata for every package identity; never retarget a published
source revision or digest.
