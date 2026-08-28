/*
 * part8_recovery_server.c -- VSF TR-06-4 Part 8 recovery server (headend, x86)
 *
 * Milestone 1. Ingests the FULL downlink multiplex as raw TS over UDP, hands it
 * to a RIST sender unchanged, and builds a PCR CATALOGUE as each RTP payload is
 * formed, so that a request addressed by PCR can later be resolved to a range of
 * RTP sequence numbers and served from librist's existing retransmission buffer.
 *
 * A SEPARATE PROCESS ON PURPOSE. librist keeps FSR state (fsr_peers[],
 * satellite_mode_active) in udp.c file-scope statics. Those are per-process even
 * against a shared librist.so, so running this as its own binary is what keeps
 * it from disturbing the working Part 6/7 units. Nothing here is shared with
 * them at process level, and nothing here modifies FSR.
 *
 * WHY THE SENDER EMITS NOTHING UNTIL ASKED
 * The output peer listens with weight=1000, children inherit that weight, and
 * weight-1000 peers sit in the FSR-gated branch. So this server is silent until
 * an FSR Enable arrives -- the required Part 8 behaviour, achieved by existing
 * librist logic rather than by anything added here.
 *
 * THE 16-BIT CEILING (read before raising the buffer)
 * librist has no extended sequence: rist.c masks the RTP sequence to 16 bits
 * ("//When we support 32bit seq this should be changed") and the retransmission
 * index is uint32_t seq_index[UINT16_SIZE], looked up as seq_index[(uint16_t)seq].
 * So the buffer is safe only while it holds FEWER THAN 65536 payloads. At the
 * full-multiplex rate that is about 17.2 seconds. Past it, two resident payloads
 * share a 16-bit sequence, the index keeps only the newer, and the integrity
 * check in rist_retry_dequeue compares two 16-bit values that are EQUAL -- so it
 * passes and serves content from the previous pass. It fails silently, with
 * plausible wrong data.
 *
 * That ceiling is reached by doing the sensible thing: Part 8 Section 5 sizes the
 * buffer for satellite RTT plus worst-case internet latency across the whole
 * fleet, and field RTT on the bad mode of a bimodal link ran 1.0-1.8 s. Raising
 * the buffer to 15 s at this rate puts us at ~57000 of 65536.
 *
 * Hence two independent tripwires below. Widening the index inside librist is
 * the real fix; it is DEFERRED, NOT REJECTED, and belongs in a milestone that can
 * retest the Part 7 units that share that code.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <grp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <librist/librist.h>

/* -------------------------------------------------------------- constants */
#define TS_PACKET_SIZE      188
#define TS_PER_RTP          7
#define RTP_PAYLOAD_SIZE    (TS_PER_RTP * TS_PACKET_SIZE)      /* 1316 */
#define TS_SYNC             0x47

#define PCR_HZ              90000ULL
#define PCR_MASK33          ((1ULL << 33) - 1)
#define PCR_HALF33          (1ULL << 32)

#define MAX_PIDS            8192
#define PCR_RING_CAP        2048        /* ~80 s at 25 PCR/s; costs 48 KB/PID */

#define SEQ_SPACE           65536ULL    /* the hard ceiling -- see header */
#define TRIPWIRE_WARN_PCT   75          /* of SEQ_SPACE, per the spec */

/* A backward PCR step larger than this starts a new epoch. Sized well above
 * mux jitter (~10 ms) and far below any real discontinuity. */
#define DISC_BACK_TICKS     (PCR_HZ / 100)

/* A forward PCR step larger than this is REPORTED but does not start an epoch:
 * a forward jump leaves the ring sorted, so the nearest-match still works.
 * One second is roughly 25x the largest legitimate PCR interval measured on this
 * transponder (40.94 ms) and 10x the DVB maximum, so a PID merely going quiet
 * for a moment will not trip it -- which matters, because O3's value depends on
 * a discontinuity report meaning something. */
#define DISC_FWD_TICKS      (PCR_HZ)

/* Per-PID floor between discontinuity log lines. A source stuck flapping must
 * not be able to fill the journal; the event ring and the counters stay exact
 * either way, only the log lines are thinned. */
#define DISC_LOG_MIN_GAP_US 1000000ULL

#define DEFAULT_BUFFER_MS   10000
#define DEFAULT_RCVBUF      (32 * 1024 * 1024)
#define DEFAULT_DEBUG_SOCK  "/tmp/part8_recovery.sock"

/* ------------------------------------------------------------------ types */
struct pcr_entry {
	uint64_t pcr_base;      /* 33-bit PCR base */
	uint64_t ext_seq;       /* our 64-bit extended sequence */
	uint32_t epoch;
	uint8_t  slot;          /* which of the 7 TS packets carried it (0-6) */
};

struct pcr_ring {
	struct pcr_entry *e;
	size_t   cap;
	size_t   count;         /* entries resident, <= cap */
	size_t   head;          /* index of next write */
	uint32_t epoch;
	uint64_t last_base;
	bool     have_last;

	/* B5 telemetry */
	uint64_t total_entries;
	uint32_t epoch_changes;
	uint64_t last_wall_us;
	uint64_t iv_min, iv_max, iv_sum, iv_n;   /* PCR interval, microseconds */
};

/* ---------------------------------------------------------------- globals */
static struct rist_ctx *sender_ctx = NULL;
static struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;
static volatile int running = 1;

static struct pcr_ring *rings[MAX_PIDS];      /* lazily allocated per PID */
static pthread_mutex_t catalogue_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t g_ext_seq = 0;                /* we own the sequence space */
static uint64_t g_payloads_written = 0;
static uint64_t g_ts_packets_in = 0;
static uint64_t g_bytes_in = 0;
static uint64_t g_sync_errors = 0;
static uint64_t g_requests_outside_buffer = 0;

/* Identity and configuration reported to the GUI, so a panel can prove it is
 * showing the instance it thinks it is rather than whichever socket answered. */
static char     g_instance_name[64] = "default";
static bool     g_require_selection = true;
static int      g_rcvbuf_got = 0;
static bool     g_rcvbuf_clamped = false;

/*
 * Recent-window input rate.
 *
 * Tripwire B judges against the LIFETIME average, which is right for it: a
 * ceiling breach is about sustained rate. It is wrong for an operator display,
 * where hours of healthy history would mask a feed that died ten minutes ago.
 * Both are exposed, and neither is a compile-time constant -- each instance
 * carries a different transponder and must derive its own.
 */
static double   g_rate_window_bps = 0.0;
static uint64_t g_window_start_us = 0;
static uint64_t g_window_start_bytes = 0;

/* Per-peer figures lifted from the librist stats callback. Quality and RTT come
 * free from it, so nothing here re-derives them. */
#define MAX_TRACKED_PEERS 16
struct peer_stat {
	uint32_t id;
	char     cname[64];
	char     type[8];            /* "data" or "rtcp" */
	double   quality;
	double   rtt_ms;
	double   avg_rtt_ms;
	uint64_t retransmitted;
	uint64_t sent;
	uint64_t received;
	uint64_t bandwidth;
	uint64_t last_seen_us;       /* for expiry: a peer that stops reporting is gone */
};
static struct peer_stat g_peers[MAX_TRACKED_PEERS];
static int g_peer_count = 0;
static pthread_mutex_t peer_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Discontinuity event log (O3).
 *
 * The epoch handling in catalogue_insert() and the nearest-across-all-epochs
 * search in resolve() have only ever been exercised synthetically -- the
 * captures available so far contained no discontinuities at all. This records
 * every one seen on a live feed so the logic can finally be checked against real
 * data, and so an observer can resolve either side of the event afterwards
 * rather than having to catch it live in the log.
 */
#define DISC_LOG_CAP 256
struct disc_event {
	uint64_t wall_us;          /* CLOCK_MONOTONIC, matched to the log stamps */
	uint64_t wall_real;        /* seconds since the epoch, for correlation   */
	uint16_t pid;
	uint8_t  kind;             /* 0 = indicator (no PCR), 1 = PCR indicator,
	                              2 = backward jump, 3 = forward jump        */
	uint32_t epoch;            /* epoch AFTER the event                      */
	uint64_t pcr_before, pcr_after;
	int64_t  delta;
	uint64_t ext_seq;
};
static struct disc_event g_disc[DISC_LOG_CAP];
static size_t   g_disc_head = 0;
static uint64_t g_disc_total = 0;
static uint64_t g_disc_indicator_nopcr = 0;
static pthread_mutex_t disc_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *disc_kind_str(uint8_t k)
{
	switch (k) {
	case 0: return "indicator-no-pcr";
	case 1: return "indicator-with-pcr";
	case 2: return "backward-jump";
	case 3: return "forward-jump";
	}
	return "?";
}


static int      g_buffer_ms = DEFAULT_BUFFER_MS;
static uint64_t g_start_us = 0;

/* Tripwire (a): librist's OWN resident count, via the stats callback. Not our
 * estimate of it -- our estimate is the thing that could be wrong. */
static uint64_t g_queue_size_now = 0;
static uint64_t g_queue_size_max = 0;         /* pessimistic: the high-water mark */
static uint64_t g_queue_time_ms = 0;
static uint64_t g_queue_bytesize = 0;
static bool     g_have_queue_stats = false;
static bool     g_tripwire_a_fired = false;
static bool     g_tripwire_b_fired = false;

/* --------------------------------------------------------------- helpers */
static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/*
 * Shutdown self-pipe.
 *
 * Clearing `running` is not enough on its own: the debug thread parks in a
 * blocking accept() and only rechecks the flag when a connection happens to
 * arrive, so pthread_join() in main never returns and the process hangs after
 * logging [STOP]. It keeps its UDP sockets the whole time, so the next start
 * cannot bind and talks to nothing -- and systemd's SIGKILL on timeout leaves
 * the same orphan across a restart.
 *
 * write() is async-signal-safe, so the handler can poke the pipe directly and
 * every blocking wait can select on it.
 */
static int shutdown_pipe[2] = { -1, -1 };

static void disc_record(uint16_t pid, uint8_t kind, uint32_t epoch,
                        uint64_t before, uint64_t after, int64_t delta,
                        uint64_t ext_seq)
{
	struct timespec rt;
	clock_gettime(CLOCK_REALTIME, &rt);
	pthread_mutex_lock(&disc_lock);
	struct disc_event *e = &g_disc[g_disc_head];
	e->wall_us    = now_us();
	e->wall_real  = (uint64_t)rt.tv_sec;
	e->pid        = pid;
	e->kind       = kind;
	e->epoch      = epoch;
	e->pcr_before = before;
	e->pcr_after  = after;
	e->delta      = delta;
	e->ext_seq    = ext_seq;
	g_disc_head = (g_disc_head + 1) % DISC_LOG_CAP;
	g_disc_total++;
	pthread_mutex_unlock(&disc_lock);
}

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
	if (shutdown_pipe[1] >= 0) {
		char c = 1;
		(void)!write(shutdown_pipe[1], &c, 1);
	}
}

