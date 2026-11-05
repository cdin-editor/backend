# Integration and end-to-end testing

The Backend test suite focuses on observable workflows. Each scenario crosses
several public or subsystem boundaries, uses real temporary files or child
processes where appropriate, and checks the behavior a host application sees.
The repository intentionally does not maintain a separate unit-test target. The current release registers six scenarios: four end-to-end workflows and two integration workflows. Every scenario exercises a user-visible sequence and includes at least one failure or boundary condition.

## Run the scenarios

The operating-system workflows need only a C11 compiler and the portable
services library:

```sh
make e2e
```

The full Backend workflows also need the pinned Kernel package:

```sh
make deps
make all
make integration
```

CTest exposes the same scenarios after a CMake build:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$PWD/.deps/kernel"
cmake --build build
ctest --test-dir build --output-on-failure
```

## Scenario coverage

| Scenario | Real-world story | Edge cases checked |
| --- | --- | --- |
| `job_pipeline` | Multiple files are indexed in the background while the UI thread remains responsive | Queue capacity, extra submission rejection, queued and running cancellation, worker failure, bounded completion queue, callback thread ownership, graceful drain |
| `job_abort_shutdown` | The host exits while background jobs are still running | In-flight worker exit, queued-job disposal, callback suppression, destructor completion and rejection after shutdown |
| `platform_file_workflow` | An editor saves a workspace document and reads it in bounded chunks | Atomic publication, overwrite disabled, maximum file size, abort preserving old data, chunk boundaries, directory listing and relative paths |
| `process_supervision` | An editor invokes a formatter or helper process | Shell metacharacters passed literally, captured output, timeout, termination and non-zero exit code |
| `backend_session` | A terminal Frontend joins a live headless session | Lifecycle transitions, wrong-thread pump, frontend limit, version mismatch, driver/observer roles, coalescing, queue full, flush backpressure, ACK validation, promotion and diagnostic logging |
| `ui_snapshot_budget` | A Runtime UI snapshot is published as an atomic group of individually framed messages | Aggregate snapshot budget is independent from per-frame size; oversized individual frames and aggregate overflow are rejected |
| `kernel_document_workflow` | A user edits and saves a document with concurrent views | Document create/read/replace, stale-version rejection, stable snapshots, notifications, staged transaction, commit/abort, undo/redo, search, unsupported regex and stale handle rejection |

## Keep a scenario useful

A scenario should tell a user story before it asserts an internal detail. It
should set up resources the way an application would, perform several related
operations, verify both successful and failed paths, and clean up after itself.
Avoid tests that merely call a function once with a constant and check a return
code. Prefer checking observable content, state transitions, event delivery,
resource cleanup and behavior after failures.

For concurrency scenarios, use gates and explicit completion signals rather
than relying on arbitrary sleeps. A timeout may be used as a test failure bound,
but it should not be the synchronization mechanism.

For every failure path, verify what remains unchanged. Examples include a
failed atomic save preserving the last good file, a rejected stale edit leaving
the document untouched, and an aborted transaction not leaking draft content.
