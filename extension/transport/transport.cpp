/*
 * transport.cpp — name resolution and the ring's provenance check. Everything
 * else about an engine lives with the engine; this file is the one place that
 * knows the name, because the worker hears about its engine as a string
 * (the third field of bgw_extra) and the SQL surface takes the same string.
 *
 * The check is deliberately exact: a segment whose magic, layout version or
 * engine does not match is refused before either side reads it as a ring. A
 * segment made for another engine must not be driven with the wrong doorbell,
 * so "close enough" here would corrupt a live session.
 */
#include "transport.h"

#include <cstddef>
#include <cstring>

/* The magic is the segment's first eight bytes. The literal is NUL-terminated,
 * so comparing eight of its bytes must be exactly comparing the name. */
static_assert(sizeof(FASTPOSTGRES_RING_MAGIC) - 1 == 8, "the ring magic must be eight bytes");

/* The tally is appended, and bench/ readers name its offset: an offset that
 * moved silently would make them parse something other than the tally. */
static_assert(offsetof(FastpostgresRing, tally) == FASTPOSTGRES_RING_TALLY_OFFSET,
              "the tally's offset is what the readers parse");

extern "C" const FastpostgresTransport *
fastpostgres_transport_by_name(const char *name)
{
    if (name == nullptr) return nullptr;

    if (std::strcmp(name, fastpostgres_transport_shm.name) == 0) return &fastpostgres_transport_shm;
    if (std::strcmp(name, fastpostgres_transport_eventfd.name) == 0) return &fastpostgres_transport_eventfd;
    return nullptr;
}

extern "C" int
fastpostgres_transport_check(const FastpostgresRing *r, const FastpostgresTransport *t)
{
    if (std::memcmp(r->magic, FASTPOSTGRES_RING_MAGIC, 8) != 0) return -1;
    if (r->version != FASTPOSTGRES_RING_VERSION) return -1;
    if (r->transport != t->id) return -1;
    return 0;
}
