#ifndef CDIN_UI_SNAPSHOT_BUDGET_H
#define CDIN_UI_SNAPSHOT_BUDGET_H

#include <stddef.h>

/* Reserve bounded room for the final ui.flush envelope. Each semantic
 * message is constrained by the frame limit; their aggregate is constrained
 * independently by the snapshot limit. */
#define CDIN_UI_SNAPSHOT_FLUSH_RESERVE_BYTES 48u

static inline int cdin_ui_snapshot_budget_add(size_t *total,
                                               size_t payload_length,
                                               size_t max_message_bytes,
                                               size_t max_snapshot_bytes)
{
    size_t budget;
    if (total == NULL || max_snapshot_bytes <= CDIN_UI_SNAPSHOT_FLUSH_RESERVE_BYTES)
        return 0;
    if (payload_length > max_message_bytes) return 0;
    budget = max_snapshot_bytes - CDIN_UI_SNAPSHOT_FLUSH_RESERVE_BYTES;
    if (*total > budget || payload_length > budget - *total) return 0;
    *total += payload_length;
    return 1;
}

#endif
