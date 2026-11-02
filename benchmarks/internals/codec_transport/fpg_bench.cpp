/* fpg_bench.cpp — the FPG format cell's CLI (SPEC.md).
 *
 *   fpg_bench --rows N --format fpcolumn|fprows --reps R [--emit-payload PATH] [--nt-store]
 *
 * Runs the format experiment in one process, no transport: encode the payload,
 * decode it into its natural in-memory form, compute the six aggregates; the
 * logical checksum is computed once outside the timed region. Prints exactly
 * one line of JSON on stdout, per the SPEC's schema.
 *
 * --nt-store is the one knob on the encode's store instruction: with it, the
 * columnar encoder writes its bulk column bytes with streaming (non-temporal)
 * stores instead of ordinary ones, which removes the destination's
 * read-for-ownership traffic. Off by default, and it changes no byte of the
 * payload — same payload_bytes, same checksum — so the A/B is the store
 * instruction and nothing else.
 *
 * Conventions this tool fixes where SPEC.md leaves room (kept here so the
 * other cells and the runner can mirror them):
 *   - encode_ms/decode_ms are the median over the R reps;
 *   - p50_ms/p99_ms are percentiles of the per-rep encode+decode total;
 *   - rows_per_s/gb_per_s are over that same encode+decode total;
 *   - rss_kb is peak resident set size (VmHWM), not the instant RSS.
 */
#include "fpg_codec.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>

/* fpg_codec.cpp's columnar encoder with the store instruction selectable:
 * `nt_store` writes the column data with non-temporal stores. Declared here
 * because fpg_codec.h is not this change's file; the payload is byte for byte
 * the same either way, which is what the --emit-payload comparison and the
 * checksum check. */
namespace fpg
{
uint64_t fpcolumn_encode(const Dataset &ds, char *out, uint64_t capacity, bool nt_store);
} /* namespace fpg */