/*
 * Signed difference between two 33-bit PCR bases, valid across the ~26.5 hour
 * wrap (2^33 / 90000). Never compare PCR values with plain < -- a stream that
 * has been up for a day would sort backwards at the wrap and the catalogue
 * would silently unsort.
 */
static int64_t pcr_diff(uint64_t a, uint64_t b)
{
	uint64_t d = (a - b) & PCR_MASK33;
	if (d >= PCR_HALF33)
		return (int64_t)d - (int64_t)(PCR_MASK33 + 1);
	return (int64_t)d;
}

static uint64_t pcr_abs_diff(uint64_t a, uint64_t b)
{
	int64_t d = pcr_diff(a, b);
	return (uint64_t)(d < 0 ? -d : d);
}

/* ------------------------------------------------------------ PCR parsing */
/*
 * Extract the PCR from one TS packet, if it carries one.
 *
 * The guards matter more than the extraction. A corrupt packet whose bytes
 * happen to look like an adaptation field yields a perfectly plausible PCR that
 * would then poison the catalogue and be impossible to distinguish later, so
 * every structural constraint is checked before the value is trusted:
 *   AFC == 2 (adaptation only)  -> the AF must fill the packet: length == 183
 *   AFC == 3 (adaptation+payload) -> length <= 182, else it overruns
 *   a PCR needs the flags byte plus 6 PCR bytes -> length >= 7
 */
/*
 * Adaptation-field discontinuity_indicator on ANY packet, PCR-bearing or not.
 *
 * catalogue_insert() only ever sees packets that carry a PCR, so an indicator
 * set on a plain payload packet -- which is the common case, since the flag
 * marks the start of the discontinuity and the next PCR may be several packets
 * later -- would never be recorded at all. That matters here specifically:
 * validating the epoch logic against real data means knowing a discontinuity
 * happened even when the catalogue did not act on it.
 */
static bool ts_discontinuity_flag(const uint8_t *p, uint16_t *pid_out)
{
	if (p[0] != TS_SYNC)
		return false;
	uint8_t afc = (uint8_t)((p[3] >> 4) & 0x03);
	if (!(afc & 0x2))
		return false;
	if (p[4] == 0)
		return false;                       /* stuffing-only AF, no flag byte */
	if (afc == 2 && p[4] != 183)
		return false;
	if (afc == 3 && p[4] > 182)
		return false;
	if (!(p[5] & 0x80))
		return false;
	*pid_out = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
	return true;
}

static bool ts_extract_pcr(const uint8_t *p, uint16_t *pid_out,
                           uint64_t *base_out, bool *disc_out)
{
	if (p[0] != TS_SYNC)
		return false;

	uint8_t afc = (uint8_t)((p[3] >> 4) & 0x03);
	if (!(afc & 0x2))
		return false;                       /* no adaptation field */

	uint8_t af_len = p[4];
	if (af_len == 0)
		return false;                       /* stuffing-only AF, no flags */

	if (afc == 2 && af_len != 183)
		return false;
	if (afc == 3 && af_len > 182)
		return false;
	if (af_len < 7)
		return false;                       /* cannot hold flags + 6 PCR bytes */

	uint8_t flags = p[5];
	if (!(flags & 0x10))
		return false;                       /* PCR_flag clear */

	uint64_t base = ((uint64_t)p[6]  << 25) |
	                ((uint64_t)p[7]  << 17) |
	                ((uint64_t)p[8]  <<  9) |
	                ((uint64_t)p[9]  <<  1) |
	                ((uint64_t)p[10] >>  7);

	*pid_out  = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
	*base_out = base & PCR_MASK33;
	*disc_out = (flags & 0x80) != 0;        /* discontinuity_indicator */
	return true;
}

/* --------------------------------------------------------- catalogue core */
static struct pcr_ring *ring_get(uint16_t pid, bool create)
{
	if (pid >= MAX_PIDS)
		return NULL;
	if (!rings[pid] && create) {
		struct pcr_ring *r = calloc(1, sizeof(*r));
		if (!r)
			return NULL;
		r->e = calloc(PCR_RING_CAP, sizeof(struct pcr_entry));
		if (!r->e) { free(r); return NULL; }
		r->cap = PCR_RING_CAP;
		r->iv_min = UINT64_MAX;
		rings[pid] = r;
		rist_log(&logging_settings, RIST_LOG_INFO,
			"[CATALOGUE] first PCR on pid 0x%04X -- ring created (%zu entries)\n",
			pid, r->cap);
	}
	return rings[pid];
}

/* Oldest-to-newest logical index i (0 .. count-1) -> physical ring slot. */
static inline struct pcr_entry *ring_at(struct pcr_ring *r, size_t i)
{
	size_t start = (r->head + r->cap - r->count) % r->cap;
	return &r->e[(start + i) % r->cap];
}

static void catalogue_insert(uint16_t pid, uint64_t base, bool disc,
                             uint64_t ext_seq, uint8_t slot)
{
	struct pcr_ring *r = ring_get(pid, true);
	if (!r)
		return;

	uint64_t wall = now_us();

	/* Epoch handling. A set discontinuity_indicator, or a backward step beyond
	 * jitter, means the source changed under us. Start a new epoch rather than
	 * letting the ring become unsorted -- an unsorted ring makes the binary
	 * search return an arbitrary neighbour rather than the nearest PCR. */
	if (r->have_last) {
		int64_t d = pcr_diff(base, r->last_base);
		if (disc || d < -(int64_t)DISC_BACK_TICKS) {
			r->epoch++;
			r->epoch_changes++;
			rist_log(&logging_settings, RIST_LOG_WARN,
				"[DISC] pid 0x%04X %s: %" PRId64 " ticks (%.1f ms), "
				"pcr %" PRIu64 " -> %" PRIu64 ", entering epoch %" PRIu32
				", ext_seq %" PRIu64 "\n",
				pid, disc ? "indicator set" : "backward jump", d,
				(double)d * 1000.0 / (double)PCR_HZ,
				r->last_base, base, r->epoch, ext_seq);
			disc_record(pid, disc ? 1 : 2, r->epoch, r->last_base, base, d, ext_seq);
		} else if (d > (int64_t)DISC_FWD_TICKS) {
			/*
			 * A large forward jump is a discontinuity too, but it does NOT need a
			 * new epoch: the ring stays sorted, so the nearest-match still works.
			 * Recorded rather than acted on -- O3 wants to know it happened, and
			 * silently starting an epoch here would split a still-searchable
			 * segment for no benefit.
			 */
			rist_log(&logging_settings, RIST_LOG_WARN,
				"[DISC] pid 0x%04X forward jump: +%" PRId64 " ticks (%.1f ms), "
				"pcr %" PRIu64 " -> %" PRIu64 ", epoch %" PRIu32 " retained, "
				"ext_seq %" PRIu64 "\n",
				pid, d, (double)d * 1000.0 / (double)PCR_HZ,
				r->last_base, base, r->epoch, ext_seq);
			disc_record(pid, 3, r->epoch, r->last_base, base, d, ext_seq);
		} else if (r->iv_n < UINT64_MAX) {
			uint64_t iv = wall - r->last_wall_us;
			if (iv < r->iv_min) r->iv_min = iv;
			if (iv > r->iv_max) r->iv_max = iv;
			r->iv_sum += iv;
			r->iv_n++;
		}
	}

	struct pcr_entry *e = &r->e[r->head];
	e->pcr_base = base;
	e->ext_seq  = ext_seq;
	e->epoch    = r->epoch;
	e->slot     = slot;

	r->head = (r->head + 1) % r->cap;
	if (r->count < r->cap)
		r->count++;

	r->last_base    = base;
	r->last_wall_us = wall;
	r->have_last    = true;
	r->total_entries++;
}

/* ------------------------------------------------------------- resolution */
/*
 * Resolution outcome.
 *
 * This is deliberately a status, not a bool. A nearest-PCR search on a
 * non-empty ring ALWAYS finds something, so "did we find an entry" and "is that
 * entry any use to the caller" are different questions. Collapsing them into
 * one flag invites exactly one bug:
 *
 *     if (rr.ok) send_retransmission(rr.start_ext, rr.end_ext);
 *
 * which will cheerfully serve a range an hour away from what was asked for,
 * because the only thing saying otherwise was a free-text note nobody read.
 * Only RESOLVE_OK means "serve this range".
 */
enum resolve_status {
	/* NOT zero. resolve() memsets its result, and a zeroed struct must never
	 * come out meaning "serve this". Zero is the unset state and is not OK. */
	RESOLVE_UNSET = 0,
	RESOLVE_OK,                /* usable: start_ext..end_ext may be served */
	RESOLVE_NO_CATALOGUE,      /* no ring for this PID at all */
	RESOLVE_NO_ENTRY,          /* ring exists but every epoch came up empty */
	RESOLVE_BEFORE_EPOCH,      /* request predates the epoch we resolved into */
	RESOLVE_OUTSIDE_BUFFER,    /* nearest PCR lies further away than the buffer */
	RESOLVE_TOO_OLD,           /* found it, but it has aged out of the buffer */
};

static const char *resolve_status_str(enum resolve_status s)
{
	switch (s) {
	case RESOLVE_UNSET:          return "UNSET";
	case RESOLVE_OK:             return "OK";
	case RESOLVE_NO_CATALOGUE:   return "NO_CATALOGUE";
	case RESOLVE_NO_ENTRY:       return "NO_ENTRY";
	case RESOLVE_BEFORE_EPOCH:   return "BEFORE_EPOCH";
	case RESOLVE_OUTSIDE_BUFFER: return "OUTSIDE_BUFFER";
	case RESOLVE_TOO_OLD:        return "TOO_OLD";
	}
	return "UNKNOWN";
}

struct resolve_result {
	enum resolve_status status;
	/*
	 * Populated whenever an entry was found at all, INCLUDING on BEFORE_EPOCH
	 * and OUTSIDE_BUFFER. On any non-OK status they are diagnostic only: they
	 * say what the nearest thing was, not what may be sent.
	 */
	uint64_t start_ext, end_ext;
	uint64_t actual_pcr;
	uint32_t epoch;
	uint8_t  slot;
	bool     end_estimated;      /* live edge: end came from the rate fallback */
	int64_t  pcr_error;          /* actual - requested, signed ticks */
	char     note[256];
};

/*
 * How many of the most recent payloads are still in librist's retransmission
 * buffer, and therefore still servable.
 *
 * Preferred source is librist's own reported queue size -- our estimate of it is
 * the thing that could be wrong. Before the first stats callback, or while no
 * peer is attached and the queue is genuinely empty, fall back to what the
 * measured input rate says WOULD be resident, so the interface stays usable for
 * diagnosis rather than declaring everything unservable.
 */
