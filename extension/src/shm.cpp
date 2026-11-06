/* shm.cpp — the data segment's backing store on the worker side.
 *
 * The segment is an anonymous memfd, not a named /dev/shm file: the worker
 * ftruncates it once, maps it read-write, asks for huge pages before the first
 * write, seals the capacity (F_SEAL_SHRINK | F_SEAL_GROW) and hands the
 * descriptor to the client once over an AF_UNIX socket (SCM_RIGHTS). There is
 * no path to widen and no fchmod: the descriptor is the whole trust boundary.
 *
 * F_SEAL_WRITE is deliberately not set. The ring recycles the same mapping for
 * every batch (that is the point of keeping it mapped), and a write seal makes
 * the existing writable mapping fault on the next publish. Shrink/grow are the
 * two seals that matter for a reader: the object's size and identity cannot
 * change under it.
 *
 * The handoff socket is authenticated with SO_PEERCRED: only the ring's owning
 * client pid may receive descriptors, and the client checks the worker pid
 * before it accepts the stream.
 *
 * The original /dev/shm/<seg> protocol is the fallback, used when the client
 * opted out (FASTPOSTGRES_MEMFD=0), asked for the eventfd engine (whose pool
 * worker has no per-worker handoff), or the kernel cannot make a sealed memfd.
 * It keeps its 0666 widening there, because a client running as a different
 * user still has to reach it. A fallback is logged once, never silent. Both
 * paths keep one mapping per segment name for the worker's lifetime; the old
 * code mapped and unmapped the whole segment on every batch.
 */
#include "pgxx.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include "fastpostgres/fastpostgres_shm.h"
#include "fastpostgres/ring.h"
}
#include "shm.h"

namespace
{

/* A ring uses two data segments (double buffering); leave room for a few more
 * so a caller that names its own segments still gets the cache. */
constexpr int kWorkerSegs = 8;

struct FastpostgresWorkerSeg {
    char   path[FASTPOSTGRES_RING_ARG]; /* "/<seg>" */
    int    fd;                          /* memfd, or -1 for /dev/shm */
    void  *map;
    size_t cap;
};

/* The handoff record: name, size, and an optional descriptor. SOCK_SEQPACKET
 * preserves the record boundary, so the client reads exactly this one send. */
struct FastpostgresHandoff {
    uint32_t name_len;
    int32_t  has_fd;
    uint64_t size;
};

std::array<FastpostgresWorkerSeg, kWorkerSegs> worker_segs{};
int                                            worker_nsegs        = 0;
int                                            worker_memfd        = 0;
int                                            worker_fd_sock      = -1;
int                                            worker_memfd_warned = 0;

/* Send one segment's descriptor (or fd = -1 for "this one is /dev/shm") to the
 * client. A failed send is not fatal: the client's bounded wait falls back to
 * the named segment, and so does this side. */
int
handoff_send(const char *name, int fd, size_t size)
{
    FastpostgresHandoff                       hdr;
    struct msghdr                             msg;
    std::array<struct iovec, 2>               iov;
    std::array<char, CMSG_SPACE(sizeof(int))> control;
    ssize_t                                   n;

    if (worker_fd_sock < 0) return -1;

    hdr.name_len = static_cast<uint32_t>(std::strlen(name));
    hdr.has_fd   = fd >= 0 ? 1 : 0;
    hdr.size     = static_cast<uint64_t>(size);

    std::memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    iov[1].iov_base = const_cast<char *>(name);
    iov[1].iov_len  = hdr.name_len;
    msg.msg_iov     = iov.data();
    msg.msg_iovlen  = iov.size();

    if (fd >= 0)
    {
        std::memset(control.data(), 0, control.size());
        msg.msg_control    = control.data();
        msg.msg_controllen = control.size();

        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);

        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type  = SCM_RIGHTS;
        c->cmsg_len   = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }

