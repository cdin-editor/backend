> Legacy migration helper: this patcher targets the pre-integration Runtime
> Host API `0.2.0` / Protocol `0.1.0` baseline and emits the historical Host API
> `0.3.0` integration. Do not apply it to Runtime `1.0.0-rc.1` or any tree
> already implementing Host API `1.0.0`; the stable candidate is already aligned.

# Runtime Host API 1.0.0 integration patch

The Backend's production C bridge now expects the matching Runtime Host API
`0.3.0` and Frontend Protocol `1.0.0`. The supplied `runtime.rar` archive was an older pre-integration snapshot
(Host API `0.2.0`, Protocol `0.1.0`). Runtime `0.4.1` already contains the
integration. The patcher now has two safe paths: it applies the fail-closed
source transformation to the known pre-integration snapshot, or verifies a
complete already-integrated Runtime and exits without writing. A partial/newer
unknown state is rejected rather than patched heuristically.

## Verify an already-integrated Runtime (recommended for Runtime 0.4.1)

```sh
python3 integration/runtime-patch/apply_runtime_integration.py \
  --runtime-root ../runtime --check-only
```

When all integration markers are present, the command reports a verified
no-op and writes nothing. It exits non-zero for partial or unknown states.

## Apply to the supplied pre-integration Runtime snapshot

```sh
python3 integration/runtime-patch/apply_runtime_integration.py \
  --runtime-root ../runtime --check-only
python3 integration/runtime-patch/apply_runtime_integration.py \
  --runtime-root ../runtime
cd ../runtime
make test boundary
```

The patcher aligns the descriptor to Frontend Protocol `1.0.0`, changes the
Protocol validator to stable same-major/minimum-minor compatibility, and retains
the legacy `0.2.0` rejection test. It checks every expected source anchor before
writing anything, stages all changed files beside their destinations, and rolls
back already-replaced files if a write fails. It never commits or pushes.

## Changes

- Pins Host API `1.0.0` and Frontend Protocol `1.0.0`.
- Requires `host.features.document_lines` and validates the optional
  `frontend_events` boolean capability.
- Adds bounded, version-pinned `Client:read_lines()` with exact byte strings.
- Adds `Runtime:read_view_lines()` and `Runtime:handle_frontend_event()` for
  key/text/paste/mouse/resize/focus/position/command events.
- Adds document version/length and active theme/style data to each semantic UI
  snapshot; keeps views/panels within the atomic projection limits (32/16), and clamps the byte scroll anchor when a document shrinks.
- Extends FakeHost and Runtime integration tests for CRLF/LF, NUL, stale
  versions, paste bytes, key dispatch, resize, focus, mouse, commands and scroll-anchor clamping after external document shrink.

The patcher was syntax-checked and both paths were exercised: it applied to a
fresh extraction of the supplied pre-integration RAR, then verified the result;
it also recognized Runtime 0.4.1 as already integrated and returned a no-op.
Runtime Lua 5.4.7 unit/integration tests pass (137 / 131 assertions). This does
not prove a live Backend + Lua 5.4 + Kernel round-trip; that remains a separate
CI gate.
