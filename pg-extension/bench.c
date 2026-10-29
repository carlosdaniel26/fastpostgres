// bench.c — read + write benchmark across three transports, for the SAME data:
//
//   tcp    — libpq over 127.0.0.1 (network stack)
//   unix   — libpq over the unix socket
//   shm    — native columnar binary over /dev/shm (no socket)
//
// Usage: ./bench [nrows ...]   (default sizes: 100000 1000000 10000000)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "np_shm.h"

#define QUERY \
  "SELECT id, account_id, balance, active, name, email, score, age FROM bench"

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void fail(PGconn *conn, const char *what) {
  fprintf(stderr, "%s: %s\n", what, PQerrorMessage(conn));
  exit(1);
}

/* ---- client-side columnar encoder (typed arrays -> NPCOLUMN -> /dev/shm) -- */
typedef struct { const char *name; int kind; uint32_t pgtype; } WColDef;
static const WColDef wcols[8] = {
    {"id", 2, 23}, {"account_id", 3, 20}, {"balance", 4, 701}, {"active", 1, 16},
    {"name", 5, 25}, {"email", 5, 25}, {"score", 4, 701}, {"age", 2, 23},
};

typedef struct { char *d; size_t len, cap; } Buf;
static void b_init(Buf *b) { b->d = NULL; b->len = b->cap = 0; }
static void b_put(Buf *b, const void *p, size_t n) {
  if (b->len + n > b->cap) {
    size_t c = b->cap ? b->cap : 1 << 16;
    while (c < b->len + n) c *= 2;
    b->d = realloc(b->d, c);
    b->cap = c;
  }
  memcpy(b->d + b->len, p, n);
  b->len += n;
}
static void b_u32(Buf *b, uint32_t v) { b_put(b, &v, 4); }
static void b_u64(Buf *b, uint64_t v) { b_put(b, &v, 8); }

/* Serialize `nrows` rows to /dev/shm/<seg>. Returns encode+write time (ms). */
static double produce_columnar_shm(const char *seg, long nrows) {
  double t0 = now_ms();
  Buf out;
  b_init(&out);
  int ncols = 8;
  char tmp[64];

  b_put(&out, "NPCOLUMN", 8);
  b_u32(&out, 1); b_u32(&out, 1); b_u32(&out, (uint32_t)ncols);
  b_u64(&out, (uint64_t)nrows);

  for (int c = 0; c < ncols; c++) {
    const WColDef *wc = &wcols[c];
    Buf data;
    b_init(&data);

    if (wc->kind == 5) {
      uint32_t *off = malloc((size_t)(nrows + 1) * 4);
      Buf bytes;
      b_init(&bytes);
      off[0] = 0;
      for (long r = 0; r < nrows; r++) {
        int len = (c == 4) ? snprintf(tmp, sizeof tmp, "user%ld", r)
                           : snprintf(tmp, sizeof tmp, "user%ld@example.com", r);
        b_put(&bytes, tmp, (size_t)len);
        off[r + 1] = (uint32_t)bytes.len;
      }
      for (long i = 0; i <= nrows; i++) b_u32(&data, off[i]);
      b_put(&data, bytes.d, bytes.len);
      free(off);
      free(bytes.d);
    } else {
      for (long r = 0; r < nrows; r++) {
        if (wc->kind == 1) {
          unsigned char v = (r & 1) == 0;
          b_put(&data, &v, 1);
        } else if (wc->kind == 2) {
          int32_t v = (c == 0) ? (int32_t)r : (int32_t)(r % 100);
          b_put(&data, &v, 4);
        } else if (wc->kind == 3) {
          int64_t v = r * 1000;
          b_put(&data, &v, 8);
        } else {
          double v = (c == 2) ? r * 0.5 : r * 0.25;
          b_put(&data, &v, 8);
        }
      }
    }

    b_u32(&out, (uint32_t)strlen(wc->name));
    b_put(&out, wc->name, strlen(wc->name));
    unsigned char kind = (unsigned char)wc->kind;
    b_put(&out, &kind, 1);
    b_u32(&out, wc->pgtype);
    size_t nb = (size_t)(nrows + 7) / 8;
    char *nulls = calloc(1, nb);
    b_put(&out, nulls, nb);
    free(nulls);
    b_u64(&out, (uint64_t)data.len);
    b_put(&out, data.d, data.len);
    free(data.d);
  }

  char path[256];
  snprintf(path, sizeof path, "/%s", seg);
  size_t seg_size = NP_SHM_HEADER_SIZE + out.len;
  int fd = shm_open(path, O_RDWR | O_CREAT, 0666);
  if (fd >= 0) {
    ftruncate(fd, (off_t)seg_size);
    void *map = mmap(NULL, seg_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map != MAP_FAILED) {
      (void) madvise(map, seg_size, MADV_HUGEPAGE);
      NpShmHeader *h = (NpShmHeader *)map;
      memcpy(h->magic, NP_SHM_MAGIC, 8);
      h->version = NP_SHM_VERSION;
      h->flags = 1;
      h->capacity = seg_size;
      memcpy((char *)map + NP_SHM_HEADER_SIZE, out.d, out.len);
      __sync_synchronize();
      h->payload_size = out.len;
      __sync_synchronize();
      h->seq++;
      munmap(map, seg_size);
    }
  }
  free(out.d);
  return now_ms() - t0;
}