static uint64_t resident_payloads(bool *estimated)
{
	if (g_have_queue_stats && g_queue_size_now) {
		if (estimated) *estimated = false;
		return g_queue_size_now;
	}
	/*
	 * Fallback uses the LIFETIME rate, not the recent window. The window decays
	 * the moment input pauses, which shrinks the estimate and makes recent,
	 * genuinely servable entries look aged out -- a false TOO_OLD is a refusal
	 * of a request that would have worked. The lifetime average is steady.
	 *
	 * This path only matters with no peer attached, where librist is holding
	 * nothing anyway and the figure is for diagnosis rather than a serving
	 * decision; once a peer is attached, g_queue_size_now above is exact.
	 */
	if (estimated) *estimated = true;
	uint64_t el = now_us() - g_start_us;
	double bps = el ? (double)g_bytes_in * 8.0 * 1000000.0 / (double)el : 0.0;
	double pps = bps / (188.0 * 8.0) / (double)TS_PER_RTP;
	return (uint64_t)(pps * (double)g_buffer_ms / 1000.0);
}

/* The single test a retransmission path should make. */
static inline bool resolve_serviceable(const struct resolve_result *rr)
{
	return rr->status == RESOLVE_OK;
}

/*
 * Nearest entry to `base` within one epoch, by absolute modular difference.
 *
 * Part 8 Section 6 asks for the CLOSEST PCR, not an exact match, so this
 * deliberately does not fail when there is no exact hit. The reference PCR sits
 * somewhere inside its 7-packet payload, so starting there resends up to 6 TS
 * packets the receiver already has. That is expected and harmless; do not
 * engineer it away, it is what makes the resolution robust.
 */
static bool ring_nearest_in_epoch(struct pcr_ring *r, uint32_t epoch,
                                  uint64_t base, size_t *idx_out)
{
	size_t lo = 0, hi = 0;
	bool found = false;

	/* Bound the epoch's contiguous logical range. */
	for (size_t i = 0; i < r->count; i++) {
		if (ring_at(r, i)->epoch == epoch) {
			if (!found) { lo = i; found = true; }
			hi = i;
		}
	}
	if (!found)
		return false;

	/* Binary search on the sign of the modular difference. Within an epoch the
	 * PCR is monotonic, and pcr_diff stays correct across the 33-bit wrap, so
	 * this is valid even for a ring that straddles it. */
	size_t a = lo, b = hi;
	while (a < b) {
		size_t mid = a + (b - a) / 2;
		if (pcr_diff(ring_at(r, mid)->pcr_base, base) < 0)
			a = mid + 1;
		else
			b = mid;
	}

	/* a is the first entry at or after base; the nearest is a or a-1. */
	size_t best = a;
	uint64_t best_d = pcr_abs_diff(ring_at(r, a)->pcr_base, base);
	if (a > lo) {
		uint64_t d = pcr_abs_diff(ring_at(r, a - 1)->pcr_base, base);
		if (d < best_d) { best = a - 1; best_d = d; }
	}
	*idx_out = best;
	return true;
}

static struct resolve_result resolve(uint16_t pid, uint64_t base, uint64_t duration)
{
	struct resolve_result rr;
	memset(&rr, 0, sizeof(rr));

	pthread_mutex_lock(&catalogue_lock);
	struct pcr_ring *r = ring_get(pid, false);
	if (!r || r->count == 0) {
		pthread_mutex_unlock(&catalogue_lock);
		rr.status = RESOLVE_NO_CATALOGUE;
		snprintf(rr.note, sizeof(rr.note), "no PCR catalogue for pid 0x%04X", pid);
		return rr;
	}

	/* Nearest match across every epoch resident in the ring, preferring the
	 * NEWEST epoch on a tie.
	 *
	 * "Search newest first" cannot mean "return the newest epoch's nearest and
	 * stop": a nearest-search always succeeds on a non-empty epoch, so a
	 * request that plainly belongs to an older segment would be answered from
	 * the newest one with a wildly wrong PCR. The ambiguity that ordering is
	 * meant to resolve is a PCR value that exists in BOTH epochs after a
	 * backward jump, and that is a tie, not a miss. So: best absolute modular
	 * difference wins, newest epoch breaks ties. */
	size_t idx = 0;
	bool found = false;
	uint64_t best_diff = 0;

	uint32_t seen[16];
	size_t n_seen = 0;
	for (size_t i = 0; i < r->count; i++) {
		uint32_t ep = ring_at(r, i)->epoch;
		bool have = false;
		for (size_t j = 0; j < n_seen; j++)
			if (seen[j] == ep) { have = true; break; }
		if (!have && n_seen < 16)
			seen[n_seen++] = ep;
	}

	for (size_t j = 0; j < n_seen; j++) {
		size_t cand;
		if (!ring_nearest_in_epoch(r, seen[j], base, &cand))
			continue;
		uint64_t d = pcr_abs_diff(ring_at(r, cand)->pcr_base, base);
		if (!found || d < best_diff ||
		    (d == best_diff && ring_at(r, cand)->epoch > ring_at(r, idx)->epoch)) {
			best_diff = d;
			idx = cand;
			found = true;
		}
	}
	if (!found) {
		pthread_mutex_unlock(&catalogue_lock);
		rr.status = RESOLVE_NO_ENTRY;
		snprintf(rr.note, sizeof(rr.note), "no entry in any epoch for pid 0x%04X", pid);
		return rr;
	}

	struct pcr_entry *start = ring_at(r, idx);
	rr.status     = RESOLVE_OK;
	rr.start_ext  = start->ext_seq;
	rr.actual_pcr = start->pcr_base;
	rr.epoch      = start->epoch;
	rr.slot       = start->slot;
	rr.pcr_error  = pcr_diff(start->pcr_base, base);

	/*
	 * Out-of-range detection.
	 *
	 * Two separate things, and the first one is not enough on its own. After a
	 * BACKWARD discontinuity the newest epoch's PCRs are lower than the older
	 * epoch's, so a request that predates everything we hold can still land
	 * nearer to the new epoch's first entry than to the old epoch's -- the
	 * nearest-match is then correct while the answer is still useless. Comparing
	 * against the ring's overall oldest entry misses exactly that case.
	 *
	 * So: (1) is the request before the start of the epoch we resolved INTO, and
	 * (2) independently of epochs, is the nearest PCR we found further away than
	 * the buffer could possibly hold? (2) is the one that actually protects the
	 * caller, because it is true regardless of how the epochs are arranged.
	 */
	struct pcr_entry *epoch_oldest = NULL;
	for (size_t i = 0; i < r->count; i++) {
		if (ring_at(r, i)->epoch == start->epoch) { epoch_oldest = ring_at(r, i); break; }
	}
	uint64_t err_abs = pcr_abs_diff(start->pcr_base, base);
	uint64_t buffer_ticks = (uint64_t)g_buffer_ms * PCR_HZ / 1000ULL;

	if (epoch_oldest && pcr_diff(base, epoch_oldest->pcr_base) < 0 && start == epoch_oldest) {
		rr.status = RESOLVE_BEFORE_EPOCH;
		snprintf(rr.note, sizeof(rr.note),
			"requested PCR precedes epoch %" PRIu32 " by %" PRId64 " ticks",
			start->epoch, pcr_diff(epoch_oldest->pcr_base, base));
	}
	/* Checked second and allowed to overwrite: it is the stronger statement of
	 * the two, and it holds regardless of how the epochs are arranged. */
	if (err_abs > buffer_ticks) {
		rr.status = RESOLVE_OUTSIDE_BUFFER;
		snprintf(rr.note, sizeof(rr.note),
			"nearest PCR is %" PRIu64 " ticks (%.0f ms) from the request, beyond the "
			"%d ms buffer -- outside the catalogue",
			err_abs, (double)err_abs * 1000.0 / (double)PCR_HZ, g_buffer_ms);
	}
	/*
	 * AGE AGAINST THE LIVE EDGE. This is a separate question from everything
	 * above and neither of those checks answers it.
	 *
	 * err_abs says how close we got to the PCR that was asked for. It says
	 * nothing about whether that entry can still be SERVED. The catalogue ring
	 * deliberately holds far more history than the retransmission buffer -- 2048
	 * entries is 39-81 s of PCRs against a 4 s buffer -- so an EXACT hit on a
	 * 40-second-old entry gives err_abs == 0, passes every distance check, and
	 * returns a sequence number librist dropped 36 seconds earlier. Measured: at
	 * a 1000 ms buffer a request 3.98 s stale returned OK with start_ext 19.
	 *
	 * The authoritative measure is payload residency, not time: librist holds
	 * the newest N payloads, so anything older than (live edge - N) is gone
	 * whatever the clock says.
	 */
	if (rr.status == RESOLVE_OK) {
		bool est = false;
		uint64_t resident = resident_payloads(&est);
		if (est) {
			/*
			 * Residency is only knowable from librist, and librist holds nothing
			 * until a peer attaches. Deriving a threshold from the input rate
			 * instead was tried and abandoned: on a bench the rate average is
			 * diluted by idle periods, and the fabricated threshold produced
			 * FALSE TOO_OLD verdicts on entries that were well inside the buffer
			 * -- refusing requests that would have worked. An unchecked answer
			 * that says so is better than a confident wrong one.
			 */
			size_t n = strlen(rr.note);
			snprintf(rr.note + n, sizeof(rr.note) - n,
				"%sage NOT checked: no peer attached, so nothing is resident and "
				"residency cannot be measured", n ? "; " : "");
		} else if (g_ext_seq > start->ext_seq &&
		           (g_ext_seq - start->ext_seq) > resident) {
			uint64_t behind = (g_ext_seq - start->ext_seq) - resident;
			double pps = g_buffer_ms > 0
			           ? (double)resident / ((double)g_buffer_ms / 1000.0) : 0.0;
			rr.status = RESOLVE_TOO_OLD;
			snprintf(rr.note, sizeof(rr.note),
				"found at ext_seq %" PRIu64 ", live edge %" PRIu64 ", only the "
				"newest %" PRIu64 " payloads resident: aged out of the %d ms "
				"buffer about %.1f s ago",
				start->ext_seq, g_ext_seq, resident, g_buffer_ms,
				pps > 0.0 ? (double)behind / pps : 0.0);
		}
	}

	if (rr.status != RESOLVE_OK)
		g_requests_outside_buffer++;

	/* END: first entry at or past base + duration, same epoch. */
	uint64_t end_target = (base + duration) & PCR_MASK33;
	bool end_found = false;
	for (size_t i = idx; i < r->count; i++) {
		struct pcr_entry *e = ring_at(r, i);
		if (e->epoch != start->epoch)
			break;
		if (pcr_diff(e->pcr_base, end_target) >= 0) {
			rr.end_ext = e->ext_seq;
			end_found = true;
			break;
		}
	}
	pthread_mutex_unlock(&catalogue_lock);

