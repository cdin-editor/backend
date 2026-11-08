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


static size_t write_cbor_uint(uint8_t *out, uint64_t value)
{
    if (value < 24u) { out[0] = (uint8_t)value; return 1u; }
    if (value <= UINT8_MAX) { out[0] = 0x18u; out[1] = (uint8_t)value; return 2u; }
    if (value <= UINT16_MAX) { out[0] = 0x19u; out[1] = (uint8_t)(value >> 8u); out[2] = (uint8_t)value; return 3u; }
    out[0] = 0x1bu;
    out[1] = (uint8_t)(value >> 56u); out[2] = (uint8_t)(value >> 48u);
    out[3] = (uint8_t)(value >> 40u); out[4] = (uint8_t)(value >> 32u);
    out[5] = (uint8_t)(value >> 24u); out[6] = (uint8_t)(value >> 16u);
    out[7] = (uint8_t)(value >> 8u); out[8] = (uint8_t)value;
    return 9u;
}
static size_t write_cbor_text(uint8_t *out, const char *text)
{
    size_t n = strlen(text), h = n < 24u ? 1u : 2u;
    if (n < 24u) out[0] = (uint8_t)(0x60u + n);
    else { out[0] = 0x78u; out[1] = (uint8_t)n; }
    memcpy(out + h, text, n);
    return h + n;
}
static size_t make_splitter_payload(uint8_t *out, size_t cap, uint64_t request_id,
                                    uint64_t revision, const char *split_id)
{
    size_t p = 0u;
    if (cap < 128u) return 0u;
    out[p++] = 0xa4u;
    p += write_cbor_text(out + p, "ratio_bp"); p += write_cbor_uint(out + p, 5000u);
    p += write_cbor_text(out + p, "split_id"); p += write_cbor_text(out + p, split_id);
    p += write_cbor_text(out + p, "request_id"); p += write_cbor_uint(out + p, request_id);
    p += write_cbor_text(out + p, "layout_revision"); p += write_cbor_uint(out + p, revision);
    return p;
}
static int get_frontend_revision(cdin_backend_frontend *frontend, uint64_t *revision)
{
    size_t i, count = cdin_backend_frontend_pending(frontend);
    cdin_backend_message message;
    for (i = 0u; i < count; ++i) {
        if (cdin_backend_frontend_message_at(frontend, i, &message) != CDIN_BACKEND_OK ||
            message.type != CDIN_BACKEND_MESSAGE_UI_LAYOUT) continue;
        if (cdin_protocol_payload_get_uint(message.payload, message.payload_length,
                                           "revision", revision) == CDIN_PROTOCOL_OK) return 1;
    }
    return 0;
}
static int send_splitter_expect_status(cdin_backend *backend, cdin_backend_frontend *frontend,
                                       uint64_t revision, uint64_t request_id,
                                       const char *split_id, const char *expected)
{
    uint8_t payload[256], frame[512];
    size_t payload_length, frame_length = 0u, i;
    size_t processed = 0u, status_length = 0u;
    uint64_t observed_id = 0u;
    char observed_status[32];
    cdin_protocol_message_view input;
    cdin_backend_message output;
    payload_length = make_splitter_payload(payload, sizeof(payload), request_id, revision, split_id);
    if (payload_length == 0u) return 0;
    memset(&input, 0, sizeof(input));
    input.major = CDIN_PROTOCOL_VERSION_MAJOR;
    input.minor = CDIN_PROTOCOL_VERSION_MINOR;
    input.sequence = 1u;
    input.type = CDIN_PROTOCOL_PANE_SPLITTER_RESIZE;
    input.name_space = "pane";
    input.payload = payload;
    input.payload_length = payload_length;
    if (cdin_protocol_encode_frame(&input, frame, sizeof(frame), &frame_length,
                                   CDIN_PROTOCOL_DEFAULT_MAX_FRAME_BYTES) != CDIN_PROTOCOL_OK) return 0;
    if (cdin_backend_frontend_receive(frontend, frame, frame_length) != CDIN_BACKEND_OK) return 0;
    if (cdin_backend_pump(backend, 64u, &processed) != CDIN_BACKEND_OK) return 0;
    for (i = 0u; i < cdin_backend_frontend_pending(frontend); ++i) {
        if (cdin_backend_frontend_message_at(frontend, i, &output) != CDIN_BACKEND_OK ||
            output.type != CDIN_BACKEND_MESSAGE_PANE_SPLITTER_RESULT) continue;
        if (cdin_protocol_payload_get_text(output.payload, output.payload_length, "status",
                                           observed_status, sizeof(observed_status),
                                           &status_length) != CDIN_PROTOCOL_OK ||
            cdin_protocol_payload_get_uint(output.payload, output.payload_length,
                                           "request_id", &observed_id) != CDIN_PROTOCOL_OK) return 0;
        observed_status[status_length < sizeof(observed_status) ? status_length : sizeof(observed_status)-1u] = '\0';
        return observed_id == request_id && strcmp(observed_status, expected) == 0;
    }
    return 0;
}

