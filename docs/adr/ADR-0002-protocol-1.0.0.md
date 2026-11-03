# ADR-0002: Freeze the shared Frontend Protocol at 1.0.0

- **ID:** FE/BE-ADR-0002
- **Status:** `ACCEPTED FOR THE SUPPLIED SOURCE RELEASE`
- **Date:** 2026-11-04
- **Supersedes:** the Protocol-version portion of Frontend FE-ADR-0001; it does not supersede the Foundation Core API stability decision.

## Context

Frontend Foundation Core API 1.0.0 had previously shipped with a Protocol 0.2.0 codec. The accepted CDIN Spec wire contract 1.0.0 is breaking: `ui.options` requires a typed `options` map and every PaneTree leaf carries `active_view` as text or CBOR null. The Backend and Frontend maintain independent C codecs and must implement the same envelope, schemas, session negotiation and bounds.

## Decision

1. The shared Frontend Protocol wire version is `1.0.0`; envelope `v` is `[1, 0]`.
2. Backend and Frontend source package versions are `1.0.0`; their Protocol constants and version declarations are also `1.0.0`. The Frontend Foundation Core API remains independently versioned at `1.0.0`.
3. The Spec `docs/architecture/protocol-wire.md` remains normative. This ADR's mirrored contract copy is documentation only; no repository-local divergence is allowed.
4. The Backend validator requires `ui.options.options`, validates defined option types/ranges and accepts nullable leaf `active_view`. The Backend Lua projector serializes the model's required options map and emits CBOR null when a leaf has no active view.
5. The Frontend validator implements the same `ui.options` and PaneTree rules. Its session negotiates exact major/minor `1.0`. Both peers must reject malformed required fields and must not silently fall back to Protocol 0.2.0.
6. The Backend currently advertises no optional capabilities. Frontend may maintain the central registry but requests only capabilities an adapter supports and uses only those acknowledged by the Backend.
7. Runtime Host API remains independently versioned at `0.3.0`. The companion Runtime integration patch must advertise Protocol `1.0.0`; until it is applied and tested against a real Runtime checkout, end-to-end Runtime integration is not claimed.
8. This version freeze does not declare GUI-SDL, production TUI, Headless, concrete OS transport, or the complete CDIN application complete.

## Required conformance rules

- A 4-byte unsigned big-endian frame prefix precedes one deterministic-CBOR envelope.
- Envelope fields and message catalogue follow the Spec contract; Protocol 1.0.0 is `[1, 0]`, namespace `core` is mandatory, and envelope sequence is per connection.
- Snapshot/response flush semantics, independent flush-group sequence, ACK bounds, Driver/Observer rules, nullable layout fields and typed options are checked by both implementations.
- Unknown optional fields may be ignored only after structural, canonical-CBOR and bound validation. No unknown core message is silently accepted.
- No backward compatibility with the 0.2.0 wire format is promised.

## Verification boundary

The paired Linux source suites pass: Backend Kernel-independent CTest and Frontend Core/TUI CTest. The complete Backend target requiring installed Kernel and PUC Lua 5.4, the Runtime patch, OS transport, native Windows/macOS matrix, live multi-process integration, fuzz campaigns and production logger installation remain unverified or incomplete.

## References

- CDIN Spec 1.2.0: ADR-0018, ADR-0020 and `docs/architecture/protocol-wire.md`.
- [Shared Protocol contract](../contracts/protocol-1.0.0.md).
- [Implementation status](../status.md).
