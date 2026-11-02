/*
 * BE-001 assigns session identity and attached-client tracking to Backend;
 * ARCH-050 keeps document bytes and undo history outside this module. Session
 * stores opaque document handles and view relationships, never a shadow text
 * buffer. This matters because duplicating Kernel-owned bytes would create two
 * competing sources of truth after an edit or undo operation.
 * https://github.com/cdin-editor/spec/blob/main/docs/components/backend.md
 * https://github.com/cdin-editor/spec/blob/main/docs/architecture/module-boundaries.md
 */
#include "session.h"
#include <stdlib.h>
#include <string.h>

typedef struct cdin_session_doc_slot {
    cdin_session_document value;
    int active;
} cdin_session_doc_slot;

typedef struct cdin_session_view_slot {
    cdin_session_view value;
    int active;
} cdin_session_view_slot;

struct cdin_session {
    uint64_t id;
    cdin_session_doc_slot *documents;
    cdin_session_view_slot *views;
    size_t document_capacity;
    size_t view_capacity;
    size_t documents_open;
    size_t views_open;
};

static cdin_session_doc_slot *find_document(cdin_session *session, uint64_t id)
{
    size_t i;
    for (i = 0u; i < session->document_capacity; i++)
        if (session->documents[i].active && session->documents[i].value.id == id) return &session->documents[i];
    return NULL;
}

static const cdin_session_doc_slot *find_document_const(const cdin_session *session, uint64_t id)
{
    size_t i;
    for (i = 0u; i < session->document_capacity; i++)
        if (session->documents[i].active && session->documents[i].value.id == id) return &session->documents[i];
    return NULL;
}

static cdin_session_view_slot *find_view(cdin_session *session, uint64_t id)
{
    size_t i;
    for (i = 0u; i < session->view_capacity; i++)
        if (session->views[i].active && session->views[i].value.id == id) return &session->views[i];
    return NULL;
}

cdin_backend_status cdin_session_create(size_t max_documents, size_t max_views, uint64_t session_id,
                                         cdin_session **out_session)
{
    cdin_session *session;
    if (out_session == NULL || max_documents == 0u || max_views == 0u || session_id == 0u) return CDIN_BACKEND_INVALID_ARGUMENT;
    *out_session = NULL;
    if (max_documents > SIZE_MAX / sizeof(cdin_session_doc_slot) || max_views > SIZE_MAX / sizeof(cdin_session_view_slot)) return CDIN_BACKEND_LIMIT_EXCEEDED;
    session = (cdin_session *)calloc(1u, sizeof(*session));
    if (session == NULL) return CDIN_BACKEND_NO_MEMORY;
    session->documents = (cdin_session_doc_slot *)calloc(max_documents, sizeof(*session->documents));
    session->views = (cdin_session_view_slot *)calloc(max_views, sizeof(*session->views));
    if (session->documents == NULL || session->views == NULL) {
        free(session->documents); free(session->views); free(session); return CDIN_BACKEND_NO_MEMORY;
    }
    session->id = session_id;
    session->document_capacity = max_documents;
    session->view_capacity = max_views;
    *out_session = session;
    return CDIN_BACKEND_OK;
}

void cdin_session_destroy(cdin_session *session)
{
    if (session == NULL) return;
    free(session->views);
    free(session->documents);
    free(session);
}

uint64_t cdin_session_id(const cdin_session *session) { return session == NULL ? 0u : session->id; }

cdin_backend_status cdin_session_document_open(cdin_session *session, uint64_t id,
                                                uint64_t version, uint64_t length)
{
    size_t i;
    if (session == NULL || id == 0u) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (find_document(session, id) != NULL) return CDIN_BACKEND_INVALID_STATE;
    if (session->documents_open == session->document_capacity) return CDIN_BACKEND_LIMIT_EXCEEDED;
    for (i = 0u; i < session->document_capacity; i++) {
        if (!session->documents[i].active) {
            session->documents[i].active = 1;
            session->documents[i].value.id = id;
            session->documents[i].value.version = version;
            session->documents[i].value.length = length;
            session->documents_open++;
            return CDIN_BACKEND_OK;
        }
    }
    return CDIN_BACKEND_LIMIT_EXCEEDED;
}

