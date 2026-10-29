// shm-read — the V8-side reader: opens a /dev/shm result segment written by
// the nativepostgress extension (native_query_shm) and decodes the NPCOLUMN
// payload (header + per-column typed data / raw text), printing a summary and
// the first few values of each column.
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "v8ext/np_shm.h"

static uint32_t rd32(const uint8_t *&p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  p += 4;
  return v;
}
static uint64_t rd64(const uint8_t *&p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  p += 8;
  return v;
}

int main(int argc, char **argv) {
  const char *name = argc > 1 ? argv[1] : "np_test";
  std::string path = "/" + std::string(name);

  int fd = shm_open(path.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    std::perror("shm_open");
    std::fprintf(stderr, "  (did the postgres extension write '%s'?)\n", name);
    return 1;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    std::perror("fstat");
    return 1;
  }
  size_t sz = static_cast<size_t>(st.st_size);
  uint8_t *map =
      static_cast<uint8_t *>(mmap(nullptr, sz, PROT_READ, MAP_SHARED, fd, 0));
  close(fd);
  if (map == MAP_FAILED) {
    std::perror("mmap");
    return 1;
  }

  NpShmHeader *h = reinterpret_cast<NpShmHeader *>(map);
  if (std::memcmp(h->magic, NP_SHM_MAGIC, 8) != 0) {
    std::fprintf(stderr, "bad shm magic\n");
    return 1;
  }

  std::printf("shm \"%s\": payload=%llu bytes seq=%llu\n", name,
              static_cast<unsigned long long>(h->payload_size),
              static_cast<unsigned long long>(h->seq));

  const uint8_t *p = map + NP_SHM_HEADER_SIZE;
  if (std::memcmp(p, "NPCOLUMN", 8) != 0) {
    std::fprintf(stderr, "bad columnar magic\n");
    return 1;
  }
  p += 8;
  uint32_t version = rd32(p);
  rd32(p); /* flags */
  uint32_t ncols = rd32(p);
  uint64_t nrows = rd64(p);
  std::printf("columnar: version=%u ncols=%u nrows=%llu\n", version, ncols,
              static_cast<unsigned long long>(nrows));

  for (uint32_t c = 0; c < ncols; ++c) {
    uint32_t name_len = rd32(p);
    std::string name(reinterpret_cast<const char *>(p), name_len);
    p += name_len;
    uint8_t kind = *p++;
    uint32_t pgtype = rd32(p);
    p += static_cast<uint32_t>((nrows + 7) / 8); /* null bitmap */
    uint64_t data_len = rd64(p);
    const uint8_t *data = p;
    p += data_len;

    std::printf("  col[%u] %-12s kind=%u pgtype=%u first:", c, name.c_str(),
                kind, pgtype);
    uint64_t show = nrows < 5 ? nrows : 5;
    switch (kind) {
      case 1: /* bool */
        for (uint64_t r = 0; r < show; ++r) std::printf(" %d", data[r] != 0);
        break;
      case 2: {
        const int32_t *v = reinterpret_cast<const int32_t *>(data);
        for (uint64_t r = 0; r < show; ++r) std::printf(" %d", v[r]);
        break;
      }
      case 3: {
        const int64_t *v = reinterpret_cast<const int64_t *>(data);
        for (uint64_t r = 0; r < show; ++r)
          std::printf(" %lld", static_cast<long long>(v[r]));
        break;
      }
      case 4: {
        const double *v = reinterpret_cast<const double *>(data);
        for (uint64_t r = 0; r < show; ++r) std::printf(" %g", v[r]);
        break;
      }
      case 5: {
        const uint32_t *off = reinterpret_cast<const uint32_t *>(data);
        const uint8_t *bytes = data + (nrows + 1) * 4;
        for (uint64_t r = 0; r < show; ++r) {
          std::string s(reinterpret_cast<const char *>(bytes) + off[r],
                        off[r + 1] - off[r]);
          std::printf(" %s", s.c_str());
        }
        break;
      }
    }
    std::printf("\n");
  }

  munmap(map, sz);
  return 0;
}
