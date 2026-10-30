/* fastpostgres_map.h — map a POSIX shared-memory segment from the Node addon.
 *
 * The backend has its own version in pg-extension/src/shm.c, but there errors
 * are reported with elog(); here every failure just returns NULL.
 */
#ifndef FASTPOSTGRES_MAP_H
#define FASTPOSTGRES_MAP_H

#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Map /dev/shm/<path> read-write, growing the file to `capacity` first and
 * mapping only `map_size` bytes (the file keeps the full capacity). */
static inline void *fastpostgres_map_rw(const char *path, size_t capacity, size_t map_size)
{
    int         fd = shm_open(path, O_RDWR | O_CREAT, 0666);
    struct stat st;
    void       *map;

    if (capacity == 0) capacity = map_size;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || ((size_t)st.st_size < capacity && ftruncate(fd, (off_t)capacity) != 0))
    {
        close(fd);
        return NULL;
    }
    map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return NULL;
#ifdef MADV_POPULATE_WRITE
    (void)madvise(map, map_size, MADV_POPULATE_WRITE); /* pre-fault in one syscall */
#endif
    return map;
}

/* Map the whole of an existing /dev/shm/<path> read-only; *size_out gets the
 * mapped size. */
static inline void *fastpostgres_map_ro(const char *path, size_t *size_out)
{
    int         fd = shm_open(path, O_RDONLY, 0);
    struct stat st;
    void       *map;

    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        close(fd);
        return NULL;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return NULL;
    *size_out = (size_t)st.st_size;
    return map;
}

#endif
