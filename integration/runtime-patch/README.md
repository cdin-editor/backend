# Runtime Host API 0.3.0 integration patch

The Backend's production C bridge now expects the matching Runtime Host API
`0.3.0` and Frontend Protocol `1.0.0`. The currently inspected public
`cdin-editor/runtime` `main` source still advertises Host API `0.2.0` and
Protocol `0.1.0`; this patch is therefore a required cross-repository step.

## Apply to a clean Runtime checkout

```sh
python3 integration/runtime-patch/apply_runtime_integration.py \
  --runtime-root ../runtime --check-only
python3 integration/runtime-patch/apply_runtime_integration.py \
  --runtime-root ../runtime
cd ../runtime
make test boundary
```

The patcher checks every expected source anchor before writing anything, stages
all changed files beside their destinations, and rolls back already-replaced
files if a write fails. It never commits or pushes. Review the diff before
committing in the Runtime repository.

## Changes

- Pins Host API `0.3.0` and Frontend Protocol `1.0.0`.
- Requires `host.features.document_lines` and validates the optional
  `frontend_events` boolean capability.
- Adds bounded, version-pinned `Client:read_lines()` with exact byte strings.
- Adds `Runtime:read_view_lines()` and `Runtime:handle_frontend_event()` for
  key/text/paste/mouse/resize/focus/position/command events.
- Adds document version/length and active theme/style data to each semantic UI
  snapshot; keeps views/panels within the atomic projection limits (32/16), and clamps the byte scroll anchor when a document shrinks.
- Extends FakeHost and Runtime integration tests for CRLF/LF, NUL, stale
  versions, paste bytes, key dispatch, resize, focus, mouse, commands and scroll-anchor clamping after external document shrink.

The patch has been syntax-checked and its anchors were reviewed against the
read-only Runtime source snapshot. It could not be applied or run against a full
Runtime checkout in this workspace; run the commands above in that checkout
before treating the cross-repository boundary as verified.