namespace
{

void
usage(const char *argv0)
{
    std::fprintf(stderr,
                 "usage: %s --rows N --format fpcolumn|fprows --reps R [--emit-payload PATH] [--nt-store]\n"
                 "  --rows N              dataset rows (default 1000000)\n"
                 "  --format FORMAT       fpcolumn (default) or fprows\n"
                 "  --reps R              encode+decode repetitions; medians are reported (default 1)\n"
                 "  --emit-payload PATH   write the encoded payload bytes to PATH\n"
                 "  --nt-store            fpcolumn only: write the column data with non-temporal\n"
                 "                        (streaming) stores; no effect on fprows (default off)\n",
                 argv0);
}

[[noreturn]] void
die(const char *what)
{
    std::fprintf(stderr, "fpg_bench: %s\n", what);
    std::exit(2);
}

double
now_ms()
{
    using clock = std::chrono::steady_clock;

    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

double
median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    size_t n = v.size();

    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* Nearest-rank percentile over an already sorted vector. */
double
percentile(const std::vector<double> &sorted, double p)
{
    size_t n    = sorted.size();
    size_t rank = (size_t)std::ceil(p / 100.0 * (double)n);

    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}

double
tv_ms(const struct timeval &tv)
{
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* Peak resident set, so the number travels with the memory the cell needed. */
long
peak_rss_kb()
{
    FILE *f = std::fopen("/proc/self/status", "r");
    char  line[256];
    long  kb = 0;

    if (f == nullptr) return 0;
    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        if (std::strncmp(line, "VmHWM:", 6) == 0)
        {
            kb = std::strtol(line + 6, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return kb;
}

} /* namespace */

int
main(int argc, char **argv)
{
    uint64_t    rows      = 1000000;
    std::string format    = "fpcolumn";
    long        reps      = 1;
    const char *emit_path = nullptr;
    bool        nt_store  = false; /* --nt-store: off unless asked for */

    for (int i = 1; i < argc; i++)
    {
        std::string a   = argv[i];
        size_t      eq  = a.find('=');
        bool        has = eq != std::string::npos;
        std::string key = has ? a.substr(0, eq) : a;
        std::string val = has ? a.substr(eq + 1) : std::string();

        auto take = [&](const char *opt) -> const char * {
            if (key != opt) return nullptr;
            if (has) return val.c_str();
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                die("missing value");
            }
            return argv[++i];
        };
        const char *v;

        if ((v = take("--rows")))
        {
            char *endp = nullptr;
            rows       = std::strtoull(v, &endp, 10);
            if (endp == v || *endp != '\0' || rows < 1 || rows > 0x7fffffffull)
                die("--rows must be 1..2147483647");
        }
        else if ((v = take("--format")))
        {
            format = v;
            if (format != "fpcolumn" && format != "fprows") die("--format must be fpcolumn or fprows");
        }
        else if ((v = take("--reps")))
        {
            char *endp = nullptr;
            reps       = std::strtol(v, &endp, 10);
            if (endp == v || *endp != '\0' || reps < 1 || reps > 100000)
                die("--reps must be a positive integer");
        }
        else if ((v = take("--emit-payload")))
        {
            emit_path = v;
        }
        else if (key == "--nt-store")
        {
            if (has) die("--nt-store takes no value");
            nt_store = true;
        }
        else if (key == "--help" || key == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            usage(argv[0]);
            die("unknown argument");
        }
    }

    const bool fpcolumn = format == "fpcolumn";

    fpg::Dataset ds = fpg::generate(rows);

    const uint64_t capacity = fpcolumn ? fpg::fpcolumn_capacity(ds) : fpg::fprows_capacity(ds);
    void          *mem      = nullptr;

    if (posix_memalign(&mem, 64, (size_t)capacity) != 0) die("cannot allocate the payload buffer");
    uint8_t *buf = static_cast<uint8_t *>(mem);

    std::vector<double> enc_ms, dec_ms;
    enc_ms.reserve((size_t)reps);
    dec_ms.reserve((size_t)reps);

    uint64_t        payload_bytes = 0;
    fpg::FpView     last_view{};
    fpg::Aggregates last_agg{};

    for (long r = 0; r < reps; r++)
    {
        double t0 = now_ms();
        /* --nt-store selects the store instruction, never the bytes: it is the
         * columnar encoder's knob, since the row frame copies no bulk arrays. */
        uint64_t n  = fpcolumn ? fpg::fpcolumn_encode(ds, reinterpret_cast<char *>(buf), capacity, nt_store)
                               : fpg::fprows_encode(ds, reinterpret_cast<char *>(buf), capacity);
        double   t1 = now_ms();

        if (r == 0)
            payload_bytes = n;
        else if (n != payload_bytes)
            die("the payload size changed between reps");

        fpg::FpView view;
        bool        ok = fpcolumn ? fpg::fpcolumn_decode(buf, n, &view) : fpg::fprows_decode(buf, n, &view);

        if (!ok) die("decode rejected the frame the encoder produced");

        fpg::Aggregates agg = fpg::aggregate(view);
        double          t2  = now_ms();

        enc_ms.push_back(t1 - t0);
        dec_ms.push_back(t2 - t1);
        last_view = view; /* aliases buf; valid until the next encode overwrites it */
        last_agg  = agg;
    }

    /* The checksum is computed once, outside the timed region (SPEC.md), from
     * the decoded view. Comparing its shared components with the last timed
     * run also proves the aggregate loop was not optimized away. */
    fpg::Checksum chk = fpg::checksum(last_view);

    if (chk.sum_id != last_agg.sum_id || chk.sum_stock != last_agg.sum_stock || chk.trues != last_agg.trues ||
        chk.text_bytes != last_agg.text_bytes)
        die("the timed aggregates disagree with the checksum pass");

    if (emit_path != nullptr)
    {
        /* The payload is complete before it leaves the encoder: fpcolumn_encode
         * fences the streamed stores it made (see copy_nt in fpg_codec.cpp), so
         * this write cannot observe a half-drained write-combining buffer. */
        FILE *f = std::fopen(emit_path, "wb");

        if (f == nullptr) die("cannot open --emit-payload path");
        if (std::fwrite(buf, 1, (size_t)payload_bytes, f) != (size_t)payload_bytes)
            die("short write on --emit-payload");
        if (std::fclose(f) != 0) die("cannot close --emit-payload path");
    }

    const double enc_med  = median(enc_ms);
    const double dec_med  = median(dec_ms);
    const double total_ms = enc_med + dec_med;
    const double total_s  = total_ms > 0.0 ? total_ms / 1000.0 : 0.0;
    const double rows_s   = total_s > 0.0 ? (double)rows / total_s : 0.0;
    const double gb_s     = total_s > 0.0 ? ((double)payload_bytes / 1e9) / total_s : 0.0;

    std::vector<double> totals;
    totals.reserve(enc_ms.size());
    for (size_t i = 0; i < enc_ms.size(); i++)
        totals.push_back(enc_ms[i] + dec_ms[i]);
    std::sort(totals.begin(), totals.end());

    struct rusage ru;
    std::memset(&ru, 0, sizeof(ru));
    (void)getrusage(RUSAGE_SELF, &ru);

    std::printf(
        "{\"cell\": \"%s\", \"rows\": %" PRIu64 ", \"payload_bytes\": %" PRIu64 ", \"encode_ms\": %.3f, "
        "\"decode_ms\": %.3f, \"rows_per_s\": %.1f, \"gb_per_s\": %.6f, \"p50_ms\": %.3f, \"p99_ms\": %.3f, "
        "\"nt_store\": %s, "
        "\"copies\": %d, \"copy_stages\": {\"values->payload\": %d, \"payload->decoded-views\": %d, "
        "\"decoded-views->aggregates\": %d}, \"cpu_user_ms\": %.1f, \"cpu_sys_ms\": %.1f, \"rss_kb\": %ld, "
        "\"checksum\": {\"sum_id\": %" PRIu64 ", \"sum_stock\": %" PRIu64 ", \"sum_price_bits\": %" PRIu64
        ", \"trues\": %" PRIu64 ", \"text_hash\": %" PRIu64 "}}\n",
        format.c_str(), rows, payload_bytes, enc_med, dec_med, rows_s, gb_s, percentile(totals, 50.0),
        percentile(totals, 99.0), nt_store ? "true" : "false", fpg::kCopies, fpg::kCopiesValuesToPayload,
        fpg::kCopiesPayloadToViews, fpg::kCopiesViewsToAggregate, tv_ms(ru.ru_utime), tv_ms(ru.ru_stime),
        peak_rss_kb(), chk.sum_id, chk.sum_stock, chk.sum_price_bits, chk.trues, chk.text_hash);

    std::free(mem);
    return 0;
}