	if (!end_found) {
		/* Live edge. Serve what exists rather than failing: estimate from the
		 * measured rate and round up to whole RTP payloads. */
		uint64_t elapsed = now_us() - g_start_us;
		double rate_bps = elapsed ? ((double)g_bytes_in * 8.0 * 1000000.0 / (double)elapsed) : 0.0;
		double ts_pkts  = ceil(((double)duration * rate_bps) /
		                       (188.0 * 8.0 * (double)PCR_HZ));
		uint64_t payloads = (uint64_t)ceil(ts_pkts / (double)TS_PER_RTP);
		rr.end_ext = rr.start_ext + payloads;
		if (rr.end_ext > g_ext_seq)
			rr.end_ext = g_ext_seq;         /* clamp to what we have written */
		rr.end_estimated = true;
		if (rr.note[0] == '\0')
			snprintf(rr.note, sizeof(rr.note),
				"live edge: end estimated from %.2f Mb/s, %" PRIu64 " payloads",
				rate_bps / 1e6, payloads);
	}
	return rr;
}

/* --------------------------------------------------------------- tripwires */
/*
 * (a) Keyed on librist's own resident payload count, lifted from the stats JSON
 *     (sender-stats.incoming_queue.size, emitted at stats.c:87). This is the
 *     authoritative figure. Deriving it from rate x configured time would be
 *     checking our own assumption, which is precisely the thing that could be
 *     wrong -- librist grows and shrinks the buffer itself
 *     (rist-common.c:3470-3487) to converge on a TIME target, so the packet
 *     count moves with the input rate whether we intend it or not.
 *
 *     Compared against the high-water mark, not the instantaneous value.
 */
static int stats_cb(void *arg, const struct rist_stats *sc)
{
	(void)arg;
	if (!sc || !sc->stats_json)
		return 0;

	const char *q = strstr(sc->stats_json, "\"incoming_queue\"");
	if (!q)
		return 0;

	const char *k;
	unsigned long long v;
	if ((k = strstr(q, "\"size\":")) && sscanf(k + 7, "%llu", &v) == 1) {
		g_queue_size_now = v;
		if (v > g_queue_size_max)
			g_queue_size_max = v;
		g_have_queue_stats = true;
	}
	if ((k = strstr(q, "\"time_length\":")) && sscanf(k + 14, "%llu", &v) == 1)
		g_queue_time_ms = v;
	if ((k = strstr(q, "\"bytesize\":")) && sscanf(k + 11, "%llu", &v) == 1)
		g_queue_bytesize = v;

	/*
	 * Per-peer figures for the GUI.
	 *
	 * The sender stats callback fires ONCE PER PEER and carries a single "peer"
	 * object -- not an array -- so peers must be accumulated across callbacks
	 * rather than read out of one document. The key is "id"; an earlier version
	 * looked for "peer_id", which exists only in the C struct and never in the
	 * JSON, so the list stayed empty while peers were plainly connected and
	 * carrying a program selection.
	 *
	 * Entries expire: a peer that stops reporting has gone away, and a stale
	 * row claiming a box is being served is worse than no row.
	 */
	const char *p = strstr(sc->stats_json, "\"peer\"");
	if (p) {
		unsigned long long uv; double dv;
		uint32_t id = 0;
		const char *k = strstr(p, "\"id\":");
		if (k && sscanf(k + 5, "%llu", &uv) == 1)
			id = (uint32_t)uv;

		pthread_mutex_lock(&peer_lock);
		struct peer_stat *ps = NULL;
		for (int i = 0; i < g_peer_count; i++)
			if (g_peers[i].id == id) { ps = &g_peers[i]; break; }
		if (!ps && g_peer_count < MAX_TRACKED_PEERS)
			ps = &g_peers[g_peer_count++];
		if (ps) {
			memset(ps, 0, sizeof(*ps));
			ps->id = id;
			ps->last_seen_us = now_us();
			if ((k = strstr(p, "\"cname\":\"")))
				sscanf(k + 9, "%63[^\"]", ps->cname);
			if ((k = strstr(p, "\"type\":\"")))
				sscanf(k + 8, "%7[^\"]", ps->type);
			if ((k = strstr(p, "\"quality\":")) && sscanf(k + 10, "%lf", &dv) == 1)
				ps->quality = dv;
			if ((k = strstr(p, "\"sent\":")) && sscanf(k + 7, "%lf", &dv) == 1)
				ps->sent = (uint64_t)dv;
			if ((k = strstr(p, "\"received\":")) && sscanf(k + 11, "%lf", &dv) == 1)
				ps->received = (uint64_t)dv;
			if ((k = strstr(p, "\"retransmitted\":")) && sscanf(k + 16, "%lf", &dv) == 1)
				ps->retransmitted = (uint64_t)dv;
			if ((k = strstr(p, "\"bandwidth\":")) && sscanf(k + 12, "%lf", &dv) == 1)
				ps->bandwidth = (uint64_t)dv;
			/* rtt/avg_rtt are already divided by RIST_CLOCK, so milliseconds. */
			if ((k = strstr(p, "\"avg_rtt\":")) && sscanf(k + 10, "%lf", &dv) == 1)
				ps->avg_rtt_ms = dv;
			if ((k = strstr(p, "\"rtt\":")) && sscanf(k + 6, "%lf", &dv) == 1)
				ps->rtt_ms = dv;
		}

		/* Expire anything not heard from for 15 s -- the callback runs at 1 s. */
		uint64_t cutoff = now_us() - 15000000ULL;
		int w = 0;
		for (int i = 0; i < g_peer_count; i++)
			if (g_peers[i].last_seen_us >= cutoff)
				g_peers[w++] = g_peers[i];
		g_peer_count = w;
		pthread_mutex_unlock(&peer_lock);
	}

	uint64_t threshold = SEQ_SPACE * TRIPWIRE_WARN_PCT / 100;
	if (g_queue_size_max >= threshold && !g_tripwire_a_fired) {
		g_tripwire_a_fired = true;
		rist_log(&logging_settings, RIST_LOG_ERROR,
			"[TRIPWIRE-A] retransmit buffer high-water %" PRIu64 " payloads is at "
			"%" PRIu64 "%% of the 65536 ceiling (now=%" PRIu64 ", %" PRIu64 " ms). "
			"librist has no extended sequence: past 65536 resident payloads the "
			"16-bit index returns content from the PREVIOUS pass and the integrity "
			"check cannot see it. REDUCE THE BUFFER or widen the index in librist.\n",
			g_queue_size_max, g_queue_size_max * 100 / SEQ_SPACE,
			g_queue_size_now, g_queue_time_ms);
	}
	return 0;
}

/*
 * (b) Independent of librist entirely: measured input rate x configured buffer
 *     time. Catches the ceiling being breached because the multiplex got
 *     bigger, which (a) would only notice once the buffer had already grown.
 */
static void tripwire_rate_check(void)
{
	uint64_t elapsed = now_us() - g_start_us;
	if (elapsed < 5000000ULL)
		return;                              /* let the rate settle */

	double rate_bps  = (double)g_bytes_in * 8.0 * 1000000.0 / (double)elapsed;
	double ts_pps    = rate_bps / (188.0 * 8.0);
	double rtp_pps   = ts_pps / (double)TS_PER_RTP;
	double projected = rtp_pps * ((double)g_buffer_ms / 1000.0);
	double threshold = (double)SEQ_SPACE * TRIPWIRE_WARN_PCT / 100.0;

	if (projected >= threshold && !g_tripwire_b_fired) {
		g_tripwire_b_fired = true;
		rist_log(&logging_settings, RIST_LOG_ERROR,
			"[TRIPWIRE-B] measured %.2f Mb/s x %d ms buffer projects %.0f payloads, "
			"%.0f%% of the 65536 ceiling. Safe buffer at this rate is %.1f s. "
			"Above the ceiling the 16-bit retransmit index silently serves the "
			"wrong pass.\n",
			rate_bps / 1e6, g_buffer_ms, projected,
			projected * 100.0 / (double)SEQ_SPACE,
			(double)SEQ_SPACE / rtp_pps);
	}
}

/* ---------------------------------------------------------------- logging */
static void log_periodic(void)
{
	uint64_t nowu = now_us();
	uint64_t elapsed = nowu - g_start_us;

	/* Roll the recent-rate window. This is the figure the GUI shows; the
	 * lifetime average below is what tripwire B judges against. */
	if (g_window_start_us && nowu > g_window_start_us) {
		g_rate_window_bps = (double)(g_bytes_in - g_window_start_bytes) * 8.0
			* 1000000.0 / (double)(nowu - g_window_start_us);
	}
	g_window_start_us = nowu;
	g_window_start_bytes = g_bytes_in;

	double rate_bps = elapsed ? ((double)g_bytes_in * 8.0 * 1000000.0 / (double)elapsed) : 0.0;
	double rtp_pps  = elapsed ? ((double)g_payloads_written * 1000000.0 / (double)elapsed) : 0.0;

	rist_log(&logging_settings, RIST_LOG_INFO,
		"[STATUS] in=%.2f Mb/s ts=%" PRIu64 " sync_err=%" PRIu64
		" payloads=%" PRIu64 " (%.0f/s) ext_seq=%" PRIu64
		" | queue: %" PRIu64 " payloads %" PRIu64 " ms %.1f MB (max %" PRIu64
		", %" PRIu64 "%% of ceiling) | outside_buffer=%" PRIu64 "\n",
		rate_bps / 1e6, g_ts_packets_in, g_sync_errors,
		g_payloads_written, rtp_pps, g_ext_seq,
		g_queue_size_now, g_queue_time_ms, (double)g_queue_bytesize / 1048576.0,
		g_queue_size_max, g_queue_size_max * 100 / SEQ_SPACE,
		g_requests_outside_buffer);

	/* Headroom, from THIS instance's measured input. Logged once the rate is
	 * real so the figure can never be a fleet-wide guess. */
	if (rate_bps > 0.0) {
		double rtp_pps = rate_bps / (188.0 * 8.0) / (double)TS_PER_RTP;
		rist_log(&logging_settings, RIST_LOG_INFO,
			"[HEADROOM] %s: %.2f Mb/s -> %.0f payloads/s; %d ms buffer projects "
			"%.0f payloads (%.0f%% of %llu). Ceiling at %.1f s, warn at %.1f s. "
			"Input buffer holds %.0f ms at this rate.\n",
			g_instance_name, rate_bps / 1e6, rtp_pps, g_buffer_ms,
			rtp_pps * g_buffer_ms / 1000.0,
			rtp_pps * g_buffer_ms / 1000.0 * 100.0 / (double)SEQ_SPACE,
			(unsigned long long)SEQ_SPACE,
			rtp_pps > 0 ? (double)SEQ_SPACE / rtp_pps : 0.0,
			rtp_pps > 0 ? (double)SEQ_SPACE * TRIPWIRE_WARN_PCT / 100.0 / rtp_pps : 0.0,
			g_rcvbuf_got > 0 ? (double)g_rcvbuf_got / 1316.0 * 1316.0 * 8.0
			                   / rate_bps * 1000.0 : 0.0);
	}

	/*
	 * Same predicate as the JSON stats: a PID counts if it holds entries.
	 *
	 * These used to differ -- the log skipped iv_n == 0 to avoid dividing by
	 * zero for the mean interval, the JSON skipped count == 0 -- so the two
	 * could report different PID counts for the same catalogue, and did. The
	 * division is guarded instead, and a summary line is emitted that can be
	 * compared directly against the panel rather than by counting log lines.
	 */
	pthread_mutex_lock(&catalogue_lock);
	size_t npids = 0;
	uint64_t entries = 0, capacity = 0;
	for (int pid = 0; pid < MAX_PIDS; pid++) {
		struct pcr_ring *r = rings[pid];
		if (!r || r->count == 0)
			continue;
		npids++;
		entries  += r->count;
		capacity += r->cap;
		rist_log(&logging_settings, RIST_LOG_INFO,
			"[CATALOGUE] pid 0x%04X depth=%zu/%zu total=%" PRIu64
			" epoch=%" PRIu32 " (%u changes) interval us min=%" PRIu64
			" max=%" PRIu64 " mean=%" PRIu64 "\n",
			pid, r->count, r->cap, r->total_entries, r->epoch, r->epoch_changes,
			r->iv_n ? r->iv_min : 0, r->iv_n ? r->iv_max : 0,
			r->iv_n ? r->iv_sum / r->iv_n : 0);
	}
	rist_log(&logging_settings, RIST_LOG_INFO,
		"[CATALOGUE] %zu PIDs, %" PRIu64 " entries resident of %" PRIu64
		" ring capacity (%.0f%%)\n",
		npids, entries, capacity,
		capacity ? 100.0 * (double)entries / (double)capacity : 0.0);
	pthread_mutex_unlock(&catalogue_lock);
}