int main(void)
{
    const char *runtime_root = getenv("CDIN_RUNTIME_ROOT");
    cdin_backend_config config;
    cdin_backend *backend = NULL;
    cdin_backend_frontend *driver = NULL, *observer = NULL;
    cdin_backend_hello hello;
    cdin_backend_hello_ack ack;
    uint64_t revision = 0u;
    cdin_backend_status status;
    size_t processed = 0u;

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
        return 1;
    }
    status = cdin_backend_start(backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend/Runtime startup failed: %s\n", cdin_backend_status_name(status));
        cdin_backend_destroy(backend);
        return 1;
    }
    if (cdin_backend_frontend_open(backend, &driver) != CDIN_BACKEND_OK) goto fail;
    memset(&hello, 0, sizeof(hello));
    hello.major = CDIN_BACKEND_PROTOCOL_MAJOR;
    hello.minor = CDIN_BACKEND_PROTOCOL_MINOR;
    hello.frontend_name = "splitter-driver";
    hello.capabilities = CDIN_BACKEND_CAP_PANE_SPLITTER_RESIZE;
    if (cdin_backend_frontend_hello(driver, &hello, &ack) != CDIN_BACKEND_OK ||
        !ack.driver || (ack.accepted_capabilities & CDIN_BACKEND_CAP_PANE_SPLITTER_RESIZE) == 0u ||
        cdin_backend_frontend_attach(driver) != CDIN_BACKEND_OK ||
        cdin_backend_pump(backend, 64u, &processed) != CDIN_BACKEND_OK ||
        !get_frontend_revision(driver, &revision) ||
        !send_splitter_expect_status(backend, driver, revision, 1u, "missing-split", "not_found")) {
        (void)fprintf(stderr, "Live Runtime splitter request did not return not_found.\n");
        goto fail;
    }
    if (cdin_backend_frontend_open(backend, &observer) != CDIN_BACKEND_OK) goto fail;
    hello.frontend_name = "splitter-observer";
    if (cdin_backend_frontend_hello(observer, &hello, &ack) != CDIN_BACKEND_OK ||
        ack.driver || (ack.accepted_capabilities & CDIN_BACKEND_CAP_PANE_SPLITTER_RESIZE) == 0u ||
        cdin_backend_frontend_attach(observer) != CDIN_BACKEND_OK ||
        cdin_backend_pump(backend, 64u, &processed) != CDIN_BACKEND_OK ||
        !get_frontend_revision(observer, &revision) ||
        !send_splitter_expect_status(backend, observer, revision, 1u, "missing-split", "not_driver")) {
        (void)fprintf(stderr, "Live Runtime did not reject Observer splitter mutation.\n");
        goto fail;
    }
    status = cdin_backend_pump(backend, 64u, &processed);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend pump failed: %s\n", cdin_backend_status_name(status));
        (void)cdin_backend_shutdown(backend, 1);
        cdin_backend_destroy(backend);
        return 1;
    }
    /* Close attached clients before shutdown destroys the Protocol server. */
    if (observer != NULL) {
        (void)cdin_backend_frontend_detach(observer);
        (void)cdin_backend_frontend_close(observer);
        observer = NULL;
    }
    if (driver != NULL) {
        (void)cdin_backend_frontend_detach(driver);
        (void)cdin_backend_frontend_close(driver);
        driver = NULL;
    }
    status = cdin_backend_shutdown(backend, 0);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend/Runtime graceful shutdown failed: %s\n", cdin_backend_status_name(status));
        cdin_backend_destroy(backend);
        return 1;
    }
    cdin_backend_destroy(backend);
    (void)puts("PASS runtime_host_smoke: real Lua 5.4 state, production cdin-runtime load/start/pump/shutdown");
    return 0;
fail:
    if (observer != NULL) (void)cdin_backend_frontend_close(observer);
    if (driver != NULL) (void)cdin_backend_frontend_close(driver);
    if (backend != NULL) {
        (void)cdin_backend_shutdown(backend, 1);
        cdin_backend_destroy(backend);
    }
    return 1;
}
