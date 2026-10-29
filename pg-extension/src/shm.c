/* shm.c — map a /dev/shm segment read/write or read-only. */
#include "postgres.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "np_shm.h"

#include "shm.h"

char *
np_shm_path(const char *seg)
{
  char *path = palloc(strlen(seg) + 2);
  sprintf(path, "/%s", seg);
  return path;
}

void *
np_shm_map_rw(const char *path, size_t size)
{
  int fd = shm_open(path, O_RDWR | O_CREAT, 0666);
  struct stat st;
  void *map;

  if (fd < 0)
    elog(ERROR, "nativepostgress: shm_open(%s) failed: %m", path);
  fchmod(fd, 0666); /* world-readable so a separate process can read it */
  if (fstat(fd, &st) != 0)
  {
    close(fd);
    elog(ERROR, "nativepostgress: fstat failed: %m");
  }
  if ((size_t) st.st_size < size && ftruncate(fd, (off_t) size) != 0)
  {
    close(fd);
    elog(ERROR, "nativepostgress: ftruncate failed: %m");
  }
  /* map only what is used; the file keeps the full capacity */
  map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    elog(ERROR, "nativepostgress: mmap failed: %m");
  (void) madvise(map, size, MADV_HUGEPAGE); /* huge pages: fewer TLB misses */
  return map;
}

void *
np_shm_map_ro(const char *path, const char *fn, size_t *size_out)
{
  int fd = shm_open(path, O_RDONLY, 0);
  struct stat st;
  NpShmHeader hdr;
  size_t map_size;
  void *map;

  if (fd < 0)
    elog(ERROR, "%s: shm_open(%s) failed: %m", fn, path);
  if (fstat(fd, &st) != 0)
  {
    close(fd);
    elog(ERROR, "%s: fstat failed: %m", fn);
  }

  /* read just the header first, then map only the payload */
  if ((size_t) st.st_size < NP_SHM_HEADER_SIZE ||
      pread(fd, &hdr, sizeof(hdr), 0) != (ssize_t) sizeof(hdr))
  {
    close(fd);
    elog(ERROR, "%s: cannot read segment header", fn);
  }
  if (memcmp(hdr.magic, NP_SHM_MAGIC, 8) != 0)
  {
    close(fd);
    elog(ERROR, "%s: bad segment magic", fn);
  }
  map_size = NP_SHM_HEADER_SIZE + (size_t) hdr.payload_size;
  if (map_size > (size_t) st.st_size)
  {
    close(fd);
    elog(ERROR, "%s: payload exceeds segment", fn);
  }

  map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    elog(ERROR, "%s: mmap failed: %m", fn);
  (void) madvise(map, map_size, MADV_HUGEPAGE);
  *size_out = map_size;
  return map;
}