/* -------------------------------------------------------- debug interface */
/*
 * Local only -- a unix socket, never a wildcard-bound port. Line protocol:
 *   resolve <pid> <pcr_base> <duration_ticks>
 *   stats
 *   bounds [pid]
 *   events
 */
static char g_debug_path[108] = DEFAULT_DEBUG_SOCK;   /* sun_path is 108 */

/*
 * Group allowed to reach the debug socket.
 *
 * CONNECTING to a unix socket requires WRITE permission on it, not read. The
 * server runs as root and the socket lands 0755 root:root, so a web UI running
 * as www-data gets EACCES on connect -- which looks exactly like "the instance
 * is down". It never showed in testing because the socket was only ever read as
 * root. World-writable would fix it and is not acceptable, so the socket is
 * chowned to this group and set 0660: reachable by the UI, nobody else.
 */
static char g_sock_group[64] = "";

/*
 * A growable response buffer.
 *
 * The previous version wrote straight to the client socket. That fixed the 1 KB
 * truncation but introduced something worse: `stats` and `events` wrote while
 * holding catalogue_lock / disc_lock, and those are the locks emit_payload takes
 * for every PCR-bearing packet. A client that stops reading -- a wedged poller,
 * a stalled pipe -- blocks write() with the lock held and stalls INGEST. It
 * never showed with a hand-run `nc` and 33 short lines, and it is exactly the
 * failure mode of a test fixture promoted to a production dependency.
 *
 * So: build the response in memory, hold the lock only long enough to copy
 * state, and write after releasing it.
 */
struct dbuf {
	char  *p;
	size_t len, cap;
	bool   oom;
};

static void dbuf_add(struct dbuf *b, const char *fmt, ...)
{
	if (b->oom)
		return;
	for (int attempt = 0; attempt < 2; attempt++) {
		size_t avail = b->cap > b->len ? b->cap - b->len : 0;
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(b->p ? b->p + b->len : NULL, avail, fmt, ap);
		va_end(ap);
		if (n < 0) { b->oom = true; return; }
		if ((size_t)n < avail) { b->len += (size_t)n; return; }
		size_t want = b->cap ? b->cap : 8192;
		while (want < b->len + (size_t)n + 1)
			want *= 2;
		char *np = realloc(b->p, want);
		if (!np) { b->oom = true; return; }
		b->p = np;
		b->cap = want;
	}
}

/*
 * Write the whole response, tolerating a client that goes away mid-transfer.
 *
 * A GUI poller that times out and closes leaves this end with a half-written
 * response. Without SIGPIPE ignored that KILLS THE SERVER -- and the bigger the
 * response the likelier it is, so the JSON document made a latent crash into a
 * reachable one. A stats reader must never be able to take down the recovery
 * path: the write is bounded by SO_SNDTIMEO, a partial write is retried, and
 * EPIPE just abandons this client.
 */
static void dbuf_write_all(int fd, const char *p, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, p + off, len - off);
		if (w > 0) { off += (size_t)w; continue; }
		if (w < 0 && errno == EINTR)
			continue;
		break;                  /* EPIPE, EAGAIN on timeout, or a closed peer */
	}
}

static void dbuf_flush(struct dbuf *b, int fd)
{
	if (b->oom) {
		static const char err[] = "{\"error\":\"out of memory building response\"}\n";
		dbuf_write_all(fd, err, sizeof(err) - 1);
	} else if (b->p && b->len) {
		dbuf_write_all(fd, b->p, b->len);
	}
	free(b->p);
	b->p = NULL; b->len = b->cap = 0;
}

/* JSON string escaping: instance names and free-text notes reach the GUI here. */
static void dbuf_json_str(struct dbuf *b, const char *s)
{
	dbuf_add(b, "\"");
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')  dbuf_add(b, "\\%c", c);
		else if (c == '\n')         dbuf_add(b, "\\n");
		else if (c == '\r')         dbuf_add(b, "\\r");
		else if (c == '\t')         dbuf_add(b, "\\t");
		else if (c < 0x20)          dbuf_add(b, "\\u%04x", c);
		else                        dbuf_add(b, "%c", c);
	}
	dbuf_add(b, "\"");
}

/*
 * The GUI's stats interface.
 *
 * One document, one round trip, everything a per-instance panel needs. Every
 * rate and every headroom figure here is derived from THIS instance's own
 * measured input -- there is no shared constant. A second transponder running at
 * a different rate gets different numbers, which is the point: a fleet-wide
 * constant would make the headroom silently wrong for every transponder that is
 * not the one it was measured on.
 */
static void emit_json(int fd)
{
	uint64_t now = now_us();
	uint64_t elapsed = now - g_start_us;

	/* Lifetime average: what tripwire B judges against. */
	double life_bps = elapsed
		? (double)g_bytes_in * 8.0 * 1000000.0 / (double)elapsed : 0.0;
	/* Recent window: what an operator needs, because a lifetime average hides a
	 * feed that stopped ten minutes ago behind hours of healthy history. */
	double win_bps = g_rate_window_bps;

	double basis_bps = win_bps > 0.0 ? win_bps : life_bps;
	double rtp_pps   = basis_bps / (188.0 * 8.0) / (double)TS_PER_RTP;
	double projected = rtp_pps * ((double)g_buffer_ms / 1000.0);
	double safe_s    = rtp_pps > 0.0 ? (double)SEQ_SPACE / rtp_pps : 0.0;
	double warn_s    = rtp_pps > 0.0
		? (double)SEQ_SPACE * TRIPWIRE_WARN_PCT / 100.0 / rtp_pps : 0.0;

	bool json_res_est = false;
	uint64_t json_resident = resident_payloads(&json_res_est);

	struct dbuf b = { 0 };
	dbuf_add(&b, "{\"schema\":1,\"instance\":");
	dbuf_json_str(&b, g_instance_name);
	dbuf_add(&b, ",\"uptime_s\":%.1f,\"buffer_ms\":%d,\"require_selection\":%s,"
		"\"ceiling_payloads\":%llu,",
		(double)elapsed / 1e6, g_buffer_ms,
		g_require_selection ? "true" : "false",
		(unsigned long long)SEQ_SPACE);

	/* (1) input bitrate. RIST reports what it SENDS, never what arrives, so this
	 * is measured here and exposed rather than computed a second time. */
	dbuf_add(&b, "\"input\":{\"ts_packets\":%" PRIu64 ",\"bytes\":%" PRIu64
		",\"sync_errors\":%" PRIu64 ",\"payloads_written\":%" PRIu64
		",\"ext_seq\":%" PRIu64 ",\"rate_bps_window\":%.0f"
		",\"rate_bps_lifetime\":%.0f,\"rtp_pps\":%.1f"
		",\"rcvbuf_bytes\":%d,\"rcvbuf_clamped\":%s},",
		g_ts_packets_in, g_bytes_in, g_sync_errors, g_payloads_written,
		g_ext_seq, win_bps, life_bps, rtp_pps,
		g_rcvbuf_got, g_rcvbuf_clamped ? "true" : "false");

	/* Buffer health: read straight from librist's stats callback, not re-derived.
	 * safe_buffer_s / warn_buffer_s are THIS instance's ceiling arithmetic. */
	dbuf_add(&b, "\"buffer\":{\"payloads\":%" PRIu64 ",\"payloads_max\":%" PRIu64
		",\"ms\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"pct_of_ceiling\":%.2f"
		",\"have_stats\":%s,\"projected_payloads\":%.0f"
		",\"safe_buffer_s\":%.1f,\"warn_buffer_s\":%.1f"
		",\"resident_payloads\":%" PRIu64 ",\"resident_measured\":%s"
		",\"oldest_servable_ext\":%" PRIu64 "},",
		g_queue_size_now, g_queue_size_max, g_queue_time_ms, g_queue_bytesize,
		100.0 * (double)g_queue_size_now / (double)SEQ_SPACE,
		g_have_queue_stats ? "true" : "false", projected, safe_s, warn_s,
		json_resident, json_res_est ? "false" : "true",
		g_ext_seq > json_resident ? g_ext_seq - json_resident : 0);

	dbuf_add(&b, "\"tripwires\":{\"a\":%s,\"b\":%s,\"outside_buffer\":%" PRIu64 "},",
		g_tripwire_a_fired ? "true" : "false",
		g_tripwire_b_fired ? "true" : "false", g_requests_outside_buffer);

	/* Peers: count, quality and RTT come free from the stats callback. */
	pthread_mutex_lock(&peer_lock);
	dbuf_add(&b, "\"peers\":{\"count\":%d,\"list\":[", g_peer_count);
	for (int i = 0; i < g_peer_count && i < MAX_TRACKED_PEERS; i++) {
		dbuf_add(&b, "%s{\"id\":%u,\"cname\":", i ? "," : "", g_peers[i].id);
		dbuf_json_str(&b, g_peers[i].cname);
		dbuf_add(&b, ",\"type\":");
		dbuf_json_str(&b, g_peers[i].type);
		dbuf_add(&b, ",\"quality\":%.2f,\"rtt_ms\":%.2f,\"avg_rtt_ms\":%.2f"
			",\"retransmitted\":%" PRIu64 ",\"sent\":%" PRIu64
			",\"received\":%" PRIu64 ",\"bandwidth_bps\":%" PRIu64
			",\"last_seen_s\":%.1f}",
			g_peers[i].quality, g_peers[i].rtt_ms, g_peers[i].avg_rtt_ms,
			g_peers[i].retransmitted, g_peers[i].sent, g_peers[i].received,
			g_peers[i].bandwidth,
			(double)(now - g_peers[i].last_seen_us) / 1e6);
	}
	dbuf_add(&b, "]},");
	pthread_mutex_unlock(&peer_lock);

	/* (2) catalogue health -- Part 8 specific, exists nowhere else. */
	/*
	 * Occupancy is not the interesting number. The ring deliberately holds far
	 * more PCR history than the retransmission buffer can serve -- 2048 entries
	 * is 39-81 s against a 4 s buffer -- so once it saturates, "entries" reads
	 * 100% forever and would read the same if ingest had stopped. What moves,
	 * and what an operator actually needs, is how much of that history is still
	 * SERVABLE, and how far back the history reaches.
	 *
	 * Counted by walking back from the newest entry until it falls out of the
	 * servable window, so the cost is the servable span (~4 s) rather than the
	 * whole ring, and the catalogue lock is held only briefly.
	 */
	uint64_t oldest_ok = g_ext_seq > json_resident ? g_ext_seq - json_resident : 0;
	pthread_mutex_lock(&catalogue_lock);
	size_t npids = 0, entries = 0, capacity = 0, servable = 0;
	int64_t history_max_ticks = 0;
	dbuf_add(&b, "\"catalogue\":{\"pids\":[");
	for (int pid = 0; pid < MAX_PIDS; pid++) {
		struct pcr_ring *r = rings[pid];
		if (!r || r->count == 0) continue;

		size_t sv = 0;
		for (size_t i = r->count; i-- > 0; ) {
			if (ring_at(r, i)->ext_seq < oldest_ok) break;
			sv++;
		}
		int64_t span = pcr_diff(ring_at(r, r->count - 1)->pcr_base,
		                        ring_at(r, 0)->pcr_base);
		if (span > history_max_ticks) history_max_ticks = span;

		dbuf_add(&b, "%s{\"pid\":%d,\"depth\":%zu,\"cap\":%zu,\"servable\":%zu"
			",\"history_ms\":%.0f,\"oldest\":%" PRIu64 ",\"newest\":%" PRIu64
			",\"total\":%" PRIu64 ",\"epoch\":%" PRIu32 "}",
			npids ? "," : "", pid, r->count, r->cap, sv,
			(double)span * 1000.0 / (double)PCR_HZ,
			ring_at(r, 0)->pcr_base, ring_at(r, r->count - 1)->pcr_base,
			r->total_entries, r->epoch);
		npids++;
		entries  += r->count;
		capacity += r->cap;
		servable += sv;
	}
	dbuf_add(&b, "],\"pid_count\":%zu,\"entries\":%zu,\"capacity\":%zu"
		",\"servable\":%zu,\"history_ms\":%.0f},",
		npids, entries, capacity, servable,
		(double)history_max_ticks * 1000.0 / (double)PCR_HZ);
	pthread_mutex_unlock(&catalogue_lock);

	/* (3) discontinuity events. Section 7 is still open and a real field
	 * discontinuity is what we are waiting for, so it must not be buried. */
	pthread_mutex_lock(&disc_lock);
	uint64_t total = g_disc_total;
	size_t n = total < DISC_LOG_CAP ? (size_t)total : DISC_LOG_CAP;
	size_t show = n < 32 ? n : 32;
	dbuf_add(&b, "\"discontinuities\":{\"total\":%" PRIu64
		",\"indicator_nopcr\":%" PRIu64 ",\"recent\":[",
		total, g_disc_indicator_nopcr);
	for (size_t i = 0; i < show; i++) {
		size_t idx = (g_disc_head + DISC_LOG_CAP - show + i) % DISC_LOG_CAP;
		struct disc_event *e = &g_disc[idx];
		dbuf_add(&b, "%s{\"at\":%" PRIu64 ",\"pid\":%u,\"kind\":\"%s\""
			",\"epoch\":%" PRIu32 ",\"pcr_before\":%" PRIu64
			",\"pcr_after\":%" PRIu64 ",\"delta_ticks\":%" PRId64
			",\"delta_ms\":%.1f,\"ext_seq\":%" PRIu64 "}",
			i ? "," : "", e->wall_real, e->pid, disc_kind_str(e->kind),
			e->epoch, e->pcr_before, e->pcr_after, e->delta,
			(double)e->delta * 1000.0 / (double)PCR_HZ, e->ext_seq);
	}
	dbuf_add(&b, "]}}\n");
	pthread_mutex_unlock(&disc_lock);

	dbuf_flush(&b, fd);
}

