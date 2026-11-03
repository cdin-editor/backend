/*
 * Live Backend -> PUC Lua 5.4 -> cdin-runtime startup/shutdown smoke test.
 *
 * Requirement references:
 * - LIFE-001 / LIFE-002: https://github.com/cdin-editor/spec/blob/c4de9bf3d506b4e1f07b8986531cd056341708b0/docs/architecture/lifecycle.md#L37-L40
 * - BE-007: https://github.com/cdin-editor/spec/blob/c4de9bf3d506b4e1f07b8986531cd056341708b0/docs/components/backend.md#L70-L71
 * - Spec ADR-0019 (PUC Lua 5.4 C API bridge): https://github.com/cdin-editor/spec/blob/main/docs/adr/ADR-0019-runtime-bridge.md
 *
 * This test intentionally uses the production Runtime tree, not tests/support/fake_host.lua.
 */
#include "cdin/backend.h"
#include "cdin/backend_protocol.h"
#include <cdin/protocol.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int send_frontend_event(cdin_backend_frontend *frontend,
                               cdin_protocol_message_type type,
                               uint64_t sequence,
                               const uint8_t *payload,
                               size_t payload_length)
{
    cdin_protocol_message_view message;
    uint8_t frame[512];
    size_t frame_length = 0u;
    cdin_protocol_status encoded;

    memset(&message, 0, sizeof(message));
    message.major = CDIN_PROTOCOL_VERSION_MAJOR;
    message.minor = CDIN_PROTOCOL_VERSION_MINOR;
    message.sequence = sequence;
    message.type = type;
    message.name_space = "core";
    message.payload = payload;
    message.payload_length = payload_length;
    encoded = cdin_protocol_encode_frame(&message, frame, sizeof(frame),
                                         &frame_length,
                                         CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES);
    if (encoded != CDIN_PROTOCOL_OK) {
        (void)fprintf(stderr, "Protocol encode failed: %s\n",
                      cdin_protocol_status_name(encoded));
        return 1;
    }
    {
        cdin_backend_status received = cdin_backend_frontend_receive(
            frontend, frame, frame_length);
        if (received != CDIN_BACKEND_OK) {
            (void)fprintf(stderr, "Frontend receive failed: %s\n",
                          cdin_backend_status_name(received));
            return 1;
        }
    }
    return 0;
}

static int pump_frontend(cdin_backend *backend)
{
    size_t processed = 0u;
    cdin_backend_status status = cdin_backend_pump(backend, 64u, &processed);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend event pump failed: %s\n",
                      cdin_backend_status_name(status));
        return 1;
    }
    return 0;
}

static int check_lines(cdin_backend_frontend *frontend,
                       uint64_t expected_version,
                       const uint8_t *expected_lines,
                       size_t expected_lines_length)
{
    size_t index;
    size_t pending = cdin_backend_frontend_pending(frontend);

    for (index = 0u; index < pending; index++) {
        cdin_backend_message message;
        uint64_t version = UINT64_MAX;
        uint64_t total_lines = 0u;
        const uint8_t *lines = NULL;
        size_t lines_length = 0u;

        if (cdin_backend_frontend_message_at(frontend, index, &message) != CDIN_BACKEND_OK)
            return 1;
        if (message.type != (uint32_t)CDIN_PROTOCOL_VIEW_LINES)
            continue;
        if (cdin_protocol_payload_get_uint(message.payload, message.payload_length,
                                           "version", &version) != CDIN_PROTOCOL_OK)
            continue;
        if (version != expected_version)
            continue;
        if (cdin_protocol_payload_get_uint(message.payload, message.payload_length,
                                           "total_lines", &total_lines) != CDIN_PROTOCOL_OK ||
            total_lines != 3u) {
            (void)fprintf(stderr, "view.lines returned an unexpected logical line count\n");
            return 1;
        }
        if (cdin_protocol_payload_get_raw(message.payload, message.payload_length,
                                          "lines", &lines, &lines_length) != CDIN_PROTOCOL_OK ||
            lines_length != expected_lines_length ||
            memcmp(lines, expected_lines, expected_lines_length) != 0) {
            (void)fprintf(stderr, "view.lines did not preserve the expected source bytes\n");
            return 1;
        }
        return 0;
    }

    (void)fprintf(stderr, "No view.lines response for document version %llu\n",
                  (unsigned long long)expected_version);
    return 1;
}

