/* carry.h — rows a batch has already taken from the executor but has no room
 * for, kept until the next batch on the same portal.
 *
 * Filling a destination slot makes the executor consume the tuple, so a row the
 * segment cannot take would otherwise be lost - and the portal would read the
 * short fetch as end-of-data besides (see the atEnd note in
 * fastpostgres_rows_fetch). A worker can serve several rings at once, each with
 * its own portal, so the carry is keyed by portal: a row carried from stream A
 * must never be written into stream B's batch. It lives in TopMemoryContext
 * because the executor's context dies with the portal.
 *
 * It is a queue, not a single row: a batch can be cut short by the segment size
 * before the receiver has finished sampling the rows it already holds, and all
 * of those rows still have to go out in order. */
#ifndef FASTPOSTGRES_CARRY_H
#define FASTPOSTGRES_CARRY_H

#include "postgres.h"

#include "utils/portal.h"

typedef struct FastpostgresCarry
{
    Portal  portal;
    char   *buf;   /* [u32 len][row bytes] ... */
    uint32  len;   /* bytes used in buf */
    uint32  cap;   /* bytes allocated */
    uint32  head;  /* offset of the first queued row */
    uint32  count; /* queued rows */
} FastpostgresCarry;

/* Find (or, with create, attach) a portal's carry queue. NULL when create is
 * false and the portal has none, or when the table is full. */
FastpostgresCarry *fastpostgres_carry_for(Portal portal, bool create);

/* Append a row. The bytes are copied into TopMemoryContext. */
void fastpostgres_carry_put(FastpostgresCarry *c, const char *row, uint32 len);

/* The first queued row (NULL when empty) and its length. Valid until the next
 * put/pop/clear on the same queue. */
char *fastpostgres_carry_peek(FastpostgresCarry *c, uint32 *len);

/* Drop the row fastpostgres_carry_peek returned. */
void fastpostgres_carry_pop(FastpostgresCarry *c);

/* Drop one portal's queue (its buffer and its slot). */
void fastpostgres_carry_drop(Portal portal);

/* Drop every queue; an error aborts the worker's transaction, so no carried row
 * can still belong to a live portal. */
void fastpostgres_carry_reset(void);

#endif