/* Accept 0x0731 as well as 1841. Everything else in the tooling -- the logs,
 * TSDuck, the PMT -- speaks hex, so requiring decimal here invites mistakes.
 * Base is chosen explicitly rather than using strtoul(.., 0), which would read
 * a leading-zero PID like 0021 as OCTAL. */
static bool parse_pid_token(const char *tok, unsigned *out)
{
	char *end = NULL;
	int base = (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) ? 16 : 10;
	unsigned long v = strtoul(tok, &end, base);
	if (end == tok || (end && *end && *end != '\n' && *end != '\r'))
		return false;
	if (v > 8191)
		return false;
	*out = (unsigned)v;
	return true;
}

static void debug_handle(int fd, const char *line)
{
	char out[1024];
	unsigned pid; unsigned long long base, dur;
	char pidtok[32];

	if (sscanf(line, "resolve %31s %llu %llu", pidtok, &base, &dur) == 3
	    && parse_pid_token(pidtok, &pid)) {
		struct resolve_result rr = resolve((uint16_t)pid, base & PCR_MASK33, dur);
		if (!resolve_serviceable(&rr)) {
			/*
			 * Non-OK statuses that still found a nearest entry report it, because
			 * knowing what the catalogue DOES hold is the useful diagnostic. The
			 * status word is what decides serviceability -- never the note, and
			 * never the presence of a start_ext.
			 */
			if (rr.status == RESOLVE_BEFORE_EPOCH || rr.status == RESOLVE_OUTSIDE_BUFFER)
				snprintf(out, sizeof(out),
					"%s pid=0x%04X  NOT SERVICEABLE\n"
					"  requested_pcr = %llu\n"
					"  nearest_pcr   = %" PRIu64 "  (error %+" PRId64 " ticks, %+.3f ms)\n"
					"  note          = %s\n",
					resolve_status_str(rr.status), pid, base,
					rr.actual_pcr, rr.pcr_error,
					(double)rr.pcr_error * 1000.0 / (double)PCR_HZ,
					rr.note[0] ? rr.note : "-");
			else
				snprintf(out, sizeof(out), "%s %s\n",
					resolve_status_str(rr.status), rr.note);
		} else {
			snprintf(out, sizeof(out),
				"OK pid=0x%04X\n"
				"  requested_pcr = %llu\n"
				"  actual_pcr    = %" PRIu64 "  (error %+" PRId64 " ticks, %+.3f ms)\n"
				"  epoch         = %" PRIu32 "  slot=%u\n"
				"  start_ext     = %" PRIu64 "   start_wire = %" PRIu64 "\n"
				"  end_ext       = %" PRIu64 "   end_wire   = %" PRIu64 "%s\n"
				"  payloads      = %" PRIu64 "\n"
				"  note          = %s\n",
				pid, base, rr.actual_pcr, rr.pcr_error,
				(double)rr.pcr_error * 1000.0 / (double)PCR_HZ,
				rr.epoch, rr.slot,
				rr.start_ext, rr.start_ext & 0xFFFF,
				rr.end_ext,   rr.end_ext & 0xFFFF,
				rr.end_estimated ? "  (estimated)" : "",
				rr.end_ext > rr.start_ext ? rr.end_ext - rr.start_ext : 0,
				rr.note[0] ? rr.note : "-");
		}
	} else if (strncmp(line, "stats", 5) == 0) {
		/*
		 * Text form, kept because the validation harness parses it. The GUI uses
		 * `json` instead. Both snapshot under the lock and write after it.
		 */
		struct dbuf b = {0};
		dbuf_add(&b,
			"ext_seq=%" PRIu64 " payloads=%" PRIu64 " ts_in=%" PRIu64
			" queue_now=%" PRIu64 " queue_max=%" PRIu64 " queue_ms=%" PRIu64
			" queue_bytes=%" PRIu64 " ceiling=%llu outside_buffer=%" PRIu64
			" disc_total=%" PRIu64 " disc_nopcr=%" PRIu64
			" tripwires=%c%c\n",
			g_ext_seq, g_payloads_written, g_ts_packets_in,
			g_queue_size_now, g_queue_size_max, g_queue_time_ms, g_queue_bytesize,
			(unsigned long long)SEQ_SPACE, g_requests_outside_buffer,
			g_disc_total, g_disc_indicator_nopcr,
			g_tripwire_a_fired ? 'A' : '-', g_tripwire_b_fired ? 'B' : '-');
		pthread_mutex_lock(&catalogue_lock);
		for (int pid = 0; pid < MAX_PIDS; pid++) {
			if (!rings[pid] || rings[pid]->count == 0) continue;
			dbuf_add(&b, "pid 0x%04X depth=%zu total=%" PRIu64 " epoch=%" PRIu32 "\n",
				pid, rings[pid]->count, rings[pid]->total_entries, rings[pid]->epoch);
		}
		pthread_mutex_unlock(&catalogue_lock);
		dbuf_flush(&b, fd);
		return;
	} else if (strncmp(line, "json", 4) == 0) {
		emit_json(fd);
		return;
	} else if (strncmp(line, "bounds", 6) == 0) {
		/*
		 * The catalogue's current extent, per PID.
		 *
		 * Added because TOO_OLD was untestable without it: exercising it needs a
		 * PCR that is still IN the ring but PAST the buffer, and there was no way
		 * to learn a current PCR from this socket at all -- `events` only reports
		 * discontinuities and `stats` only depths. The only route left was to
		 * induce a discontinuity, which is absurd for a routine check. It is also
		 * the obvious thing to want when a request has just been refused.
		 *
		 * Any base between `oldest` and `oldest_servable` must resolve TOO_OLD.
		 */
		char btok[32]; unsigned only_pid = 0; bool one = false;
		if (sscanf(line, "bounds %31s", btok) == 1 && parse_pid_token(btok, &only_pid))
			one = true;

		bool est = false;
		uint64_t resident = resident_payloads(&est);
		struct dbuf b = {0};
		pthread_mutex_lock(&catalogue_lock);
		uint64_t oldest_ok = g_ext_seq > resident ? g_ext_seq - resident : 0;
		dbuf_add(&b, "live_ext_seq=%" PRIu64 " resident=%" PRIu64 "%s"
			" oldest_servable_ext=%" PRIu64 "\n",
			g_ext_seq, resident, est ? "(est)" : "", oldest_ok);
		for (int pid = 0; pid < MAX_PIDS; pid++) {
			struct pcr_ring *r = rings[pid];
			if (!r || r->count == 0) continue;
			if (one && (unsigned)pid != only_pid) continue;
			struct pcr_entry *o = ring_at(r, 0);
			struct pcr_entry *n = ring_at(r, r->count - 1);
			/*
			 * Spans are measured WITHIN THE NEWEST EPOCH only. A PCR difference
			 * across a discontinuity is meaningless -- the clock restarted -- so
			 * subtracting the ring's oldest base from its newest would report a
			 * span that never elapsed. epochs= says whether that matters here.
			 */
			uint32_t ep = n->epoch, nep = 1;
			uint64_t osb = n->pcr_base, oldest_ep = n->pcr_base;
			size_t sv = 0;
			bool still_servable = true;
			for (size_t i = r->count; i-- > 0; ) {
				struct pcr_entry *e = ring_at(r, i);
				if (e->epoch != ep) { nep++; ep = e->epoch; continue; }
				if (e->epoch == n->epoch) {
					oldest_ep = e->pcr_base;
					if (still_servable && e->ext_seq >= oldest_ok) {
						osb = e->pcr_base;
						sv++;
					} else {
						still_servable = false;
					}
				}
			}
			dbuf_add(&b,
				"pid 0x%04X count=%zu servable=%zu oldest=%" PRIu64
				" newest=%" PRIu64 " oldest_servable=%" PRIu64
				" history_ms=%.0f servable_ms=%.0f epoch=%" PRIu32 " epochs=%u\n",
				pid, r->count, sv, o->pcr_base, n->pcr_base, osb,
				(double)pcr_diff(n->pcr_base, oldest_ep) * 1000.0 / (double)PCR_HZ,
				(double)pcr_diff(n->pcr_base, osb) * 1000.0 / (double)PCR_HZ,
				n->epoch, nep);
		}
		pthread_mutex_unlock(&catalogue_lock);
		dbuf_flush(&b, fd);
		return;
	} else if (strncmp(line, "events", 6) == 0) {
		/*
		 * The discontinuity ring (O3). Reported after the fact on purpose: the
		 * point is to be able to resolve either side of a real discontinuity
		 * once one occurs, without having to catch it live in the journal.
		 */
		struct dbuf b = {0};
		pthread_mutex_lock(&disc_lock);
		uint64_t total = g_disc_total;
		size_t n = total < DISC_LOG_CAP ? (size_t)total : DISC_LOG_CAP;
		dbuf_add(&b, "disc_total=%" PRIu64 " indicator_nopcr=%" PRIu64
			" shown=%zu cap=%d\n", total, g_disc_indicator_nopcr, n, DISC_LOG_CAP);
		for (size_t i = 0; i < n; i++) {
			size_t idx = (g_disc_head + DISC_LOG_CAP - n + i) % DISC_LOG_CAP;
			struct disc_event *e = &g_disc[idx];
			dbuf_add(&b,
				"event real=%" PRIu64 " mono_us=%" PRIu64 " pid=0x%04X kind=%s"
				" epoch=%" PRIu32 " pcr_before=%" PRIu64 " pcr_after=%" PRIu64
				" delta=%" PRId64 " ext_seq=%" PRIu64 "\n",
				e->wall_real, e->wall_us, e->pid, disc_kind_str(e->kind),
				e->epoch, e->pcr_before, e->pcr_after, e->delta, e->ext_seq);
		}
		pthread_mutex_unlock(&disc_lock);
		dbuf_flush(&b, fd);
		return;
	} else {
		snprintf(out, sizeof(out),
			"ERR usage: 'resolve <pid|0xPID> <pcr_base> <duration_ticks>' | 'stats' |\n"
			"    'bounds [pid]' | 'events' | 'json'\n");
	}
	dbuf_write_all(fd, out, strlen(out));
}

