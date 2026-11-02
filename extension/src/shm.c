/* shm.c — map a /dev/shm segment read/write or read-only. */
#include "postgres.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fastpostgres/fastpostgres_shm.h"

#include "shm.h"

char *
fastpostgres_shm_path(const char *seg)
{
    char *path = palloc(strlen(seg) + 2);
    sprintf(path, "/%s", seg);
    return path;
}

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
