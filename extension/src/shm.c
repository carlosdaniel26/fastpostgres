/* shm.c — map a /dev/shm segment read/write or read-only.
 *
 * The worker publishes every batch into the same one or two data segments (the
 * pipelined client alternates: <ring>_a, <ring>_b), so the open/mmap/madvise
 * sequence is a per-batch cost paid to map memory that is already mapped. A
 * small keyed cache keeps those mappings for the life of a stream: the lookup
 * is a string compare, the creation happens once per segment. The cache is
 * flushed at a stream boundary (a new stream may name new segments) and on
 * worker exit.
 */
#include "postgres.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fastpostgres/fastpostgres_shm.h"

#include "shm.h"

/* Enough for the client's two pipelined segments and a few stale names, so a
 * capacity change does not evict a mapping the next batch still needs. */
#define FASTPOSTGRES_SHM_CACHE_MAX 8
#define FASTPOSTGRES_SHM_PATH_MAX  256

typedef struct FastpostgresShmCacheEntry
{
    char     path[FASTPOSTGRES_SHM_PATH_MAX];
    size_t   size;
    void    *map;
    uint64_t use; /* monotonic tick: the least recently used entry is evicted */
} FastpostgresShmCacheEntry;

/* One backend, one worker: fastpostgres_shm_map_cached() is only called from
 * the ring worker's batch path, which is single-threaded. */
static FastpostgresShmCacheEntry shm_cache[FASTPOSTGRES_SHM_CACHE_MAX];
static uint64_t                  shm_cache_tick;

char *
fastpostgres_shm_path(const char *seg)
{
    char *path = palloc(strlen(seg) + 2);
    sprintf(path, "/%s", seg);
    return path;
}

/* Create/truncate the segment to `size` bytes and map it read-write. */
void *
fastpostgres_shm_map_rw(const char *path, size_t size)
{
    int         fd = shm_open(path, O_RDWR | O_CREAT, 0666);
    struct stat st;
    void       *map;

    if (fd < 0) elog(ERROR, "fastpostgres: shm_open(%s) failed: %m", path);
    fchmod(fd, 0666); /* world-readable so a separate process can read it */
    if (fstat(fd, &st) != 0)
    {
        close(fd);
        elog(ERROR, "fastpostgres: fstat failed: %m");
    }
    if ((size_t)st.st_size < size && ftruncate(fd, (off_t)size) != 0)
    {
        close(fd);
        elog(ERROR, "fastpostgres: ftruncate failed: %m");
    }
    /* map only what is used; the file keeps the full capacity */
    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) elog(ERROR, "fastpostgres: mmap failed: %m");
    (void)madvise(map, size, MADV_HUGEPAGE); /* huge pages: fewer TLB misses */
    return map;
}

static void
shm_cache_unmap(FastpostgresShmCacheEntry *e)
{
    if (e->map != NULL) munmap(e->map, e->size);
    e->map     = NULL;
    e->size    = 0;
    e->path[0] = 0;
}

/* Drop every cached mapping. Called when a stream opens (the next stream may
 * use different segment names, or the same names over fresh files) and when the
 * worker exits, so a mapping never outlives the segment it describes. */
void
fastpostgres_shm_cache_fini(void)
{
    int i;

    for (i = 0; i < FASTPOSTGRES_SHM_CACHE_MAX; i++) shm_cache_unmap(&shm_cache[i]);
    shm_cache_tick = 0;
}

/* The mapping for (path, size), created on first use. `size` is part of the
 * key: a client that asks for a different capacity gets its own mapping rather
 * than an mmap shorter than the frame it is about to write. */
void *
fastpostgres_shm_map_cached(const char *path, size_t size)
{
    FastpostgresShmCacheEntry *victim = NULL;
    int                        free_slot = -1;
    int                        i;

    for (i = 0; i < FASTPOSTGRES_SHM_CACHE_MAX; i++)
    {
        FastpostgresShmCacheEntry *e = &shm_cache[i];

        if (e->map != NULL && e->size == size && strcmp(e->path, path) == 0)
        {
            e->use = ++shm_cache_tick;
            return e->map;
        }
        if (e->map == NULL)
        {
            if (free_slot < 0) free_slot = i;
        }
        else if (victim == NULL || e->use < victim->use)
            victim = e;
    }

    if (strlen(path) >= FASTPOSTGRES_SHM_PATH_MAX)
        elog(ERROR, "fastpostgres: segment name too long: %s", path);

    /* a free slot first, otherwise the least recently used mapping */
    if (free_slot >= 0) victim = &shm_cache[free_slot];
    shm_cache_unmap(victim);
    victim->map  = fastpostgres_shm_map_rw(path, size);
    victim->size = size;
    victim->use  = ++shm_cache_tick;
    strlcpy(victim->path, path, sizeof(victim->path));
    return victim->map;
}
