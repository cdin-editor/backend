#ifndef CDIN_BACKEND_INTERNAL_H
#define CDIN_BACKEND_INTERNAL_H

#include "../include/cdin/backend.h"
#include "core/status.h"
#include "jobs/pool.h"
#include "kernel/adapter.h"
#include "protocol/server.h"
#include "session/session.h"
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef DWORD cdin_owner_thread;
#else
#include <pthread.h>
typedef pthread_t cdin_owner_thread;
#endif

typedef struct cdin_lua_host cdin_lua_host;

struct cdin_backend {
    cdin_backend_config config;
    char *runtime_root;
    char *runtime_entry;
    cdin_backend_state state;
    cdin_backend_status last_error;
    cdin_kernel_host *kernel;
    cdin_job_pool *jobs;
    cdin_session *session;
    cdin_protocol_server *protocol;
    cdin_lua_host *runtime;
    cdin_owner_thread owner_thread;
    int owner_thread_valid;
    uint64_t session_id;
};

int cdin_backend_is_owner(const cdin_backend *backend);
void cdin_backend_protocol_log_sink(void *context, int level, const char *event,
                                    uint64_t client_id, cdin_backend_status status,
                                    uint64_t detail);
void cdin_lua_host_destroy(cdin_lua_host *host);
cdin_backend_status cdin_lua_host_dispatch_frontend(cdin_lua_host *host, size_t max_events, size_t *out_processed);
cdin_backend_status cdin_lua_host_publish_latest_ui(cdin_lua_host *host, cdin_protocol_client *client);
cdin_backend_status cdin_lua_host_start(cdin_backend *backend, cdin_lua_host **out_host);
cdin_backend_status cdin_lua_host_pump(cdin_lua_host *host, size_t max_events, size_t *out_processed);
cdin_backend_status cdin_lua_host_shutdown(cdin_lua_host *host);

#endif
