/*
 * PROTO-030 defines the in-process C ABI as one transport for the same
 * semantic messages; PROTO-033 requires that transport to obey ABI rules.
 * Keep this file as a thin public-API adapter over the protocol core so the
 * public header does not expose internal queue or client structures.
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/frontend-protocol.md
 */
#include "../backend_internal.h"
#include "../diagnostics/log.h"
#include "../../include/cdin/backend_protocol.h"


void cdin_backend_protocol_log_sink(void *context, int level, const char *event,
                                    uint64_t client_id, cdin_backend_status status,
                                    uint64_t detail)
{
    cdin_backend *backend = (cdin_backend *)context;
    if (backend == NULL || backend->kernel == NULL) return;
    cdin_backend_logf(backend->kernel, (cdin_backend_log_level)level, "PROTOCOL",
                      __FILE__, (uint32_t)__LINE__, __func__,
                      "event=%s client_id=%llu status=%s detail=%llu",
                      event != NULL ? event : "unknown",
                      (unsigned long long)client_id,
                      cdin_backend_status_name(status),
                      (unsigned long long)detail);
}

cdin_backend_status cdin_backend_frontend_open(cdin_backend *backend,
                                                 cdin_backend_frontend **out_frontend)
{
    if (backend == NULL || out_frontend == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (!cdin_backend_is_owner(backend) || backend->state != CDIN_BACKEND_STATE_RUNNING) return CDIN_BACKEND_INVALID_STATE;
    if (backend->protocol == NULL) return CDIN_BACKEND_UNSUPPORTED;
    return cdin_protocol_client_open(backend->protocol, (cdin_protocol_client **)out_frontend);
}

cdin_backend_status cdin_backend_frontend_hello(cdin_backend_frontend *frontend,
                                                  const cdin_backend_hello *hello,
                                                  cdin_backend_hello_ack *ack)
{
    cdin_protocol_hello request;
    cdin_protocol_hello_ack response;
    cdin_backend_status status;
    if (frontend == NULL || hello == NULL || ack == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    request.major = hello->major;
    request.minor = hello->minor;
    request.frontend_name = hello->frontend_name;
    request.capabilities = hello->capabilities;
    status = cdin_protocol_hello_accept((cdin_protocol_client *)frontend, &request, &response);
    if (status != CDIN_BACKEND_OK) return status;
    ack->major = response.major;
    ack->minor = response.minor;
    ack->accepted_capabilities = response.accepted_capabilities;
    ack->client_id = response.client_id;
    ack->session_id = response.session_id;
    ack->driver = response.driver;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_backend_frontend_attach(cdin_backend_frontend *frontend)
{ return cdin_protocol_attach((cdin_protocol_client *)frontend); }
cdin_backend_status cdin_backend_frontend_detach(cdin_backend_frontend *frontend)
{ return cdin_protocol_detach((cdin_protocol_client *)frontend); }
cdin_backend_status cdin_backend_frontend_close(cdin_backend_frontend *frontend)
{ return cdin_protocol_client_close((cdin_protocol_client *)frontend); }
int cdin_backend_frontend_can_mutate(const cdin_backend_frontend *frontend)
{ return cdin_protocol_can_mutate((const cdin_protocol_client *)frontend); }

cdin_backend_status cdin_backend_frontend_enqueue(cdin_backend_frontend *frontend,
                                                    const cdin_backend_message *message)
{
    cdin_protocol_message request;
    if (message == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    request.type = message->type;
    request.coalesce_key = message->coalesce_key;
    request.flush_sequence = message->flush_sequence;
    request.payload = message->payload;
    request.payload_length = message->payload_length;
    return cdin_protocol_enqueue((cdin_protocol_client *)frontend, &request);
}

cdin_backend_status cdin_backend_frontend_flush(cdin_backend_frontend *frontend,
                                                 uint64_t *out_sequence)
{ return cdin_protocol_flush((cdin_protocol_client *)frontend, out_sequence); }
cdin_backend_status cdin_backend_frontend_ack(cdin_backend_frontend *frontend, uint64_t sequence)
{ return cdin_protocol_ack((cdin_protocol_client *)frontend, sequence); }
size_t cdin_backend_frontend_pending(const cdin_backend_frontend *frontend)
{ return cdin_protocol_pending_messages((const cdin_protocol_client *)frontend); }

cdin_backend_status cdin_backend_frontend_message_at(const cdin_backend_frontend *frontend,
                                                       size_t index,
                                                       cdin_backend_message *out_message)
{
    cdin_protocol_message message;
    cdin_backend_status status;
    if (out_message == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    status = cdin_protocol_message_at((const cdin_protocol_client *)frontend, index, &message);
    if (status != CDIN_BACKEND_OK) return status;
    out_message->type = message.type;
    out_message->coalesce_key = message.coalesce_key;
    out_message->flush_sequence = message.flush_sequence;
    out_message->payload = message.payload;
    out_message->payload_length = message.payload_length;
    return CDIN_BACKEND_OK;
}
