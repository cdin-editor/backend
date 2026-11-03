#!/usr/bin/env python3
"""Apply the CDIN Frontend-boundary integration to a cdin-editor/runtime checkout.

The script is intentionally fail-closed: it verifies every source anchor before
writing any file, then applies all edits as one local working-tree operation.
It does not commit or push. Run it from any directory with --runtime-root.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import os
import sys
import tempfile


class PatchError(RuntimeError):
    pass


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise PatchError(f"{label}: expected one source anchor, found {count}")
    return text.replace(old, new, 1)


def insert_before(text: str, anchor: str, insertion: str, label: str) -> str:
    return replace_once(text, anchor, insertion + anchor, label)


def load(root: Path, relative: str) -> str:
    path = root / relative
    if not path.is_file():
        raise PatchError(f"missing required file: {relative}")
    return path.read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-root", required=True, type=Path,
                        help="path to a clean cdin-editor/runtime checkout")
    parser.add_argument("--check-only", action="store_true",
                        help="validate every source anchor and print the file plan without writing")
    args = parser.parse_args()
    root = args.runtime_root.resolve()
    if not (root / "lua/cdin/runtime.lua").is_file() or not (root / "AGENTS.md").is_file():
        print("error: --runtime-root is not a cdin-editor/runtime checkout", file=sys.stderr)
        return 2

    changed: dict[str, str] = {}
    try:
        # 1. Pin the Host API and Frontend Protocol versions as one boundary change.
        path = "lua/cdin/runtime/core/version.lua"
        text = load(root, path)
        text = replace_once(text,
            "host_api = { major = 0, minor = 2, patch = 0 },",
            "host_api = { major = 0, minor = 3, patch = 0 },",
            "Runtime Host API version")
        text = replace_once(text,
            "frontend_protocol = { major = 0, minor = 1, patch = 0 },",
            "frontend_protocol = { major = 0, minor = 2, patch = 0 },",
            "Frontend Protocol version")
        text = replace_once(text,
            "-- Host API 0.2 adds the Kernel-backed logging descriptor (`info.logging`).",
            "-- Host API 0.3 adds the document line-window service and Frontend event boundary.",
            "Host API version comment")
        changed[path] = text

        # 2. Require line-window reads and validate the optional frontend event feature.
        path = "lua/cdin/runtime/host/contract.lua"
        text = load(root, path)
        text = replace_once(text,
            '    "documents",\n    "notifications",',
            '    "documents",\n    "document_lines",\n    "notifications",',
            "required document_lines feature")
        text = replace_once(text,
            '  for _, name in ipairs(required_host_features) do\n',
            '  if type(features.frontend_events) ~= "boolean" then\n'
            '    errors.raise("HOST_API_MISMATCH", "frontend_events must be reported as a boolean capability")\n'
            '  end\n'
            '  for _, name in ipairs(required_host_features) do\n',
            "frontend_events feature descriptor")
        text = replace_once(text,
            '    "create", "close", "read", "replace", "replace_batch", "undo", "redo",',
            '    "create", "close", "read", "read_lines", "replace", "replace_batch", "undo", "redo",',
            "documents.read_lines Host method")
        text = text.replace(
            'Backend advertised regex under Kernel ABI 1.0.0, where it is unsupported',
            'Backend advertised regex under Kernel ABI 1.1.0, where it is unsupported')
        text = text.replace(
            'The Frontend Protocol is still provisional. Backend negotiates transport;',
            'Frontend Protocol 0.2 adds model projection and inbound event contracts. Backend negotiates transport;')
        changed[path] = text

        # 3. Add a bounded, version-pinned line-window method to the document client.
        path = "lua/cdin/runtime/document/client.lua"
        text = load(root, path)
        anchor = "-- Every write uses the last observed version unless the caller supplies one.\n"
        refresh_and_read_lines = r'''-- Refresh only version/length metadata after an external Kernel mutation.
-- The zero-byte read is the Host API's explicit latest-version resync operation.
function Client:refresh_metadata()
  self:_assert_open()
  local result = check_result(self.host.documents:read(self.handle, {
    start = 0, length = 0, expected_version = nil,
  }), "metadata refresh")
  if type(result.version) ~= "number" or result.version < self.version
      or type(result.total_length) ~= "number" or result.total_length < 0 then
    errors.raise("INTERNAL", "host metadata refresh returned an invalid version/length")
  end
  local changed = result.version ~= self.version or result.total_length ~= self.length
  self.version = result.version
  self.length = result.total_length
  return changed, { version = self.version, length = self.length }
end

-- Read a bounded line window without retaining document bytes on the client.
-- Each returned line is an exact byte string, including its original EOL bytes.
function Client:read_lines(first_line, line_count, expected_version)
  self:_assert_open()
  if type(first_line) ~= "number" or first_line < 0 or first_line % 1 ~= 0 then
    errors.raise("INVALID_ARGUMENT", "first_line must be a non-negative integer")
  end
  if type(line_count) ~= "number" or line_count < 1 or line_count > 4096 or line_count % 1 ~= 0 then
    errors.raise("INVALID_ARGUMENT", "line_count must be an integer from 1 to 4096")
  end
  local pinned_version = expected_version == nil and self.version or expected_version
  if type(pinned_version) ~= "number" or pinned_version < 0 or pinned_version % 1 ~= 0 then
    errors.raise("INVALID_ARGUMENT", "expected_version must be a non-negative integer")
  end
  local result = check_result(self.host.documents:read_lines(self.handle, {
    first_line = first_line,
    line_count = line_count,
    expected_version = pinned_version,
  }), "read_lines")
  if type(result.version) ~= "number" or result.version ~= pinned_version
      or result.first_line ~= first_line
      or type(result.total_lines) ~= "number" or result.total_lines < 1
      or first_line >= result.total_lines
      or type(result.lines) ~= "table" or #result.lines == 0
      or #result.lines > math.min(line_count, result.total_lines - first_line) then
    errors.raise("INTERNAL", "host document read_lines result violates the documented shape")
  end
  for index, bytes in ipairs(result.lines) do
    if type(bytes) ~= "string" then
      errors.raise("INTERNAL", "host document read_lines returned a non-byte-string line", { index = index })
    end
  end
  self.version = result.version
  return result
end

'''
        text = insert_before(text, anchor, refresh_and_read_lines, "Client.read_lines insertion")
        changed[path] = text

        # 4. Bound the semantic model so a full projection always fits one default queue batch.
        path = "lua/cdin/runtime/presentation/ui/model.lua"
        text = load(root, path)
        text = replace_once(text,
            'local Model = {}\nModel.__index = Model\n',
            'local Model = {}\nModel.__index = Model\n\n'
            '-- These limits match one atomic Frontend Protocol projection batch.\n'
            'Model.MAX_VIEWS = 32\nModel.MAX_PANELS = 16\n',
            "UI model projection bounds")
        text = replace_once(text,
            '  if options.split_with ~= nil and not self.views[options.split_with] then',
            '  local view_count = 0\n'
            '  for _ in pairs(self.views) do view_count = view_count + 1 end\n'
            '  if view_count >= Model.MAX_VIEWS then\n'
            '    errors.raise("LIMIT_EXCEEDED", "UI model view limit reached", { limit = Model.MAX_VIEWS })\n'
            '  end\n'
            '  if options.split_with ~= nil and not self.views[options.split_with] then',
            "UI model view limit")
        text = replace_once(text,
            'function Model:set_panel(name, panel)\n  self.panels[name] = panel and tablex.copy(panel) or nil\n  self:_touch()\nend',
            'function Model:set_panel(name, panel)\n'
            '  if type(name) ~= "string" or name == "" then\n'
            '    errors.raise("INVALID_ARGUMENT", "panel name is required")\n'
            '  end\n'
            '  if panel ~= nil and self.panels[name] == nil then\n'
            '    local panel_count = 0\n'
            '    for _ in pairs(self.panels) do panel_count = panel_count + 1 end\n'
            '    if panel_count >= Model.MAX_PANELS then\n'
            '      errors.raise("LIMIT_EXCEEDED", "UI model panel limit reached", { limit = Model.MAX_PANELS })\n'
            '    end\n'
            '  end\n'
            '  self.panels[name] = panel and tablex.copy(panel) or nil\n'
            '  self:_touch()\n'
            'end',
            "UI model panel limit")
        changed[path] = text

        # 5. Keep the byte-based scroll anchor inside the refreshed document range.
        path = "lua/cdin/runtime/presentation/ui/view.lua"
        text = load(root, path)
        text = replace_once(text,
            'function View:clamp_selection(length)\n'
            '  local anchor = math.min(self.selection.anchor, length)\n'
            '  local caret = math.min(self.selection.caret, length)\n'
            '  self:set_selection(anchor, caret)\n'
            'end',
            'function View:clamp_selection(length)\n'
            '  local anchor = math.min(self.selection.anchor, length)\n'
            '  local caret = math.min(self.selection.caret, length)\n'
            '  self:set_selection(anchor, caret)\n'
            '  self.scroll_anchor = math.min(self.scroll_anchor, length)\n'
            'end',
            "clamp scroll anchor with view positions")
        changed[path] = text

        # 6. Align FakeHost with Host API 0.3.0 and mirror the Kernel's byte/EOL line rules.
        path = "tests/support/fake_host.lua"
        text = load(root, path)
        text = replace_once(text,
            'host_api = options.host_api or { major = 0, minor = 2, patch = 0 },',
            'host_api = options.host_api or { major = 0, minor = 3, patch = 0 },',
            "FakeHost Host API version")
        text = replace_once(text,
            'frontend_protocol = options.frontend_protocol or { major = 0, minor = 1, patch = 0 },',
            'frontend_protocol = options.frontend_protocol or { major = 0, minor = 2, patch = 0 },',
            "FakeHost Frontend Protocol version")
        text = replace_once(text,
            'features = options.features or { documents = true, notifications = true, snapshots = true, transactions = true, batch_edits = true, literal_search = true, ui_model = true, logging = true },',
            'features = options.features or { documents = true, document_lines = true, frontend_events = options.frontend_events ~= false, notifications = true, snapshots = true, transactions = true, batch_edits = true, literal_search = true, ui_model = true, logging = true },',
            "FakeHost feature descriptor")
        line_service = r'''  function service:read_lines(handle, request)
    local document = get_doc(handle)
    check_version(document, request.expected_version)
    local first_line = request.first_line
    local line_count = request.line_count
    local max_bytes = request.max_bytes or 16384
    if type(first_line) ~= "number" or first_line < 0 or first_line % 1 ~= 0
        or type(line_count) ~= "number" or line_count < 1 or line_count > 4096 or line_count % 1 ~= 0
        or type(max_bytes) ~= "number" or max_bytes < 1 or max_bytes % 1 ~= 0 then
      errors.raise("INVALID_ARGUMENT", "invalid line-window request")
    end

    -- The Kernel treats CRLF as one terminator and also recognizes lone CR.
    -- Starts are 1-based here to match Lua string.sub; the public API stays 0-based.
    local starts = { 1 }
    local cursor = 1
    while cursor <= #document.bytes do
      local byte = document.bytes:byte(cursor)
      if byte == 13 then
        if cursor < #document.bytes and document.bytes:byte(cursor + 1) == 10 then
          cursor = cursor + 1
        end
        starts[#starts + 1] = cursor + 1
      elseif byte == 10 then
        starts[#starts + 1] = cursor + 1
      end
      cursor = cursor + 1
    end

    local total_lines = #starts
    if first_line >= total_lines then
      errors.raise("OUT_OF_RANGE", "first_line is past the end of the document", {
        requested = first_line, length = total_lines,
      })
    end

    local lines, used = {}, 0
    local last = math.min(total_lines, first_line + line_count)
    for line_index = first_line + 1, last do
      local start_offset = starts[line_index]
      local finish_offset = starts[line_index + 1] and (starts[line_index + 1] - 1) or #document.bytes
      local bytes = document.bytes:sub(start_offset, finish_offset)
      if used + #bytes > max_bytes then
        if #lines == 0 then
          errors.raise("LIMIT_EXCEEDED", "first requested line exceeds max_bytes", { required = #bytes })
        end
        break
      end
      lines[#lines + 1] = bytes
      used = used + #bytes
    end
    return {
      version = document.version,
      first_line = first_line,
      total_lines = total_lines,
      lines = lines,
    }
  end

'''
        text = insert_before(text, '  local function mutate(document, request, kind, new_bytes, blob)\n',
                             line_service, "FakeHost.read_lines insertion")
        changed[path] = text

        # 7. Bring Runtime startup, UI projection metadata and inbound events onto the new Host contract.
        path = "lua/cdin/runtime/application.lua"
        text = load(root, path)
        text = replace_once(text,
            '    if self.host.ui.publish then\n      self.host.ui:publish(self.ui:snapshot())\n    end',
            '    self:_publish_ui()',
            "Runtime startup UI publication")
        old_publish = '''function Runtime:_publish_ui()
  local snapshot = self.ui:snapshot()
  local failures = self.events:emit("ui.model_changed", Table.copy(snapshot))
  for _, failure in ipairs(failures) do
    self.log:error(failure.message, { code = failure.code, event = "ui.model_changed" })
  end
  return self.host.ui:publish(snapshot)
end'''
        new_publish = '''function Runtime:_publish_ui()
  local snapshot = self.ui:snapshot()
  -- The Frontend renders against an exact document version. Keep only metadata
  -- here; canonical bytes remain owned by the Kernel.
  for _, view_snapshot in pairs(snapshot.views) do
    local document = self.documents.documents[view_snapshot.document_id]
    if not document or document.state ~= "open" then
      errors.raise("INVALID_HANDLE", "UI view references a document that is not open", {
        view_id = view_snapshot.id,
      })
    end
    view_snapshot.document_version = document.version
    view_snapshot.document_length = document.length
  end
  snapshot.theme = self.themes.active
  snapshot.styles = self.themes:snapshot().styles

  local failures = self.events:emit("ui.model_changed", Table.copy(snapshot))
  for _, failure in ipairs(failures) do
    self.log:error(failure.message, { code = failure.code, event = "ui.model_changed" })
  end
  return self.host.ui:publish(snapshot)
end'''
        text = replace_once(text, old_publish, new_publish, "Runtime UI projection metadata")
        frontend_methods = r'''-- Emit non-document frontend events on the Runtime owner thread. Frontend
-- geometry remains frontend-owned; resize and mouse coordinates are not
-- converted into document offsets here.
function Runtime:_emit_frontend_event(name, payload)
  local failures = self.events:emit(name, payload)
  for _, failure in ipairs(failures) do
    self.log:error(failure.message, { code = failure.code, event = name })
  end
  return #failures == 0
end

function Runtime:_insert_frontend_bytes(bytes)
  if type(bytes) ~= "string" then
    errors.raise("INVALID_ARGUMENT", "frontend text payload must be a byte string")
  end
  local view = self:focused_view()
  local document = self:focused_document()
  if not view or not document then
    errors.raise("INVALID_STATE", "text input requires a focused document view")
  end
  local result = view:replace_selection(document, bytes, {
    expected_version = document.version,
  })
  self:_publish_ui()
  return { status = "inserted", version = result.version, byte_count = #bytes }
end

-- Version-pinned line reads are a pull service. The returned strings are
-- transient exact bytes; this method never stores them on Runtime or View.
function Runtime:read_view_lines(view_id, first_line, line_count, expected_version)
  self:_assert_running()
  if type(view_id) ~= "string" or view_id == "" then
    errors.raise("INVALID_ARGUMENT", "view_id is required")
  end
  local view = self.ui.views[view_id]
  if not view then errors.raise("NOT_FOUND", "view does not exist", { view_id = view_id }) end
  local document = self.documents:get(view.document_id)
  local changed, previous
  previous = { version = document.version, length = document.length }
  changed = document:refresh_metadata()
  if changed then
    -- External mutations may not have reached the notification pump yet. We
    -- cannot infer their edit sequence, so clamp offsets and invalidate all
    -- visible content at the newly observed version instead of guessing.
    self:_apply_document_edits(document.id, nil, nil, document.length)
    self:_publish_ui()
    self.log:debug("document metadata refreshed for line request", {
      document_id = document.id, previous_version = previous.version,
      current_version = document.version, length = document.length,
    })
  end
  return document:read_lines(first_line, line_count, expected_version)
end

-- Called only by the Backend's Lua owner-thread dispatcher. Raw key policy
-- remains in Runtime; frontends do not resolve key bindings or hit-test text.
function Runtime:handle_frontend_event(event)
  self:_assert_running()
  if type(event) ~= "table" or type(event.type) ~= "string" then
    errors.raise("INVALID_ARGUMENT", "frontend event must have a type")
  end

  local event_type = event.type
  if event_type == "input.key" then
    if event.action == "release" then return { status = "ignored", reason = "key-release" } end
    if event.action ~= "press" and event.action ~= "repeat" then
      errors.raise("INVALID_ARGUMENT", "unsupported key action", { action = tostring(event.action) })
    end
    return self:handle_key({ kind = "key_down", key = event.key, modifiers = event.modifiers })
  elseif event_type == "input.text" then
    return self:_insert_frontend_bytes(event.text)
  elseif event_type == "input.paste" then
    return self:_insert_frontend_bytes(event.bytes)
  elseif event_type == "input.mouse" then
    if event.target ~= nil then
      if type(event.target) ~= "string" or not self.ui.views[event.target] then
        errors.raise("NOT_FOUND", "mouse target view does not exist", { view_id = event.target })
      end
      if self.ui.focus ~= event.target then
        self.ui:set_focus(event.target)
        self:_publish_ui()
      end
    end
    self:_emit_frontend_event("frontend.mouse", event)
    return { status = "observed" }
  elseif event_type == "view.set_position" then
    if type(event.view_id) ~= "string" then errors.raise("INVALID_ARGUMENT", "view_id is required") end
    local view = self.ui.views[event.view_id]
    if not view then errors.raise("NOT_FOUND", "view does not exist", { view_id = event.view_id }) end
    local document = self.documents:get(view.document_id)
    -- Refresh without pinning to the stale client version. External Kernel edits
    -- must invalidate view state before the Frontend's byte offsets are accepted.
    local changed = document:refresh_metadata()
    if changed then
      self:_apply_document_edits(document.id, nil, nil, document.length)
      self:_publish_ui()
    end
    if type(event.version) ~= "number" or event.version ~= document.version then
      errors.raise("STALE_VERSION", "view position uses a stale document version", {
        expected = event.version, current = document.version,
      })
    end
    local anchor, caret = event.anchor, event.caret
    if type(anchor) ~= "number" or type(caret) ~= "number"
        or anchor < 0 or caret < 0 or anchor % 1 ~= 0 or caret % 1 ~= 0
        or anchor > document.length or caret > document.length then
      errors.raise("OUT_OF_RANGE", "view position is outside the document", {
        length = document.length,
      })
    end
    self.ui:set_focus(event.view_id)
    view:set_selection(anchor, caret)
    self:_publish_ui()
    return { status = "position-set", version = document.version }
  elseif event_type == "request.command" then
    local ok, result = self:dispatch(event.name, event.arguments or {})
    if not ok then
      errors.raise("NOT_FOUND", "frontend command is not registered or cannot execute", {
        command = tostring(event.name),
      })
    end
    return { status = "executed", result = result }
  elseif event_type == "window.resize" then
    self:_emit_frontend_event("frontend.window_resized", event)
    return { status = "observed" }
  elseif event_type == "focus" then
    self:_emit_frontend_event("frontend.focus_changed", event)
    return { status = "observed" }
  end

  errors.raise("UNSUPPORTED", "frontend event type is not supported by Runtime", {
    event_type = event_type,
  })
end

'''
        text = insert_before(text, 'function Runtime:publish_ui()\n', frontend_methods,
                             "Runtime frontend dispatch API")
        changed[path] = text

        # 8. Extend Runtime integration coverage and update version-negotiation expectations.
        path = "tests/integration/run.lua"
        text = load(root, path)
        line_tests = r'''-- Version-pinned line service preserves CRLF, LF, NUL and exact document bytes.
local line_document = runtime:open_document("one\r\ntwo\nlast\0")
local line_client = runtime.documents:get(line_document.id)
local line_window = line_client:read_lines(0, 8, 0)
A.equal(line_window.version, 0)
A.equal(line_window.total_lines, 3)
A.equal(line_window.lines[1], "one\r\n")
A.equal(line_window.lines[2], "two\n")
A.equal(line_window.lines[3], "last\0")
A.error_code(function() line_client:read_lines(0, 1, 1) end, "STALE_VERSION")
local line_view = runtime:create_view(line_document.id)
line_view.scroll_anchor = line_document.length
local prior_line_version = line_document.version
host.documents:replace(line_document.handle, {
  expected_version = prior_line_version, start = 0, finish = line_document.length, bytes = "x",
  undo_blob_before = "", undo_blob_after = "",
})
A.error_code(function() runtime:read_view_lines(line_view.id, 0, 1, prior_line_version) end,
  "STALE_VERSION")
A.equal(line_document.version, prior_line_version + 1)
A.equal(line_document.length, 1)
A.equal(line_view.scroll_anchor, 1)
runtime:close_document(line_document.id)

'''
        text = insert_before(text, '-- Snapshots remain version-pinned while live document edits continue.\n',
                             line_tests, "Runtime line read integration test")
        frontend_tests = r'''-- The Frontend event path reaches Runtime on its owner thread and preserves paste bytes.
local event_document = runtime:open_document("abc")
local event_view = runtime:create_view(event_document.id)
local initial_position_event = runtime:handle_frontend_event({
  type = "view.set_position", view_id = event_view.id, version = event_document.version, anchor = 1, caret = 1,
})
A.equal(initial_position_event.status, "position-set")
local text_event = runtime:handle_frontend_event({ type = "input.text", text = "Q" })
A.equal(text_event.status, "inserted")
local after_text = event_document:read(0, 16); A.equal(after_text, "aQbc")
local paste_event = runtime:handle_frontend_event({ type = "input.paste", bytes = "\0P" })
A.equal(paste_event.status, "inserted")
local after_paste = event_document:read(0, 16); A.equal(after_paste, "aQ\0Pbc")
local position_event = runtime:handle_frontend_event({
  type = "view.set_position", view_id = event_view.id, version = event_document.version, anchor = 0, caret = 1,
})
A.equal(position_event.status, "position-set")
A.error_code(function()
  runtime:handle_frontend_event({
    type = "view.set_position", view_id = event_view.id, version = 0, anchor = 0, caret = 1,
  })
end, "STALE_VERSION")
local key_event = runtime:handle_frontend_event({
  type = "input.key", key = "e", action = "press", modifiers = { "ctrl" },
})
A.equal(key_event.status, "executed")
local after_key = event_document:read(0, 16); A.equal(after_key, "ZQ\0Pbc")
local command_event = runtime:handle_frontend_event({ type = "request.command", name = "document.undo", arguments = {} })
A.equal(command_event.status, "executed")
local resized_width
runtime.events:on("frontend.window_resized", function(payload) resized_width = payload.width end)
runtime:handle_frontend_event({ type = "window.resize", width = 80, height = 24, units = "cells" })
A.equal(resized_width, 80)
runtime:handle_frontend_event({ type = "focus", focused = false })
runtime:handle_frontend_event({
  type = "input.mouse", action = "press", button = "left", modifiers = {}, target = event_view.id,
  position = { x = 2, y = 1 },
})
runtime:close_document(event_document.id)

'''
        text = insert_before(text,
            'local search = document:search("Z", { mode = "literal" }); A.equal(search.count, 1)\n',
            frontend_tests, "Runtime frontend event integration test")
        # Tests that used the old version as the expected current version must now exercise 0.2.x rejection.
        text = replace_once(text,
            'host_api = { major = 0, minor = 3, patch = 0 }',
            'host_api = { major = 0, minor = 2, patch = 0 }',
            "old Host API rejection test")
        text = replace_once(text,
            'frontend_protocol = { major = 1, minor = 0, patch = 0 }',
            'frontend_protocol = { major = 0, minor = 2, patch = 0 }',
            "old Frontend Protocol rejection test")
        text = replace_once(text,
            'local incomplete_host = FakeHost.new(); incomplete_host.documents.read = nil',
            'local incomplete_host = FakeHost.new(); incomplete_host.documents.read_lines = nil',
            "required line service negative test")
        changed[path] = text

        # The logging suite is loaded separately by tests/integration/run.lua.
        # Keep its startup metadata assertion aligned with Host API 0.3.0.
        path = "tests/integration/logging_tests.lua"
        text = load(root, path)
        text = replace_once(text,
            'A.equal(started.fields.host_api, "0.2.0"); A.equal(started.fields.kernel_abi, "1.1.0")',
            'A.equal(started.fields.host_api, "0.3.0"); A.equal(started.fields.kernel_abi, "1.1.0")',
            "logging test Host API version")
        changed[path] = text

        # 9. Update the normative Runtime Host API contract.
        path = "docs/contracts/runtime-host-api.md"
        text = load(root, path)
        text = replace_once(text, "# Runtime Host API 0.2.0", "# Runtime Host API 0.3.0",
                            "Runtime Host API contract version")
        text = replace_once(text,
            '  host_api = { major = 0, minor = 2, patch = 0 },',
            '  host_api = { major = 0, minor = 3, patch = 0 },',
            "Host API descriptor version")
        text = replace_once(text,
            '  frontend_protocol = { major = 0, minor = 1, patch = 0 },',
            '  frontend_protocol = { major = 0, minor = 2, patch = 0 },',
            "Frontend Protocol descriptor version")
        text = replace_once(text,
            '    ui_model = true, logging = true,',
            '    ui_model = true, document_lines = true, frontend_events = true, logging = true,',
            "Host API feature list")
        text = replace_once(text,
            '- `read(handle, { start, length, expected_version? }) -> { bytes, version, total_length, flags? }`',
            '- `read(handle, { start, length, expected_version? }) -> { bytes, version, total_length, flags? }`\n'
            '- `read_lines(handle, { first_line, line_count, expected_version, max_bytes? }) -> { version, first_line, total_lines, lines: array<byte-string> }`',
            "Host API line-window method")
        text = replace_once(text,
            'Reads\nreturn caller-owned transient strings. `DocumentClient` must not store those\nstrings on itself. Snapshot handles are pinned leases and must be released.',
            'Reads return caller-owned transient strings. `read_lines` returns a bounded, version-pinned\n'
            'window; each line is an exact byte string including its original EOL bytes. The Backend\n'
            'may return a complete prefix when `max_bytes` is reached; it never splits a line.\n'
            '`DocumentClient` must not store read results on itself. Snapshot handles are pinned\n'
            'leases and must be released.',
            "Host API read ownership and line semantics")
        text = replace_once(text,
            'The model is serializable semantic state. Backend translates it to Frontend\nProtocol messages; Runtime does not know which frontends are attached and does\nnot draw. The current model schema is `cdin.ui-model.v1` and remains provisional.',
            'The model is serializable semantic state. Backend validates it and projects it to\n'
            'Frontend Protocol 1.0.0 messages on the Lua owner thread. Every publication is a\n'
            'complete snapshot ending in one `ui.flush`; Frontends replace projected model state\n'
            'atomically at that boundary. Runtime does not know which frontends are attached and\n'
            'does not draw. `view.request_lines` is answered from the Kernel through `read_lines`.',
            "Host API UI projection semantics")
        changed[path] = text

        # 10. Document the enriched view contract and full-snapshot replacement semantics.
        path = "docs/contracts/runtime-types.md"
        text = load(root, path)
        text = replace_once(text,
            '  revision = 3,\n}',
            '  revision = 3,\n  document_version = 12,\n  document_length = 512,\n}',
            "Runtime UI view metadata example")
        text = replace_once(text,
            'The published model uses `schema = "cdin.ui-model.v1"`, a monotonically\nincreasing `revision`, view snapshots, logical layout, focus, panels, prompt,\nstatus data and a theme identifier.',
            'The published model uses `schema = "cdin.ui-model.v1"`, a monotonically\n'
            'increasing `revision`, view snapshots, logical layout, focus, panels, prompt,\n'
            'status data, the active theme style map, and per-view `document_version` plus\n'
            '`document_length`. The protocol projection is bounded to 32 views and 16 panels\n'
            'per snapshot so the message group fits the default bounded output queue.',
            "Runtime UI model version and bounds")
        changed[path] = text

        # 11. Clarify the architecture's now-implemented bridge and its unverified integration gate.
        path = "docs/architecture/overview.md"
        text = load(root, path)
        text = replace_once(text,
            'The Lua modules and simulated-Host tests are present. The production Backend\nadapter, live Kernel integration and Frontend Protocol connection are not yet\nincluded, so the architecture above describes the intended connection between\ncomponents, not a fully integrated editor today.',
            'The Backend bridge now provides a bounded `cdin.ui-model.v1` projection, owner-thread\n'
            'inbound event dispatch, and version-pinned line-window reads through the Host API.\n'
            'The production Kernel adapter and Frontend Protocol implementation are present in\n'
            'the companion Backend source snapshot. This Runtime patch and FakeHost tests verify\n'
            'the Lua side; a live Lua 5.4 + Kernel + Backend build and the OS matrix remain\n'
            'release gates and must not be inferred from FakeHost tests.',
            "Runtime architecture status")
        changed[path] = text

    except PatchError as exc:
        print(f"patch aborted before writing: {exc}", file=sys.stderr)
        return 2

    print("Validated Frontend-boundary integration source anchors:")
    for relative in sorted(changed):
        print(f"  {relative}")
    if args.check_only:
        print("\nCheck-only: no files were written.")
        return 0

    # Stage every new file before replacing any source. If a replacement fails,
    # restore already-replaced files from the original text captured above.
    staged: dict[str, Path] = {}
    originals = {relative: load(root, relative) for relative in changed}
    replaced: list[str] = []
    try:
        for relative, content in changed.items():
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n",
                                             dir=destination.parent, prefix=destination.name + ".",
                                             suffix=".tmp", delete=False) as stream:
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
                staged[relative] = Path(stream.name)
        for relative in sorted(changed):
            os.replace(staged[relative], root / relative)
            replaced.append(relative)
    except OSError as exc:
        for relative in reversed(replaced):
            destination = root / relative
            with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n",
                                             dir=destination.parent, prefix=destination.name + ".rollback.",
                                             suffix=".tmp", delete=False) as stream:
                stream.write(originals[relative])
                stream.flush()
                os.fsync(stream.fileno())
                rollback = Path(stream.name)
            os.replace(rollback, destination)
        print(f"write failed; restored files already replaced: {exc}", file=sys.stderr)
        return 2
    finally:
        for temporary in staged.values():
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    print("\nApplied Frontend-boundary integration to Runtime working tree.")
    print("No commit or push was performed. Run `make test boundary` in the Runtime checkout.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