/* ---- setup: create the bench table with nrows rows ---- */
static void setup_table(PGconn *conn, long nrows) {
  char setup[1024];
  snprintf(setup, sizeof(setup),
           "DROP TABLE IF EXISTS bench;"
           "CREATE TABLE bench AS "
           "SELECT g::int4 AS id, (g::int8 * 1000) AS account_id,"
           "       (g*1.5)::float8 AS balance, (g%%2=0)::bool AS active,"
           "       ('user'||g)::text AS name, ('user'||g||'@example.com')::text AS email,"
           "       (g*0.25)::float8 AS score, (g%%100)::int4 AS age "
           "FROM generate_series(1,%ld) g;",
           nrows);
  PGresult *r = PQexec(conn, setup);
  if (PQresultStatus(r) != PGRES_COMMAND_OK) fail(conn, "setup");
  PQclear(r);
}

/* ---- READ: libpq streaming (over any connection) ---- */
static double read_normal(PGconn *conn) {
  PGresult *r;
  long long sum = 0;

  double t0 = now_ms();
  if (!PQsendQuery(conn, QUERY)) fail(conn, "PQsendQuery");
  if (!PQsetSingleRowMode(conn)) fail(conn, "PQsetSingleRowMode");
  while ((r = PQgetResult(conn)) != NULL) {
    if (PQresultStatus(r) == PGRES_SINGLE_TUPLE) {
      int cols = PQnfields(r);
      for (int j = 0; j < cols; j++) sum += (long long)strlen(PQgetvalue(r, 0, j));
    } else if (PQresultStatus(r) == PGRES_FATAL_ERROR) {
      fprintf(stderr, "query failed: %s\n", PQresultErrorMessage(r));
      exit(1);
    }
    PQclear(r);
  }
  double ms = now_ms() - t0;
  (void)sum;
  return ms;
}