cdin_backend_status cdin_session_document_update(cdin_session *session, uint64_t id,
                                                  uint64_t version, uint64_t length)
{
    cdin_session_doc_slot *slot;
    if (session == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    slot = find_document(session, id);
    if (slot == NULL) return CDIN_BACKEND_NOT_FOUND;
    if (version < slot->value.version) return CDIN_BACKEND_INVALID_STATE;
    slot->value.version = version;
    slot->value.length = length;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_session_document_close(cdin_session *session, uint64_t id)
{
    size_t i;
    cdin_session_doc_slot *slot;
    if (session == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    slot = find_document(session, id);
    if (slot == NULL) return CDIN_BACKEND_NOT_FOUND;
    for (i = 0u; i < session->view_capacity; i++)
        if (session->views[i].active && session->views[i].value.document_id == id) return CDIN_BACKEND_BUSY;
    memset(&slot->value, 0, sizeof(slot->value));
    slot->active = 0;
    session->documents_open--;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_session_view_add(cdin_session *session, uint64_t id, uint64_t document_id)
{
    size_t i;
    if (session == NULL || id == 0u || document_id == 0u) return CDIN_BACKEND_INVALID_ARGUMENT;
    if (find_view(session, id) != NULL) return CDIN_BACKEND_INVALID_STATE;
    if (find_document_const(session, document_id) == NULL) return CDIN_BACKEND_NOT_FOUND;
    if (session->views_open == session->view_capacity) return CDIN_BACKEND_LIMIT_EXCEEDED;
    for (i = 0u; i < session->view_capacity; i++) {
        if (!session->views[i].active) {
            session->views[i].active = 1;
            session->views[i].value.id = id;
            session->views[i].value.document_id = document_id;
            session->views[i].value.visible = 1;
            session->views[i].value.focused = 0;
            session->views_open++;
            return CDIN_BACKEND_OK;
        }
    }
    return CDIN_BACKEND_LIMIT_EXCEEDED;
}

cdin_backend_status cdin_session_view_remove(cdin_session *session, uint64_t id)
{
    cdin_session_view_slot *slot;
    if (session == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    slot = find_view(session, id);
    if (slot == NULL) return CDIN_BACKEND_NOT_FOUND;
    memset(&slot->value, 0, sizeof(slot->value));
    slot->active = 0;
    session->views_open--;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_session_view_focus(cdin_session *session, uint64_t id)
{
    size_t i;
    cdin_session_view_slot *target;
    if (session == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    target = find_view(session, id);
    if (target == NULL) return CDIN_BACKEND_NOT_FOUND;
    for (i = 0u; i < session->view_capacity; i++)
        if (session->views[i].active) session->views[i].value.focused = 0;
    target->value.focused = 1;
    return CDIN_BACKEND_OK;
}

cdin_backend_status cdin_session_view_get(const cdin_session *session, uint64_t id,
                                           cdin_session_view *out_view)
{
    size_t i;
    if (session == NULL || out_view == NULL) return CDIN_BACKEND_INVALID_ARGUMENT;
    for (i = 0u; i < session->view_capacity; i++) {
        if (session->views[i].active && session->views[i].value.id == id) {
            *out_view = session->views[i].value;
            return CDIN_BACKEND_OK;
        }
    }
    return CDIN_BACKEND_NOT_FOUND;
}

size_t cdin_session_document_count(const cdin_session *session) { return session == NULL ? 0u : session->documents_open; }
size_t cdin_session_view_count(const cdin_session *session) { return session == NULL ? 0u : session->views_open; }
