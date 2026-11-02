#ifndef CDIN_BACKEND_SESSION_H
#define CDIN_BACKEND_SESSION_H
#include "../core/status.h"
#include <stddef.h>
#include <stdint.h>
typedef struct cdin_session cdin_session;
typedef struct cdin_session_document { uint64_t id; uint64_t version; uint64_t length; } cdin_session_document;
typedef struct cdin_session_view { uint64_t id; uint64_t document_id; int visible; int focused; } cdin_session_view;
cdin_backend_status cdin_session_create(size_t max_documents, size_t max_views, uint64_t session_id,
                                         cdin_session **out_session);
void cdin_session_destroy(cdin_session *session);
uint64_t cdin_session_id(const cdin_session *session);
cdin_backend_status cdin_session_document_open(cdin_session *session, uint64_t id,
                                                uint64_t version, uint64_t length);
cdin_backend_status cdin_session_document_update(cdin_session *session, uint64_t id,
                                                  uint64_t version, uint64_t length);
cdin_backend_status cdin_session_document_close(cdin_session *session, uint64_t id);
cdin_backend_status cdin_session_view_add(cdin_session *session, uint64_t id, uint64_t document_id);
cdin_backend_status cdin_session_view_remove(cdin_session *session, uint64_t id);
cdin_backend_status cdin_session_view_focus(cdin_session *session, uint64_t id);
cdin_backend_status cdin_session_view_get(const cdin_session *session, uint64_t id,
                                           cdin_session_view *out_view);
size_t cdin_session_document_count(const cdin_session *session);
size_t cdin_session_view_count(const cdin_session *session);
#endif