/* ---- READ: native columnar -> /dev/shm -> decode (control msg via conn) ---- */
static double read_native(PGconn *conn) {
  char shm_q[512];
  snprintf(shm_q, sizeof(shm_q),
           "SELECT native_query_shm('bench_seg', $q$%s$q$)", QUERY);

  double t0 = now_ms();
  PGresult *r = PQexec(conn, shm_q);
  if (PQresultStatus(r) != PGRES_TUPLES_OK) fail(conn, "native_query_shm");
  PQclear(r);

  int fd = shm_open("/bench_seg", O_RDONLY, 0);
  if (fd < 0) { perror("shm_open"); exit(1); }
  struct stat st;
  fstat(fd, &st);
  size_t sz = (size_t)st.st_size;
  uint8_t *map = (uint8_t *)mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED) { perror("mmap"); exit(1); }
  (void) madvise(map, sz, MADV_HUGEPAGE);

  const uint8_t *p = map + NP_SHM_HEADER_SIZE;
  p += 8; p += 4; p += 4; /* magic, version, flags */
  uint32_t ncols;
  uint64_t nr;
  memcpy(&ncols, p, 4); p += 4;
  memcpy(&nr, p, 8); p += 8;

  long long sum = 0;
  char *scratch = malloc((size_t)nr * 8);
  for (uint32_t c = 0; c < ncols; c++) {
    uint32_t name_len;
    memcpy(&name_len, p, 4); p += 4;
    p += name_len;
    uint8_t kind = *p++;
    p += 4; /* pgtype */
    p += (nr + 7) / 8; /* nulls */
    uint64_t data_len;
    memcpy(&data_len, p, 8); p += 8;
    const uint8_t *data = p;
    p += data_len;
    if (kind == 5) {
      const uint32_t *off = (const uint32_t *)data;
      const uint8_t *bytes = data + (nr + 1) * 4;
      for (uint64_t rr = 0; rr < nr; rr++) sum += bytes[off[rr]];
    } else {
      memcpy(scratch, data, data_len);
    }
  }
  double ms = now_ms() - t0;
  free(scratch);
  munmap(map, sz);
  (void)sum;
  return ms;
}

/* ---- WRITE: COPY dst FROM STDIN (client formats text + streams) ---- */
static double write_copy(PGconn *conn, long nrows) {
  PGresult *r = PQexec(conn, "DROP TABLE IF EXISTS dst; CREATE TABLE dst (LIKE bench);");
  if (PQresultStatus(r) != PGRES_COMMAND_OK) fail(conn, "dst setup");
  PQclear(r);

  double t0 = now_ms();
  r = PQexec(conn, "COPY dst FROM STDIN");
  if (PQresultStatus(r) != PGRES_COPY_IN) fail(conn, "COPY");
  PQclear(r);
  {
    char rowbuf[256];
    char bigbuf[1 << 20];
    size_t bi = 0;
    for (long rr = 0; rr < nrows; rr++) {
      int len = snprintf(rowbuf, sizeof rowbuf,
          "%ld\t%lld\t%.1f\t%c\tuser%ld\tuser%ld@example.com\t%.2f\t%ld\n",
          rr, (long long)(rr * 1000), rr * 0.5, ((rr & 1) == 0) ? 't' : 'f',
          rr, rr, rr * 0.25, rr % 100);
      if (bi + (size_t)len >= sizeof bigbuf) {
        PQputCopyData(conn, bigbuf, (int)bi);
        bi = 0;
      }
      memcpy(bigbuf + bi, rowbuf, (size_t)len);
      bi += (size_t)len;
    }
    if (bi) PQputCopyData(conn, bigbuf, (int)bi);
  }
  if (PQputCopyEnd(conn, NULL) != 1) fail(conn, "COPY end");
  r = PQgetResult(conn);
  if (PQresultStatus(r) != PGRES_COMMAND_OK) fail(conn, "COPY result");
  PQclear(r);
  return now_ms() - t0;
}

/* ---- WRITE: native columnar -> /dev/shm -> multi_insert ---- */
static double write_native(PGconn *conn, long nrows) {
  PGresult *r = PQexec(conn, "DROP TABLE IF EXISTS dst; CREATE TABLE dst (LIKE bench);");
  if (PQresultStatus(r) != PGRES_COMMAND_OK) fail(conn, "dst setup");
  PQclear(r);

  double enc_ms = produce_columnar_shm("wseg", nrows);
  double t0 = now_ms();
  r = PQexec(conn, "SELECT native_write_shm('wseg', 'dst')");
  if (PQresultStatus(r) != PGRES_TUPLES_OK) fail(conn, "native_write_shm");
  PQclear(r);
  return enc_ms + (now_ms() - t0);
}