static void *debug_thread(void *arg)
{
	(void)arg;
	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "debug socket() failed: %s\n", strerror(errno));
		return NULL;
	}
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", g_debug_path);
	unlink(g_debug_path);
	if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "debug bind(%s) failed: %s\n",
			g_debug_path, strerror(errno));
		close(srv);
		return NULL;
	}
	/*
	 * Permissions AFTER bind: the socket does not exist until bind() creates it,
	 * and umask would otherwise decide who can reach it.
	 */
	if (g_sock_group[0]) {
		struct group *gr = getgrnam(g_sock_group);
		if (!gr) {
			rist_log(&logging_settings, RIST_LOG_WARN,
				"debug socket group '%s' does not exist; leaving default "
				"permissions -- a UI running as another user will get EACCES\n",
				g_sock_group);
		} else if (chown(g_debug_path, (uid_t)-1, gr->gr_gid) != 0) {
			rist_log(&logging_settings, RIST_LOG_WARN,
				"could not chown debug socket to group %s: %s\n",
				g_sock_group, strerror(errno));
		} else if (chmod(g_debug_path, 0660) != 0) {
			rist_log(&logging_settings, RIST_LOG_WARN,
				"could not chmod debug socket to 0660: %s\n", strerror(errno));
		} else {
			rist_log(&logging_settings, RIST_LOG_INFO,
				"[DEBUG] socket group=%s mode=0660 (connect needs write; "
				"NOT world-writable)\n", g_sock_group);
		}
	}

	listen(srv, 4);
	rist_log(&logging_settings, RIST_LOG_INFO,
		"[DEBUG] listening on unix:%s  (echo 'resolve <pid> <base> <dur>' | nc -U %s)\n",
		g_debug_path, g_debug_path);

	/*
	 * Wait on the listening socket AND the shutdown pipe. A bare blocking
	 * accept() here is what used to wedge the whole process on SIGTERM: main
	 * gets to pthread_join() and waits forever for a thread that only looks at
	 * `running` once a client happens to connect.
	 */
	while (running) {
		struct pollfd pfd[2];
		pfd[0].fd = srv;              pfd[0].events = POLLIN; pfd[0].revents = 0;
		pfd[1].fd = shutdown_pipe[0]; pfd[1].events = POLLIN; pfd[1].revents = 0;

		int pr = poll(pfd, shutdown_pipe[0] >= 0 ? 2 : 1, -1);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd[1].revents & POLLIN)   /* shutdown poked us; do not drain it, */
			break;                     /* other waiters need to see it too    */
		if (!(pfd[0].revents & POLLIN))
			continue;

		int c = accept(srv, NULL, NULL);
		if (c < 0) { if (running && errno != EINTR) usleep(100000); continue; }
		/* The accept loop is single-threaded: without a bound, one client that
		 * stops reading would park it and every other poller with it. */
		struct timeval sto = { .tv_sec = 2, .tv_usec = 0 };
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &sto, sizeof(sto));
		char buf[512];
		ssize_t n = read(c, buf, sizeof(buf) - 1);
		if (n > 0) { buf[n] = '\0'; debug_handle(c, buf); }
		close(c);
	}
	close(srv);
	unlink(g_debug_path);
	return NULL;
}

/* ------------------------------------------------------------- UDP ingest */
static int open_input(const char *host, int port, int want_rcvbuf)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;

	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	/*
	 * Set SO_RCVBUF explicitly and REPORT WHAT WE ACTUALLY GOT. The kernel
	 * silently clamps to net.core.rmem_max and reports back double what it
	 * granted. We lost days to exactly this on the STB at 1/23rd of this rate,
	 * where the default 87380 held ~33 datagrams against 36-datagram bursts and
	 * the loss looked like a satellite problem. At the measured multiplex rate
	 * the margin is far thinner, so this is logged, not assumed.
	 */
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want_rcvbuf, sizeof(want_rcvbuf)) < 0)
		rist_log(&logging_settings, RIST_LOG_WARN, "SO_RCVBUF set failed: %s\n", strerror(errno));

	int got = 0;
	socklen_t glen = sizeof(got);
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &glen);

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = (host && *host && strcmp(host, "@") != 0)
	                     ? inet_addr(host) : htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "bind %d failed: %s\n", port, strerror(errno));
		close(fd);
		return -1;
	}

	/*
	 * No holding time is quoted here, and that is deliberate.
	 *
	 * This line used to say "at 40 Mb/s" -- an estimate 48% below the real rate,
	 * which overstated the headroom by the same margin. Replacing it with the
	 * measured 59.1 Mb/s fixed THIS transponder and would silently break every
	 * other one, because a second instance carries a different multiplex at a
	 * different rate. There is nothing to measure yet at bind time, so the
	 * capacity is reported in datagrams -- which is rate-independent and true --
	 * and the time it represents is logged by the periodic status once this
	 * instance has measured its own input.
	 */
	g_rcvbuf_got = got;
	g_rcvbuf_clamped = (got < want_rcvbuf);
	double datagrams = (double)got / 1316.0;
	rist_log(&logging_settings, RIST_LOG_INFO,
		"[INPUT] bound :%d  SO_RCVBUF requested %d -> kernel reports %d bytes "
		"(~%.0f datagrams of 1316; holding time follows once the rate is "
		"measured from this input)%s\n",
		port, want_rcvbuf, got, datagrams,
		got < want_rcvbuf ? "  << CLAMPED, raise net.core.rmem_max" : "");
	if (got < want_rcvbuf)
		rist_log(&logging_settings, RIST_LOG_WARN,
			"SO_RCVBUF was clamped. Fix with: sysctl -w net.core.rmem_max=%d\n", want_rcvbuf);
	return fd;
}

/*
 * Form one RTP payload from 7 staged TS packets, catalogue any PCR they carry,
 * and write it with OUR sequence number.
 *
 * The catalogue is built HERE, at packetization time, because this is the only
 * point where the mapping (PCR -> sequence) is known for free. Recovering it at
 * query time would mean re-parsing the retransmit buffer, which is both slower
 * and unable to see payloads that have already aged out.
 */
static void emit_payload(uint8_t *stage)
{
	uint64_t ext = g_ext_seq;

	/* Test every packet rather than consulting PAT/PMT. The request names the
	 * PCR_PID explicitly, so we only ever look up what we are asked for, and an
	 * index built from observation cannot go stale when a PMT changes. */
	for (int s = 0; s < TS_PER_RTP; s++) {
		const uint8_t *p = stage + s * TS_PACKET_SIZE;
		uint16_t pid; uint64_t base; bool disc;
		if (ts_extract_pcr(p, &pid, &base, &disc)) {
			pthread_mutex_lock(&catalogue_lock);
			catalogue_insert(pid, base, disc, ext, (uint8_t)s);
			pthread_mutex_unlock(&catalogue_lock);
		} else if (ts_discontinuity_flag(p, &pid)) {
			/* Flagged but carrying no PCR, so the catalogue never sees it.
			 * Counted and reported anyway: O3 needs to know a discontinuity
			 * happened even where there was nothing for the ring to act on. */
			g_disc_indicator_nopcr++;
			disc_record(pid, 0, 0, 0, 0, 0, ext);
			static uint64_t last_log[8];      /* tiny per-PID-hash throttle */
			uint64_t n = now_us();
			uint64_t *slot = &last_log[pid & 7];
			if (n - *slot > DISC_LOG_MIN_GAP_US) {
				*slot = n;
				rist_log(&logging_settings, RIST_LOG_WARN,
					"[DISC] pid 0x%04X discontinuity_indicator set on a packet "
					"with no PCR, ext_seq %" PRIu64 " (total %" PRIu64 ")\n",
					pid, ext, g_disc_indicator_nopcr);
			}
		}
	}

	struct rist_data_block db;
	memset(&db, 0, sizeof(db));
	db.payload     = stage;
	db.payload_len = RTP_PAYLOAD_SIZE;
	db.seq         = (uint32_t)(ext & 0xFFFF);   /* librist masks to 16 bits anyway */
	db.flags       = RIST_DATA_FLAGS_USE_SEQ;    /* we own the sequence space */

	int ret = rist_sender_data_write(sender_ctx, &db);
	if (ret < 0) {
		static uint64_t last_err = 0;
		uint64_t n = now_us();
		if (n > last_err + 1000000ULL) {
			rist_log(&logging_settings, RIST_LOG_ERROR,
				"rist_sender_data_write failed (%d) at ext_seq %" PRIu64 "\n", ret, ext);
			last_err = n;
		}
	}
	g_ext_seq++;
	g_payloads_written++;
}

