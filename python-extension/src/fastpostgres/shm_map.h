/* shm_map.h — map a POSIX shared-memory segment for the Python client.
 *
 * The backend has its own copy in pg-extension/src/shm.c (where errors go
 * through elog()); here every failure just returns nullptr so the caller can
 * raise a Python exception.
 */
#ifndef FASTPOSTGRES_PY_SHM_MAP_H
#define FASTPOSTGRES_PY_SHM_MAP_H

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>

/* Map /dev/shm/<path> read-write, growing the file to `capacity` first and
 * mapping only `map_size` bytes (the file keeps the full capacity). */
static inline void *fp_map_rw(const char *path, size_t capacity, size_t map_size)
{
    int         fd = shm_open(path, O_RDWR | O_CREAT, 0666);
    struct stat st;
    void       *map;

    if (capacity == 0) capacity = map_size;
    if (fd < 0) return nullptr;
    if (fstat(fd, &st) != 0 || ((size_t)st.st_size < capacity && ftruncate(fd, (off_t)capacity) != 0))
    {
        close(fd);
        return nullptr;
    }
    map = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return nullptr;
#ifdef MADV_POPULATE_WRITE
    (void)madvise(map, map_size, MADV_POPULATE_WRITE); /* pre-fault in one syscall */
#endif
    return map;
}

/* Map the whole of an existing /dev/shm/<path> read-only; *size_out gets the
 * mapped size. */
static inline void *fp_map_ro(const char *path, size_t *size_out)
{
    int         fd = shm_open(path, O_RDONLY, 0);
    struct stat st;
    void       *map;

    if (fd < 0) return nullptr;
    if (fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        close(fd);
        return nullptr;
    }
    map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return nullptr;
    *size_out = (size_t)st.st_size;
    return map;
}

#endif
