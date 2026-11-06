/* carry.c — the per-portal queue of rows a batch already consumed. */
#include "postgres.h"

#include <string.h>

#include "utils/memutils.h"

#include "carry.h"

#define FASTPOSTGRES_CARRY_MAX 32

static FastpostgresCarry carries[FASTPOSTGRES_CARRY_MAX];

FastpostgresCarry *
fastpostgres_carry_for(Portal portal, bool create)
{
    int free_slot = -1;
    int i;

    for (i = 0; i < FASTPOSTGRES_CARRY_MAX; i++)
    {
        if (carries[i].portal == portal) return &carries[i];
        if (free_slot < 0 && carries[i].portal == NULL) free_slot = i;
    }
    if (!create) return NULL;
    if (free_slot < 0) elog(ERROR, "fastpostgres: too many open row-frame streams");

    carries[free_slot].portal = portal;
    return &carries[free_slot];
}

void
fastpostgres_carry_put(FastpostgresCarry *c, const char *row, uint32 len)
{
    MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
    uint32        need = c->len + 4 + len;

    if (need > c->cap)
    {
        uint32 cap = c->cap ? c->cap : 1024;

        while (cap < need) cap *= 2;
        if (c->buf == NULL)
            c->buf = palloc(cap);
        else
            c->buf = repalloc(c->buf, cap);
        c->cap = cap;
    }
    memcpy(c->buf + c->len, &len, 4);
    memcpy(c->buf + c->len + 4, row, len);
    c->len += 4 + len;
    c->count++;
    MemoryContextSwitchTo(old);
}

char *
fastpostgres_carry_peek(FastpostgresCarry *c, uint32 *len)
{
    if (c->count == 0) return NULL;
    memcpy(len, c->buf + c->head, 4);
    return c->buf + c->head + 4;
}

void
fastpostgres_carry_pop(FastpostgresCarry *c)
{
    uint32 len;

    if (c->count == 0) return;
    memcpy(&len, c->buf + c->head, 4);
    c->head += 4 + len;
    c->count--;
    if (c->count == 0) c->head = c->len = 0;
}

/* Free the buffer, which lives in TopMemoryContext, from wherever we are. */
static void
carry_clear(FastpostgresCarry *c)
{
    MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

    if (c->buf != NULL)
    {
        pfree(c->buf);
        c->buf = NULL;
    }
    c->len = c->cap = c->head = c->count = 0;
    c->portal = NULL;
    MemoryContextSwitchTo(old);
}

void
fastpostgres_carry_drop(Portal portal)
{
    FastpostgresCarry *c = fastpostgres_carry_for(portal, false);

    if (c != NULL) carry_clear(c);
}

void
fastpostgres_carry_reset(void)
{
    int i;

    for (i = 0; i < FASTPOSTGRES_CARRY_MAX; i++)
        if (carries[i].portal != NULL) carry_clear(&carries[i]);
}
