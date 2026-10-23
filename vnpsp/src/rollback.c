/* Rollback: a ring of serialized checkpoints, one per interaction (say, menu,
 * click pause, input). Rolling back restores the previous one and re-runs its
 * interaction instruction. */
#include "engine_internal.h"

#include <stdlib.h>
#include <string.h>

static checkpoint *ring_at(vn_engine *e, int i)
{
    return &e->ring[(e->ring_start + i) % ROLLBACK_MAX];
}

static void drop_oldest(vn_engine *e)
{
    checkpoint *c = ring_at(e, 0);
    e->ring_bytes -= c->len;
    free(c->data);
    c->data = NULL;
    e->ring_start = (e->ring_start + 1) % ROLLBACK_MAX;
    e->ring_count--;
}

void rb_clear(vn_engine *e)
{
    while (e->ring_count) drop_oldest(e);
    e->ring_start = 0;
}

void rb_free(vn_engine *e)
{
    rb_clear(e);
    free(e->current.data);
    memset(&e->current, 0, sizeof e->current);
}

void rb_checkpoint(vn_engine *e)
{
    vn_buf b = {0};
    ck_serialize(e, &b);
    free(e->current.data);
    e->current.data = b.data;
    e->current.len = b.len;
    if (e->restoring) {
        /* Re-running the interaction we just rolled back to: it is already the ring top. */
        e->restoring = false;
        e->current.serial = e->ring_count ? ring_at(e, e->ring_count - 1)->serial : e->serial;
        persistent_save(e, false);
        return;
    }
    e->current.serial = ++e->serial;
    while (e->ring_count && (e->ring_count == ROLLBACK_MAX || e->ring_bytes + b.len > ROLLBACK_BYTES))
        drop_oldest(e);
    checkpoint *c = ring_at(e, e->ring_count);
    c->data = vn_alloc(b.len ? b.len : 1);
    memcpy(c->data, b.data, b.len);
    c->len = b.len;
    c->serial = e->current.serial;
    e->ring_count++;
    e->ring_bytes += b.len;
    persistent_save(e, false);
}

bool rb_back(vn_engine *e)
{
    if (e->ring_count < 2) return false;
    checkpoint *top = ring_at(e, e->ring_count - 1);
    e->ring_bytes -= top->len;
    free(top->data);
    top->data = NULL;
    e->ring_count--;
    checkpoint *prev = ring_at(e, e->ring_count - 1);
    if (!ck_restore(e, prev->data, prev->len)) return false;
    history_truncate(e, prev->serial);
    e->restoring = true;
    return true;
}