    do
        n = sendmsg(worker_fd_sock, &msg, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : 0;
}

void
seg_destroy(FastpostgresWorkerSeg *s)
{
    if (s->map != nullptr && s->cap != 0) munmap(s->map, s->cap);
    if (s->fd >= 0) close(s->fd);
    s->map     = nullptr;
    s->fd      = -1;
    s->cap     = 0;
    s->path[0] = 0;
}

/* Say once why the memfd path could not be used. The fallback itself is
 * normal (an old kernel, a client that opted out); repeating it per segment
 * would drown the log. */
void
memfd_fallback_warn(const char *why)
{
    if (worker_memfd_warned) return;
    worker_memfd_warned = 1;
    elog(WARNING, "fastpostgres: sealed memfd unavailable (%s); data segments use /dev/shm", why);
}

/* Fill `s` with one segment: a sealed memfd first, /dev/shm only if the memfd
 * (or handing it over) is not possible. The fallback is deliberate and said
 * once, never silent: a client that believes it has a sealed object must not
 * be handed a named one without a record. */
int
seg_fill(FastpostgresWorkerSeg *s, const char *path, size_t size, size_t *cap_out)
{
    const char *name = path[0] == '/' ? path + 1 : path;

    std::memset(s, 0, sizeof(*s));
    s->fd = -1;

    if (worker_memfd)
    {
        int         fd  = memfd_create("fastpostgres-seg", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        void       *map = MAP_FAILED;
        const char *why = nullptr;

        if (fd < 0)
            why = "memfd_create";
        else if (ftruncate(fd, static_cast<off_t>(size)) != 0)
            why = "ftruncate";
        else if ((map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
            why = "mmap";
        else
        {
            madvise(map, size, MADV_HUGEPAGE); /* before the first write */

            /* The size and the object stay fixed; writes stay allowed because
             * the worker rewrites this same mapping for the next batch. An
             * unsealed object is not the contract, so it falls back rather
             * than being handed over with a warning. */
            if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0)
                why = "F_ADD_SEALS";
            else if (handoff_send(name, fd, size) == 0)
            {
                /* `path` with its leading slash, not `name`: the cache is keyed
                 * by the name the caller passes to fp::shm_map_rw. */
                fp::copy_bounded(s->path, sizeof(s->path), path);
                s->fd    = fd;
                s->map   = map;
                s->cap   = size;
                *cap_out = size;
                return 0;
            }
            else
                why = "fd handoff";
            munmap(map, size);
        }
        if (fd >= 0) close(fd);
        if (why != nullptr) memfd_fallback_warn(why);

        /* A descriptor is not coming for this segment: tell the client now so
         * it maps /dev/shm instead of spending its handoff wait. */
        handoff_send(name, -1, size);
    }

    {
        int         fd = shm_open(path, O_RDWR | O_CREAT, 0666);
        struct stat st;
        void       *map;

        if (fd < 0) return -1;
        fchmod(fd, 0666); /* the fallback's cross-user trust boundary, unchanged */
        if (fstat(fd, &st) != 0)
        {
            close(fd);
            return -1;
        }
        if (static_cast<size_t>(st.st_size) < size && ftruncate(fd, static_cast<off_t>(size)) != 0)
        {
            close(fd);
            return -1;
        }
        map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (map == MAP_FAILED) return -1;
        madvise(map, size, MADV_HUGEPAGE);

        fp::copy_bounded(s->path, sizeof(s->path), path);
        s->fd    = -1;
        s->map   = map;
        s->cap   = size;
        *cap_out = size;
        return 0;
    }
}

/* The handoff socket is a second trust boundary: its abstract address is
 * reachable by any local process, and whatever is sent over it becomes the
 * client's mapped object. The worker verifies the peer is the process that
 * owns the ring before it sends a descriptor; a mismatch keeps the named
 * /dev/shm path, which the ring's lifecycle already governs. */
int
handoff_peer_ok([[maybe_unused]] int s, [[maybe_unused]] const FastpostgresRing *ring)
{
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t    len = sizeof(cred);

    if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
    {
        elog(WARNING, "fastpostgres: fd handoff SO_PEERCRED failed: %m; using /dev/shm");
        return 0;
    }
    if (ring->client_pid != 0 && cred.pid != static_cast<pid_t>(ring->client_pid))
    {
        elog(WARNING, "fastpostgres: fd handoff peer pid %d is not the ring's client %u; using /dev/shm",
             static_cast<int>(cred.pid), ring->client_pid);
        return 0;
    }
#endif
    return 1;
}

} // namespace

char *
fp::shm_path(const char *seg)
{
    const size_t n    = std::strlen(seg);
    char        *path = static_cast<char *>(palloc(n + 2));

    path[0] = '/';
    std::memcpy(path + 1, seg, n + 1);
    return path;
}

void *
fp::shm_map_rw(const char *path, size_t size, size_t *cap_out)
{
    for (int i = 0; i < worker_nsegs; i++)
    {
        FastpostgresWorkerSeg *s = &worker_segs[i];

        if (std::strcmp(s->path, path) != 0) continue;
        if (s->cap >= size)
        {
            *cap_out = s->cap;
            return s->map;
        }

        /* the ask grew: a sealed memfd cannot grow, so replace the object */
        seg_destroy(s);
        if (seg_fill(s, path, size, cap_out) == 0) return s->map;
        return nullptr;
    }

    if (worker_nsegs >= kWorkerSegs) return nullptr;
    if (seg_fill(&worker_segs[worker_nsegs], path, size, cap_out) == 0)
    {
        worker_nsegs++;
        return worker_segs[worker_nsegs - 1].map;
    }
    return nullptr;
}

void
fp::shm_worker_begin(FastpostgresRing *ring)
{
    fp::shm_worker_end();

    if (ring->fd_handoff != 1 || ring->fd_pathlen == 0 || ring->fd_pathlen > FASTPOSTGRES_RING_FD_PATH)
        return;

    {
        struct sockaddr_un addr;
        socklen_t len = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + ring->fd_pathlen);
        int       s   = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

        if (s < 0) return;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, ring->fd_path, ring->fd_pathlen);

        const int rc = connect(s, reinterpret_cast<struct sockaddr *>(&addr), len);
        if (rc == 0 && handoff_peer_ok(s, ring))
        {
            /* The client's accept checks this pid, and it must be visible
             * before worker_ready. */
            __atomic_store_n(&ring->srv_pid, static_cast<uint32_t>(getpid()), __ATOMIC_RELEASE);
            worker_fd_sock = s;
            worker_memfd   = 1;
        }
        else
        {
            if (rc != 0) elog(WARNING, "fastpostgres: fd handoff connect failed: %m; using /dev/shm");
            close(s);
        }

        /* If the connect fails, worker_memfd stays 0 and both sides use
         * /dev/shm: the client's accept times out and it maps by name. */
    }
}

void
fp::shm_worker_end(void)
{
    /* Only the slots that were filled are released: an untouched slot still
     * holds fd == 0, which is the worker's own stdin, not a segment. */
    for (FastpostgresWorkerSeg &s : std::span(worker_segs).first(static_cast<size_t>(worker_nsegs)))
        seg_destroy(&s);
    worker_nsegs = 0;

    if (worker_fd_sock >= 0)
    {
        close(worker_fd_sock);
        worker_fd_sock = -1;
    }
    worker_memfd = 0;
}