int main(void)
{
    const char *runtime_root = getenv("CDIN_RUNTIME_ROOT");
    cdin_backend_config config;
    cdin_backend *backend = NULL;
    cdin_backend_frontend *frontend = NULL;
    cdin_backend_hello hello;
    cdin_backend_hello_ack hello_ack;
    cdin_backend_status status;
    size_t processed = 0u;
    int result = 1;

    /* These payloads are deliberately hand-authored, protocol-valid CBOR. The
     * first command creates a byte-oriented document with CRLF, LF and NUL;
     * subsequent requests verify the production Kernel -> Host -> Runtime ->
     * Protocol line-window path, then mutate the document through input.text. */
    static const uint8_t new_document_payload[] = {
        0xa2u,
        0x64u, 'n','a','m','e',
        0x6cu, 'd','o','c','u','m','e','n','t','.','n','e','w',
        0x69u, 'a','r','g','u','m','e','n','t','s',
        0xa1u,
        0x65u, 'b','y','t','e','s',
        0x4eu, 'o','n','e','\r','\n','t','w','o','\n','l','a','s','t','\0'
    };
    static const uint8_t request_lines_v0[] = {
        0xa4u,
        0x67u, 'v','e','r','s','i','o','n', 0x00u,
        0x67u, 'v','i','e','w','_','i','d', 0x66u, 'v','i','e','w','-','1',
        0x6au, 'f','i','r','s','t','_','l','i','n','e', 0x00u,
        0x6au, 'l','i','n','e','_','c','o','u','n','t', 0x03u
    };
    static const uint8_t request_lines_v1[] = {
        0xa4u,
        0x67u, 'v','e','r','s','i','o','n', 0x01u,
        0x67u, 'v','i','e','w','_','i','d', 0x66u, 'v','i','e','w','-','1',
        0x6au, 'f','i','r','s','t','_','l','i','n','e', 0x00u,
        0x6au, 'l','i','n','e','_','c','o','u','n','t', 0x03u
    };
    static const uint8_t text_payload[] = {
        0xa1u, 0x64u, 't','e','x','t', 0x61u, 'X'
    };
    static const uint8_t expected_lines_v0[] = {
        0x83u,
        0xa1u, 0x65u, 'b','y','t','e','s', 0x45u, 'o','n','e','\r','\n',
        0xa1u, 0x65u, 'b','y','t','e','s', 0x44u, 't','w','o','\n',
        0xa1u, 0x65u, 'b','y','t','e','s', 0x45u, 'l','a','s','t','\0'
    };
    static const uint8_t expected_lines_v1[] = {
        0x83u,
        0xa1u, 0x65u, 'b','y','t','e','s', 0x46u, 'X','o','n','e','\r','\n',
        0xa1u, 0x65u, 'b','y','t','e','s', 0x44u, 't','w','o','\n',
        0xa1u, 0x65u, 'b','y','t','e','s', 0x45u, 'l','a','s','t','\0'
    };

    if (runtime_root == NULL || runtime_root[0] == '\0') {
        (void)fprintf(stderr, "CDIN_RUNTIME_ROOT must point to the checked-out runtime/lua directory.\n");
        return 2;
    }

    cdin_backend_config_init(&config);
    config.enable_lua = 1;
    config.enable_headless_protocol = 1;
    config.runtime_root = runtime_root;
    config.runtime_entry = "cdin.runtime";
    config.log_level = CDIN_BACKEND_LOG_WARN;

    status = cdin_backend_create(&config, &backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend create failed: %s\n", cdin_backend_status_name(status));
        goto cleanup;
    }
    status = cdin_backend_start(backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend/Runtime startup failed: %s\n", cdin_backend_status_name(status));
        goto cleanup;
    }
    status = cdin_backend_pump(backend, 64u, &processed);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend initial pump failed: %s\n", cdin_backend_status_name(status));
        goto cleanup;
    }

    status = cdin_backend_frontend_open(backend, &frontend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Frontend open failed: %s\n", cdin_backend_status_name(status));
        goto cleanup;
    }
    memset(&hello, 0, sizeof(hello));
    hello.major = CDIN_PROTOCOL_VERSION_MAJOR;
    hello.minor = CDIN_PROTOCOL_VERSION_MINOR;
    hello.frontend_name = "runtime-host-smoke";
    status = cdin_backend_frontend_hello(frontend, &hello, &hello_ack);
    if (status != CDIN_BACKEND_OK ||
        hello_ack.major != CDIN_PROTOCOL_VERSION_MAJOR ||
        hello_ack.minor != CDIN_PROTOCOL_VERSION_MINOR) {
        (void)fprintf(stderr, "Frontend handshake failed: %s\n",
                      cdin_backend_status_name(status));
        goto cleanup;
    }
    status = cdin_backend_frontend_attach(frontend);
    if (status != CDIN_BACKEND_OK || !cdin_backend_frontend_can_mutate(frontend)) {
        (void)fprintf(stderr, "Frontend attach/driver lease failed: %s\n",
                      cdin_backend_status_name(status));
        goto cleanup;
    }

    if (send_frontend_event(frontend, CDIN_PROTOCOL_REQUEST_COMMAND, 1u,
                            new_document_payload, sizeof(new_document_payload)) != 0 ||
        pump_frontend(backend) != 0) goto cleanup;
    if (send_frontend_event(frontend, CDIN_PROTOCOL_VIEW_REQUEST_LINES, 2u,
                            request_lines_v0, sizeof(request_lines_v0)) != 0 ||
        pump_frontend(backend) != 0 ||
        check_lines(frontend, 0u, expected_lines_v0, sizeof(expected_lines_v0)) != 0) goto cleanup;

    if (send_frontend_event(frontend, CDIN_PROTOCOL_INPUT_TEXT, 3u,
                            text_payload, sizeof(text_payload)) != 0 ||
        pump_frontend(backend) != 0) goto cleanup;
    if (send_frontend_event(frontend, CDIN_PROTOCOL_VIEW_REQUEST_LINES, 4u,
                            request_lines_v1, sizeof(request_lines_v1)) != 0 ||
        pump_frontend(backend) != 0 ||
        check_lines(frontend, 1u, expected_lines_v1, sizeof(expected_lines_v1)) != 0) goto cleanup;

    result = 0;

cleanup:
    if (frontend != NULL) (void)cdin_backend_frontend_close(frontend);
    if (backend != NULL) {
        status = cdin_backend_shutdown(backend, 0);
        if (status != CDIN_BACKEND_OK) {
            (void)fprintf(stderr, "Backend/Runtime graceful shutdown failed: %s\n",
                          cdin_backend_status_name(status));
            result = 1;
        }
        cdin_backend_destroy(backend);
    }
    if (result == 0) {
        (void)puts("PASS runtime_host_smoke: Lua 5.4 + Kernel + Runtime 0.3.0 + Protocol 0.2.0; create/edit/versioned line read/shutdown");
    }
    return result;
}
