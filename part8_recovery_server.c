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
#include <string.h>
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

static void on_signal(int sig) { (void)sig; running = 0; }

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
				"[CATALOGUE] pid 0x%04X discontinuity: %s, %" PRId64
				" ticks, entering epoch %" PRIu32 "\n",
				pid, disc ? "indicator set" : "backward jump", d, r->epoch);
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
struct resolve_result {
	bool     ok;
	uint64_t start_ext, end_ext;
	uint64_t actual_pcr;
	uint32_t epoch;
	uint8_t  slot;
	bool     end_estimated;      /* live edge: end came from the rate fallback */
	bool     before_buffer;      /* request older than anything we hold */
	int64_t  pcr_error;          /* actual - requested, signed ticks */
	char     note[160];
};

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
		snprintf(rr.note, sizeof(rr.note), "no entry in any epoch for pid 0x%04X", pid);
		return rr;
	}

	struct pcr_entry *start = ring_at(r, idx);
	rr.ok         = true;
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
		rr.before_buffer = true;
		snprintf(rr.note, sizeof(rr.note),
			"requested PCR precedes epoch %" PRIu32 " by %" PRId64 " ticks",
			start->epoch, pcr_diff(epoch_oldest->pcr_base, base));
	}
	if (err_abs > buffer_ticks) {
		rr.before_buffer = true;
		snprintf(rr.note, sizeof(rr.note),
			"nearest PCR is %" PRIu64 " ticks (%.0f ms) from the request, beyond the "
			"%d ms buffer -- outside the catalogue",
			err_abs, (double)err_abs * 1000.0 / (double)PCR_HZ, g_buffer_ms);
	}
	if (rr.before_buffer)
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
	uint64_t elapsed = now_us() - g_start_us;
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

	pthread_mutex_lock(&catalogue_lock);
	for (int pid = 0; pid < MAX_PIDS; pid++) {
		struct pcr_ring *r = rings[pid];
		if (!r || r->iv_n == 0)
			continue;
		rist_log(&logging_settings, RIST_LOG_INFO,
			"[CATALOGUE] pid 0x%04X depth=%zu/%zu total=%" PRIu64
			" epoch=%" PRIu32 " (%u changes) interval us min=%" PRIu64
			" max=%" PRIu64 " mean=%" PRIu64 "\n",
			pid, r->count, r->cap, r->total_entries, r->epoch, r->epoch_changes,
			r->iv_min, r->iv_max, r->iv_sum / r->iv_n);
	}
	pthread_mutex_unlock(&catalogue_lock);
}

/* -------------------------------------------------------- debug interface */
/*
 * Local only -- a unix socket, never a wildcard-bound port. Line protocol:
 *   resolve <pid> <pcr_base> <duration_ticks>
 *   stats
 */
static char g_debug_path[108] = DEFAULT_DEBUG_SOCK;   /* sun_path is 108 */

static void debug_handle(int fd, const char *line)
{
	char out[1024];
	unsigned pid; unsigned long long base, dur;

	if (sscanf(line, "resolve %u %llu %llu", &pid, &base, &dur) == 3) {
		struct resolve_result rr = resolve((uint16_t)pid, base & PCR_MASK33, dur);
		if (!rr.ok) {
			snprintf(out, sizeof(out), "ERR %s\n", rr.note);
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
		int n = snprintf(out, sizeof(out),
			"ext_seq=%" PRIu64 " payloads=%" PRIu64 " ts_in=%" PRIu64
			" queue_now=%" PRIu64 " queue_max=%" PRIu64 " queue_ms=%" PRIu64
			" queue_bytes=%" PRIu64 " ceiling=%llu tripwires=%c%c\n",
			g_ext_seq, g_payloads_written, g_ts_packets_in,
			g_queue_size_now, g_queue_size_max, g_queue_time_ms, g_queue_bytesize,
			(unsigned long long)SEQ_SPACE,
			g_tripwire_a_fired ? 'A' : '-', g_tripwire_b_fired ? 'B' : '-');
		pthread_mutex_lock(&catalogue_lock);
		for (int pid = 0; pid < MAX_PIDS && n < (int)sizeof(out) - 96; pid++) {
			if (!rings[pid] || rings[pid]->count == 0) continue;
			n += snprintf(out + n, sizeof(out) - n,
				"pid 0x%04X depth=%zu epoch=%" PRIu32 "\n",
				pid, rings[pid]->count, rings[pid]->epoch);
		}
		pthread_mutex_unlock(&catalogue_lock);
	} else {
		snprintf(out, sizeof(out),
			"ERR usage: 'resolve <pid> <pcr_base> <duration_ticks>' | 'stats'\n");
	}
	(void)!write(fd, out, strlen(out));
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
	listen(srv, 4);
	rist_log(&logging_settings, RIST_LOG_INFO,
		"[DEBUG] listening on unix:%s  (echo 'resolve <pid> <base> <dur>' | nc -U %s)\n",
		g_debug_path, g_debug_path);

	while (running) {
		int c = accept(srv, NULL, NULL);
		if (c < 0) { if (running) usleep(100000); continue; }
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
	 * the loss looked like a satellite problem. At 40 Mb/s the margin is far
	 * thinner, so this is logged, not assumed.
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

	double datagrams = (double)got / 1316.0;
	rist_log(&logging_settings, RIST_LOG_INFO,
		"[INPUT] bound :%d  SO_RCVBUF requested %d -> kernel reports %d bytes "
		"(~%.0f datagrams of 1316, ~%.0f ms at 40 Mb/s)%s\n",
		port, want_rcvbuf, got, datagrams, datagrams * 1316.0 * 8.0 / 40e6 * 1000.0,
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
		uint16_t pid; uint64_t base; bool disc;
		if (ts_extract_pcr(stage + s * TS_PACKET_SIZE, &pid, &base, &disc)) {
			pthread_mutex_lock(&catalogue_lock);
			catalogue_insert(pid, base, disc, ext, (uint8_t)s);
			pthread_mutex_unlock(&catalogue_lock);
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

	while ((c = getopt(argc, argv, "i:o:b:r:d:Snvh")) != -1) {
		switch (c) {
		case 'i': inurl  = strdup(optarg); break;
		case 'o': outurl = strdup(optarg); break;
		case 'b': g_buffer_ms = atoi(optarg); break;
		case 'r': rcvbuf = atoi(optarg); break;
		case 'd': snprintf(g_debug_path, sizeof(g_debug_path), "%s", optarg); break;
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
	 * (udp.c:218-228) and only logs a rate-limited warning, which at 40 Mb/s is
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
		if (select(in + 1, &rf, NULL, NULL, &tv) <= 0) {
			if (now_us() > next_log) { log_periodic(); tripwire_rate_check(); next_log = now_us() + 5000000ULL; }
			continue;
		}

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
	rist_destroy(sender_ctx);
	return 0;
}
