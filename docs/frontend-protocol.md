# Frontend Protocol

A Frontend is the part of `cdin` that accepts user input and draws the interface. It might use a GUI toolkit, a terminal, or no display at all. The Backend protocol describes the information exchanged between those parts without choosing how a screen is drawn.

The Spec marks the Frontend Protocol as **proposed**. Its byte-level transport is not final.

## 1. The model: Backend owns meaning, Frontend owns presentation

The Backend and Runtime decide which documents and views exist, what logical positions are selected, and what the status line should say. A Frontend decides how that information fits a window or terminal.

The protocol should not send font-specific line wrapping, pixel positions, or terminal cells as document meaning. A Frontend can calculate those details for its own display.

## 2. Connection lifecycle

Every connection follows this order:

1. Open a Frontend client.
2. Send `hello` with the Frontend Protocol major/minor version, a name, and requested capability bits.
3. Receive `hello_ack` with the selected protocol version, accepted capabilities, client ID, and session ID.
4. Attach to the session.
5. Exchange semantic messages.
6. Acknowledge each applied flush sequence.
7. Detach and close.

The handshake is mandatory. An incompatible major/minor pair returns `VERSION_MISMATCH`.

## 3. Driver and observers

The protocol currently implements a provisional single-driver rule because Spec decision OD-24 is still open:

- The first attached Frontend becomes the **driver** and may mutate session state.
- Later attached Frontends are **observers**. They can receive messages but `cdin_backend_frontend_can_mutate()` returns false.
- When the driver detaches, the next attached client becomes the driver.

This rule is an interim policy, not a permanent architecture decision. Once OD-24 is accepted, this module can change without modifying Kernel or Runtime.

## 4. Bounded queues and frame acknowledgements

Each Frontend has its own outgoing queue. The queue has a configured maximum number of messages and each payload has a configured maximum byte size. When the queue is full, enqueue returns `QUEUE_FULL` unless an unflushed message with the same message type and non-zero coalescing key can be replaced by the newer value.

A `flush` marks all messages that have not yet been flushed with one monotonically increasing sequence number. The Backend refuses to create more than the configured number of unacknowledged flushes. The Frontend sends `ack(sequence)` after it has applied the update. Messages covered by the acknowledgement are removed from its queue.

A message payload is owned by the queue after enqueue. A pointer returned by `cdin_backend_frontend_message_at()` is borrowed and remains valid only until the queue is changed or the client is closed. Copy it before retaining it.

## 5. In-process API example

```c
#include "cdin/backend.h"
#include "cdin/backend_protocol.h"

cdin_backend_frontend *frontend = NULL;
cdin_backend_hello hello = { 0u, 1u, "example-ui", 0u };
cdin_backend_hello_ack ack;

if (cdin_backend_frontend_open(backend, &frontend) == CDIN_BACKEND_OK &&
    cdin_backend_frontend_hello(frontend, &hello, &ack) == CDIN_BACKEND_OK &&
    cdin_backend_frontend_attach(frontend) == CDIN_BACKEND_OK) {
    /* The frontend is attached. Check ack.driver before sending mutations. */
}
```

The Frontend API is owner-thread-only. Call it on the same thread that owns the Backend lifecycle.

## 6. Current message surface

The public header declares message types for UI options, style tables, layout, visible view lines, invalidations, status line, prompt, panels, notifications, clipboard, dialogs, and errors.

The queue accepts payload bytes without interpreting them. The C library does not yet define a stable schema for those payloads. This is intentional: a protocol message's conceptual payload and its wire encoding are separate design questions.

## 7. What is not implemented yet

The protocol implementation in `0.1.0` provides in-process lifecycle and backpressure semantics only. It does not:

- serialize payloads to JSON, MessagePack, CBOR, or a custom binary format;
- open sockets, named pipes, or terminal streams;
- validate a serialized message schema;
- automatically convert `host.ui:publish(model)` into the declared message catalogue;
- negotiate named optional capabilities.

The current capability bitmap is deliberately conservative: no optional capability bits are accepted until the Spec defines their numeric mapping. The mandatory `core` namespace does not need an optional capability bit.

See [Known limitations](limitations.md) for the open Spec decisions that control the next step.
