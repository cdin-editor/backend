# Registry release metadata

Backend publishes its component Release Metadata in `registry/releases/`. The current entry is `backend-1.2.0.json`, with source revision `c0a88090460929796bd7cd731966f04115256910`.

- Component identity: `backend/default`
- Package version: `1.2.0`
- Component dependencies:
  - `kernel/default`: `>=1.2.1 <2.0.0`
  - `runtime/default`: `>=1.0.0-rc.1 <2.0.0`
- Provided contracts:
  - `org.cdin.runtime-host-api` version `1.0.0`
  - `org.cdin.frontend-protocol` version `1.0.0`
- Required contract: `org.cdin.kernel-abi`, constraint `>=1.1.0 <2.0.0`

Registry resolves these declared dependencies and contract relationships for consuming products. Backend does not link to or execute Registry. Publish new immutable metadata for every release; never retarget a published source revision.
