/* carry.cpp — the per-portal queue of rows a batch already consumed. */
#include "pgxx.h"

#include <array>
#include <cstring>

#include "carry.h"

namespace
{

constexpr int kCarryMax = 32;

std::array<fp::Carry, kCarryMax> carries{};

inline bool
same_name(const char *a, const char *b)
{
    return std::strncmp(a, b, NAMEDATALEN - 1) == 0;
}

/* Free the buffer, which lives in TopMemoryContext, from wherever we are. */
void
carry_clear(fp::Carry *c)
{
    MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

    if (c->buf != nullptr)
    {
        pfree(c->buf);
        c->buf = nullptr;
    }
    c->len = c->cap = c->head = c->count = 0;
    c->portal                            = nullptr;
    c->name[0]                           = 0;
    MemoryContextSwitchTo(old);
}

} // namespace

fp::Carry *
fp::carry_for(Portal portal, bool create)
{
    int free_slot = -1;

    for (int i = 0; i < kCarryMax; i++)
    {
        if (portal != nullptr && carries[i].portal == portal && same_name(carries[i].name, portal->name))
            return &carries[i];

        /* A savepoint abort destroys the portal but not the carry; the next
         * request re-creates a portal with the same stream name. Reattach the
         * queued rows to it instead of losing them or leaking them. */
        if (portal != nullptr && carries[i].portal != nullptr && carries[i].name[0] != 0 &&
            same_name(carries[i].name, portal->name))
        {
            carries[i].portal = portal;
            return &carries[i];
        }

        if (free_slot < 0 && carries[i].portal == nullptr) free_slot = i;
    }
    if (!create) return nullptr;
    if (free_slot < 0) elog(ERROR, "fastpostgres: too many open row-frame streams");

    carries[free_slot].portal = portal;
    if (portal != nullptr) strlcpy(carries[free_slot].name, portal->name, sizeof(carries[free_slot].name));
    return &carries[free_slot];
}

void
fp::carry_put(fp::Carry *c, const char *row, uint32 len)
{
    MemoryContext old  = MemoryContextSwitchTo(TopMemoryContext);
    uint32        need = c->len + 4 + len;

    if (need > c->cap)
    {
        uint32 cap = c->cap != 0 ? c->cap : 1024;

        while (cap < need)
            cap *= 2;
        if (c->buf == nullptr)
            c->buf = static_cast<char *>(palloc(cap));
        else
            c->buf = static_cast<char *>(repalloc(c->buf, cap));
        c->cap = cap;
    }
    std::memcpy(c->buf + c->len, &len, 4);
    std::memcpy(c->buf + c->len + 4, row, len);
    c->len += 4 + len;
    c->count++;
    MemoryContextSwitchTo(old);
}

char *
fp::carry_peek(fp::Carry *c, uint32 *len)
{
    if (c->count == 0) return nullptr;
    std::memcpy(len, c->buf + c->head, 4);
    return c->buf + c->head + 4;
}

void
fp::carry_pop(fp::Carry *c)
{
    uint32 len;

    if (c->count == 0) return;
    std::memcpy(&len, c->buf + c->head, 4);
    c->head += 4 + len;
    c->count--;
    if (c->count == 0) c->head = c->len = 0;
}

void
fp::carry_drop_name(const char *name)
{
    if (name == nullptr || name[0] == 0) return;
    for (fp::Carry &c : carries)
        if (c.name[0] != 0 && same_name(c.name, name))
        {
            carry_clear(&c);
            return;
        }
}

void
fp::carry_drop(Portal portal)
{
    if (portal == nullptr) return;
    fp::carry_drop_name(portal->name);
}

void
fp::carry_reset(void)
{
    for (fp::Carry &c : carries)
        if (c.portal != nullptr) carry_clear(&c);
}