/* -------------------------------------------------------------------- main */
static void usage(const char *me)
{
	fprintf(stderr,
		"VSF TR-06-4 Part 8 recovery server (Milestone 1)\n\n"
		"Usage: %s -i udp://@:PORT -o rist://@HOST:PORT?weight=1000&buffer=10000 [opts]\n"
		"  -i URL       MPTS input, raw TS over UDP\n"
		"  -o URL       RIST output peer (weight=1000 for Part 8 FSR gating)\n"
		"  -b MS        retransmit buffer target, default %d\n"
		"  -r BYTES     SO_RCVBUF request, default %d\n"
		"  -d PATH      debug unix socket, default %s\n"
		"  -N NAME      instance name, reported in stats and logs\n"
		"  -g GROUP     chown the debug socket to GROUP and set 0660, so a UI\n"
		"               running as that group can connect (connect needs write)\n"
		"  -S           do NOT require a registered content selection (unsafe)\n"
		"  -v           verbose\n", me, DEFAULT_BUFFER_MS, DEFAULT_RCVBUF, DEFAULT_DEBUG_SOCK);
}

int main(int argc, char *argv[])
{
	char *inurl = NULL, *outurl = NULL;
	int rcvbuf = DEFAULT_RCVBUF;
	bool require_selection = true;
	bool saw_npd = false;
	int c;

	while ((c = getopt(argc, argv, "i:o:b:r:d:N:g:Snvh")) != -1) {
		switch (c) {
		case 'i': inurl  = strdup(optarg); break;
		case 'o': outurl = strdup(optarg); break;
		case 'b': g_buffer_ms = atoi(optarg); break;
		case 'r': rcvbuf = atoi(optarg); break;
		case 'd': snprintf(g_debug_path, sizeof(g_debug_path), "%s", optarg); break;
		case 'N': snprintf(g_instance_name, sizeof(g_instance_name), "%s", optarg); break;
		case 'g': snprintf(g_sock_group, sizeof(g_sock_group), "%s", optarg); break;
		case 'S': require_selection = false; break;
		case 'n': saw_npd = true; break;
		case 'v': logging_settings.log_level = RIST_LOG_DEBUG; break;
		default:  usage(argv[0]); return 1;
		}
	}

	/* LOGGING_SETTINGS_INITIALIZER defaults to RIST_LOG_DISABLE, so an
	 * unconfigured tool is silent -- including the tripwires, which would be
	 * the worst possible thing to lose. Default to INFO explicitly. */
	if (logging_settings.log_level == RIST_LOG_DISABLE)
		logging_settings.log_level = RIST_LOG_INFO;
	struct rist_logging_settings *lp = &logging_settings;
	rist_logging_set(&lp, logging_settings.log_level, NULL, NULL, NULL, stderr);

	/*
	 * -n is refused at startup, structurally, not with a runtime warning.
	 * send_filtered_data_to_peer() bails out UNFILTERED on NPD'd payloads
	 * (udp.c:218-228) and only logs a rate-limited warning, which at the measured
	 * multiplex rate is
	 * invisible. The consequence is serving the whole multiplex instead of one
	 * service -- roughly 15x -- so this must not be survivable by inattention.
	 */
	if (saw_npd) {
		fprintf(stderr,
			"FATAL: -n (null packet deletion) cannot be used on a Part 8 recovery server.\n"
			"       NPD and Part 6 program selection are mutually exclusive in this fork:\n"
			"       filtering is skipped for NPD'd payloads, so every retransmission would\n"
			"       carry the entire multiplex. Remove -n.\n");
		return 1;
	}
	if (!inurl || !outurl) { usage(argv[0]); return 1; }

	/* Before the handlers, so a signal can never find a half-built pipe. */
	if (pipe(shutdown_pipe) < 0) {
		fprintf(stderr, "pipe() failed: %s\n", strerror(errno));
		return 1;
	}
	/* A stats client that hangs up mid-response must not be able to kill the
	 * recovery path. Writes report EPIPE instead and are handled locally. */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	char host[256] = "";
	int port = 0;
	if (sscanf(inurl, "udp://@%255[^:]:%d", host, &port) != 2 &&
	    sscanf(inurl, "udp://@:%d", &port) != 1 &&
	    sscanf(inurl, "udp://%255[^:]:%d", host, &port) != 2) {
		fprintf(stderr, "FATAL: cannot parse input URL: %s\n", inurl);
		return 1;
	}

	if (rist_sender_create(&sender_ctx, RIST_PROFILE_MAIN, 0, &logging_settings) != 0) {
		fprintf(stderr, "FATAL: rist_sender_create failed\n");
		return 1;
	}

	struct rist_peer_config *pcfg = NULL;
	if (rist_parse_address2(outurl, &pcfg) != 0) {
		fprintf(stderr, "FATAL: cannot parse output URL: %s\n", outurl);
		return 1;
	}
	struct rist_peer *peer = NULL;
	if (rist_peer_create(sender_ctx, &peer, pcfg) != 0) {
		fprintf(stderr, "FATAL: rist_peer_create failed\n");
		rist_peer_config_free2(&pcfg);
		return 1;
	}
	rist_peer_config_free2(&pcfg);

	/* P1(a): refuse retransmission to any peer that has not declared what it is
	 * watching. Opt-in flag, default-off in librist, so the Part 6/7 units are
	 * bit-identical -- only this process sets it. */
	g_require_selection = require_selection;
	if (require_selection) {
		if (rist_sender_require_selection_enable(sender_ctx) != 0)
			rist_log(&logging_settings, RIST_LOG_WARN,
				"could not enable require-selection; retransmissions will NOT be gated\n");
		else
			rist_log(&logging_settings, RIST_LOG_INFO,
				"[POLICY] retransmissions require a registered Part 6 content selection\n");
	} else {
		rist_log(&logging_settings, RIST_LOG_WARN,
			"[POLICY] -S given: serving retransmissions to peers with NO selection. "
			"A single request can pull the entire multiplex.\n");
	}

	rist_stats_callback_set(sender_ctx, 1000, stats_cb, NULL);

	if (rist_start(sender_ctx) != 0) {
		fprintf(stderr, "FATAL: rist_start failed\n");
		return 1;
	}

	int in = open_input(host, port, rcvbuf);
	if (in < 0) return 1;

	pthread_t dbg;
	pthread_create(&dbg, NULL, debug_thread, NULL);

	rist_log(&logging_settings, RIST_LOG_INFO,
		"[START] Part 8 recovery server: in=%s out=%s buffer=%d ms ceiling=%llu payloads\n",
		inurl, outurl, g_buffer_ms, (unsigned long long)SEQ_SPACE);

	g_start_us = now_us();
	uint64_t next_log = g_start_us + 5000000ULL;

	uint8_t rx[65536];
	uint8_t stage[RTP_PAYLOAD_SIZE];
	size_t staged = 0;
	uint8_t partial[TS_PACKET_SIZE];
	size_t partial_len = 0;

	while (running) {
		struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
		fd_set rf; FD_ZERO(&rf); FD_SET(in, &rf);
		int maxfd = in;
		if (shutdown_pipe[0] >= 0) {
			FD_SET(shutdown_pipe[0], &rf);
			if (shutdown_pipe[0] > maxfd) maxfd = shutdown_pipe[0];
		}
		if (select(maxfd + 1, &rf, NULL, NULL, &tv) <= 0) {
			if (now_us() > next_log) { log_periodic(); tripwire_rate_check(); next_log = now_us() + 5000000ULL; }
			continue;
		}
		if (shutdown_pipe[0] >= 0 && FD_ISSET(shutdown_pipe[0], &rf))
			break;
		if (!FD_ISSET(in, &rf))
			continue;

		ssize_t n = recv(in, rx, sizeof(rx), 0);
		if (n <= 0) continue;
		g_bytes_in += (uint64_t)n;

		/* Datagrams are not guaranteed to be a whole number of TS packets, so
		 * carry the remainder rather than discarding it. */
		size_t off = 0;
		while (off < (size_t)n) {
			if (partial_len) {
				size_t need = TS_PACKET_SIZE - partial_len;
				size_t take = ((size_t)n - off) < need ? ((size_t)n - off) : need;
				memcpy(partial + partial_len, rx + off, take);
				partial_len += take; off += take;
				if (partial_len < TS_PACKET_SIZE) break;
				partial_len = 0;
				if (partial[0] != TS_SYNC) { g_sync_errors++; continue; }
				memcpy(stage + staged * TS_PACKET_SIZE, partial, TS_PACKET_SIZE);
				if (++staged == TS_PER_RTP) { emit_payload(stage); staged = 0; }
				g_ts_packets_in++;
				continue;
			}
			if ((size_t)n - off < TS_PACKET_SIZE) {
				partial_len = (size_t)n - off;
				memcpy(partial, rx + off, partial_len);
				break;
			}
			uint8_t *p = rx + off;
			off += TS_PACKET_SIZE;
			if (p[0] != TS_SYNC) { g_sync_errors++; continue; }
			memcpy(stage + staged * TS_PACKET_SIZE, p, TS_PACKET_SIZE);
			if (++staged == TS_PER_RTP) { emit_payload(stage); staged = 0; }
			g_ts_packets_in++;
		}

		if (now_us() > next_log) { log_periodic(); tripwire_rate_check(); next_log = now_us() + 5000000ULL; }
	}

	rist_log(&logging_settings, RIST_LOG_INFO, "[STOP] shutting down\n");
	log_periodic();
	close(in);
	pthread_join(dbg, NULL);
	rist_log(&logging_settings, RIST_LOG_INFO, "[STOP] debug thread joined\n");
	rist_destroy(sender_ctx);
	if (shutdown_pipe[0] >= 0) { close(shutdown_pipe[0]); close(shutdown_pipe[1]); }
	rist_log(&logging_settings, RIST_LOG_INFO, "[STOP] clean exit\n");
	return 0;
}
