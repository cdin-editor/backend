#include "cdin/backend.h"

#include <stdio.h>

int main(void)
{
    cdin_backend_config config;
    cdin_backend *backend = NULL;
    cdin_backend_status status;
    size_t processed = 0u;

    cdin_backend_config_init(&config);
    config.enable_lua = 0;
    config.enable_headless_protocol = 0;
    config.log_level = CDIN_BACKEND_LOG_WARN;

    status = cdin_backend_create(&config, &backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "cdin_backend_create failed: %s\n", cdin_backend_status_name(status));
        return 1;
    }
    status = cdin_backend_start(backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "cdin_backend_start failed: %s\n", cdin_backend_status_name(status));
        cdin_backend_destroy(backend);
        return 1;
    }
    status = cdin_backend_pump(backend, 8u, &processed);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "cdin_backend_pump failed: %s\n", cdin_backend_status_name(status));
        (void)cdin_backend_shutdown(backend, 1);
        cdin_backend_destroy(backend);
        return 1;
    }
    status = cdin_backend_shutdown(backend, 0);
    cdin_backend_destroy(backend);
    if (status != CDIN_BACKEND_OK) {
        (void)fprintf(stderr, "cdin_backend_shutdown failed: %s\n", cdin_backend_status_name(status));
        return 1;
    }
    return 0;
}