static const char *fmt(long n) {
  static char b[32];
  if (n >= 1000000) snprintf(b, sizeof b, "%ldM", n / 1000000);
  else if (n >= 1000) snprintf(b, sizeof b, "%ldK", n / 1000);
  else snprintf(b, sizeof b, "%ld", n);
  return b;
}

/* direct heap scan vs SPI SELECT * (both produce the same columnar result) */
static void bench_scan(PGconn *conn, double *query_ms, double *scan_ms) {
  PGresult *r;
  double t0 = now_ms();
  r = PQexec(conn, "SELECT native_query_shm('q_seg', 'SELECT * FROM bench')");
  if (PQresultStatus(r) != PGRES_TUPLES_OK) fail(conn, "native_query_shm");
  PQclear(r);
  *query_ms = now_ms() - t0;

  t0 = now_ms();
  r = PQexec(conn, "SELECT native_scan_shm('s_seg', 'bench')");
  if (PQresultStatus(r) != PGRES_TUPLES_OK) fail(conn, "native_scan_shm");
  PQclear(r);
  *scan_ms = now_ms() - t0;
}

int main(int argc, char **argv) {
  long sizes[16];
  int nsizes = 0;
  for (int i = 1; i < argc && nsizes < 16; i++) sizes[nsizes++] = atol(argv[i]);
  if (nsizes == 0) {
    sizes[0] = 100000; sizes[1] = 1000000; sizes[2] = 10000000;
    sizes[3] = 100000000; nsizes = 4;
  }

  PGconn *tcp_conn = PQconnectdb("host=127.0.0.1 dbname=postgres");
  PGconn *unix_conn = PQconnectdb("host=/tmp dbname=postgres");
  if (PQstatus(tcp_conn) != CONNECTION_OK) fail(tcp_conn, "tcp connect");
  if (PQstatus(unix_conn) != CONNECTION_OK) fail(unix_conn, "unix connect");

  printf("READ (rows/s): %-8s %-8s %-8s %-10s %-10s\n",
         "tcp", "unix", "shm", "shm/tcp", "shm/unix");
  for (int i = 0; i < nsizes; i++) {
    long n = sizes[i];
    setup_table(unix_conn, n);
    double rt = read_normal(tcp_conn);
    double ru = read_normal(unix_conn);
    double rs = read_native(unix_conn);
    printf("  %-8s %8.1fM %8.1fM %8.1fM %8.2fx %9.2fx\n",
           fmt(n), n / (rt / 1000.0) / 1e6, n / (ru / 1000.0) / 1e6,
           n / (rs / 1000.0) / 1e6, rt / rs, ru / rs);
  }

  printf("\nWRITE (rows/s): %-8s %-8s %-8s %-10s %-10s\n",
         "tcp", "unix", "shm", "shm/tcp", "shm/unix");
  for (int i = 0; i < nsizes; i++) {
    long n = sizes[i];
    double wt = write_copy(tcp_conn, n);
    double wu = write_copy(unix_conn, n);
    double ws = write_native(unix_conn, n);
    printf("  %-8s %8.1fM %8.1fM %8.1fM %8.2fx %9.2fx\n",
           fmt(n), n / (wt / 1000.0) / 1e6, n / (wu / 1000.0) / 1e6,
           n / (ws / 1000.0) / 1e6, wt / ws, wu / ws);
  }

  printf("\nSCAN vs QUERY (rows/s): query(SPI)  scan(direct)  speedup\n");
  for (int i = 0; i < nsizes; i++) {
    long n = sizes[i];
    double qm, sm;
    setup_table(unix_conn, n);
    bench_scan(unix_conn, &qm, &sm);
    printf("  %-8s %8.1fM %8.1fM %6.2fx\n",
           fmt(n), n / (qm / 1000.0) / 1e6, n / (sm / 1000.0) / 1e6, qm / sm);
  }

  PQfinish(tcp_conn);
  PQfinish(unix_conn);
  return 0;
}