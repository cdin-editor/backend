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

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const char *runtime_root = getenv("CDIN_RUNTIME_ROOT");
    cdin_backend_config config;
    cdin_backend *backend = NULL;
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
    status = cdin_backend_pump(backend, 64u, &processed);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "Backend pump failed: %s\n", cdin_backend_status_name(status));
        (void)cdin_backend_shutdown(backend, 1);
        cdin_backend_destroy(backend);
        return 1;
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
}
