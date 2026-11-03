# Specification traceability

This page maps accepted cross-project contracts to the implementation and executable evidence in the delivered Backend source. A source link is not, by itself, proof that a live Runtime/Kernel/Frontend session has passed.

## 1. Source snapshots examined

| Repository | Examined boundary | Result |
|---|---|---|
| `cdin-editor/spec` | Spec 1.1.0 source archive, ADRs, open decisions, architecture, protocol schema and reporting script | Design authority. OD-22/23/24/32 are recorded in ADR-0018/0019 and implementation maps. |
| `cdin-editor/backend` | Supplied Backend archive, C11 source, public headers, CMake/Make, protocol/server, Lua bridge, adapter and tests | Modified source snapshot in this delivery. |
| `cdin-editor/runtime` | Current `main` Host contract/version, application, UI model/view/PaneTree, key conversion, document client, FakeHost and integration tests | Read-only inspection. A fail-closed Runtime patch is included; this workspace could not push or test it against a full checkout. |
| `cdin-editor/kernel` | Current public `include/cdin/kernel.h`, ABI docs, document/notification wrappers and line arithmetic | Read-only inspection. Public ABI is 1.1.0, package 1.2.1. Kernel source is unchanged. |

## 2. Accepted Spec decisions and implementation evidence

| Decision / contract | Spec location | Backend source evidence | Evidence status |
|---|---|---|---|
| OD-22 deterministic CBOR and 4-byte BE framing | ADR-0018; `docs/architecture/protocol-wire.md` | `include/cdin/protocol.h`; `src/protocol/codec.c`; `tests/protocol/test_codec.c` | Codec/frame tests pass; concrete OS transport absent. |
| OD-23 standalone reference codec | ADR-0018 | `CMakeLists.txt` target `cdin_protocol`; `Makefile` `protocol-test`; installed `include/cdin/protocol.h` | Builds/tests without Kernel and Lua. |
| OD-24 one Driver, Observers and FIFO promotion | ADR-0018 permissions model | `src/protocol/server.c`; `tests/protocol/test_server.c` | In-process role, queue, flush and ACK tests pass. |
| OD-32 PUC Lua 5.4 bridge | ADR-0019 | `src/runtime/lua_host*.c`; `CMakeLists.txt` `CDIN_WITH_LUA`; Runtime integration workflow | Bridge source exists. Live build not run because Lua 5.4 development files and installed Kernel package are missing locally. |
| `cdin.ui-model.v1` projection | Spec `docs/architecture/ui-model-v1.md`; Runtime Host API 0.3.0 | `src/runtime/lua_host_protocol.c::lua_ui_publish`, `publish_model_to_client`, `build_layout_message`, `build_view_message`, `build_panel_message`; `src/protocol/server.c::cdin_protocol_publish_snapshot` | Implemented in the Backend source, including `scroll_anchor` as a byte offset and null-safe empty layout. Runtime must publish `styles`, `document_version`, `document_length` and bounded views/panels; companion Runtime patch does this. |
| Inbound Frontend event dispatch | [Protocol wire contract](https://github.com/cdin-editor/spec/blob/main/docs/architecture/protocol-wire.md) | `src/protocol/backend_protocol.c::cdin_backend_frontend_receive`; `src/runtime/lua_host_protocol.c::cdin_lua_host_dispatch_frontend`; `src/runtime/lua_host.c::cdin_lua_host_pump` | Bounded queue and owner-thread dispatch implemented. Runtime handler is supplied by the companion patch and is not present in inspected Runtime `main`. |
| `view.request_lines` and `view.lines` | Spec `protocol-wire.md`; Runtime Host API 0.3.0 | `src/runtime/lua_host_protocol.c::lua_call_read_view_lines` / `encode_view_lines_payload`; `src/runtime/lua_host_documents.c::lua_doc_read_lines`; `src/kernel/adapter_documents.c::cdin_kernel_host_document_read_lines` | Backend/Kernel path implemented with Snapshot pinning, original EOL/NUL/raw-byte preservation, frame-budget accounting and same-version `total_lines`. Runtime client method and event method are in the companion Runtime patch. |
| Headless/replay/fuzz | Spec `docs/testing/conformance.md` and `test-matrix.md` | `tests/headless/headless_frontend.c`; `tests/fuzz/protocol_decode_fuzz.c`; `tests/protocol/*` | Headless and bounded fuzz smoke tests run; not a full frontend app or exhaustive fuzz campaign. |
| Kernel C ABI mapping | [Runtime Host API contract](https://github.com/cdin-editor/runtime/blob/main/docs/contracts/runtime-host-api.md); [Kernel ABI](https://github.com/cdin-editor/kernel/blob/main/docs/abi.md) | `src/kernel/adapter*.c`; only adapter includes the public Kernel header | Kernel ABI source reviewed; live package integration not run locally. |

## 3. Runtime integration patch

The inspected Runtime `main` has Host API `0.2.0` and Frontend Protocol `0.1.0`; this Backend now reports `0.3.0` and `1.0.0`. Runtime uses exact provisional major/minor matching, so these versions are intentionally incompatible until Runtime is updated. The fail-closed patch at `integration/runtime-patch/apply_runtime_integration.py` updates:

- Runtime version descriptor and Host contract to Host API `0.3.0` / Protocol `1.0.0`;
- required `documents.read_lines` and `document_lines` capability validation;
- `DocumentClient:refresh_metadata()` and version-pinned `read_lines()`;
- UI Model bounds (32 views, 16 panels);
- FakeHost semantics for LF/CRLF/CR and byte-preserving line windows;
- UI snapshot document metadata/styles/theme publication;
- `Runtime:handle_frontend_event()` and `Runtime:read_view_lines()`;
- integration tests for text, paste, key/command, resize/focus, stale position and line reads.

The GitHub integration refused branch creation with HTTP 403 and a network checkout was unavailable in this workspace. Therefore this patch is delivered for application to a clean Runtime checkout; no Runtime commit or push is claimed.

## 4. Source and tests for the executable paths

| Path | Source anchors | Expected invariant |
|---|---|---|
| Runtime UI model → Frontend snapshot | `lua_host_protocol.c:660-856`; `lua_host_protocol.c:860-898`; `server.c:407-503` | Per-client bounded, all-or-nothing snapshot ending in `ui.flush(scope="snapshot")`; cursor projection includes the logical byte `scroll_anchor`. |
| Attach/resync snapshot | `backend_protocol.c` attach path; `lua_host.c::cdin_lua_host_publish_latest_ui` | Newly attached client receives the retained latest model. |
| Frontend frame → Runtime | `backend_protocol.c::cdin_backend_frontend_receive`; `lua_host_protocol.c:1234-1325`; `lua_host.c:157-189` | Validated event is dispatched and consumed only on owner thread. |
| View line request → Kernel | `lua_host_protocol.c:1085-1232`; `lua_host_documents.c:86-165`; `adapter_documents.c:160-273` | Exact version, pinned snapshot, bounded complete line prefix, `total_lines`, explicit lease release. |

Line numbers refer to this delivered source tree and should be regenerated after commits or refactors.

## 5. Verification boundary

The local workspace ran Protocol tests, Backend service E2E tests, fresh Kernel/Lua-independent CMake/CTest and a bounded Clang libFuzzer smoke run. It did not build `CDIN_WITH_LUA=ON`, execute real Runtime + Kernel integration or produce macOS/Windows test results. Those remain explicit gates in [1.0.0 readiness](1.0.0-readiness.md).
