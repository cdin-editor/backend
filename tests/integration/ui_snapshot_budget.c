#include "runtime/ui_snapshot_budget.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

int main(void)
{
    size_t total = 0u;
    const size_t frame_limit = 16u * 1024u;
    const size_t snapshot_limit = 64u * 1024u;

    /* Individually valid frames may exceed the per-frame limit in aggregate. */
    assert(cdin_ui_snapshot_budget_add(&total, 8u * 1024u,
                                       frame_limit, snapshot_limit));
    assert(cdin_ui_snapshot_budget_add(&total, 9u * 1024u,
                                       frame_limit, snapshot_limit));
    assert(total == 17u * 1024u);

    /* One frame still cannot exceed its own limit. */
    assert(!cdin_ui_snapshot_budget_add(&total, frame_limit + 1u,
                                        frame_limit, snapshot_limit));
    assert(total == 17u * 1024u);

    /* The aggregate bound reserves room for the final ui.flush envelope. */
    total = 0u;
    assert(cdin_ui_snapshot_budget_add(&total, 16u * 1024u,
                                       frame_limit, snapshot_limit));
    assert(cdin_ui_snapshot_budget_add(&total, 16u * 1024u,
                                       frame_limit, snapshot_limit));
    assert(cdin_ui_snapshot_budget_add(&total, 16u * 1024u,
                                       frame_limit, snapshot_limit));
    assert(!cdin_ui_snapshot_budget_add(&total, 16u * 1024u,
                                        frame_limit, snapshot_limit));
    assert(!cdin_ui_snapshot_budget_add(&total, 1u, frame_limit, 48u));
    assert(!cdin_ui_snapshot_budget_add(NULL, 1u, frame_limit, snapshot_limit));

    (void)puts("PASS ui_snapshot_budget: per-frame and aggregate snapshot limits are independent");
    return 0;
}
