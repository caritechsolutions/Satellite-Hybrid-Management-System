/*
 * stb_part7_receiver.c -- VSF TR-06-4 Part 7 marker RECEIVER (STB, ARM)
 *
 * WHERE THIS RUNS: on the set-top box. It takes the box's dmx2 capture as UDP,
 * validates the Part 7 markers the headend inserted, strips them, and re-emits
 * the original flow as RIST to the local ristreceiver's weight-0 satellite peer.
 * In Part 7 terms this is the RECEIVER: it is the side that validates. The
 * headend side, which marks, is headend_part7_sender.
 *
 * (The legacy name for this tool was "ristsender_marker", which read as a
 * headend sender despite running on the box. That file is left in place
 * untouched; this is a new variant alongside it.)
 *
 * WHAT DIFFERS FROM THE LEGACY TOOL -- the counterpart to the sender's change:
 *
 * 1. ELEMENTARY STREAMS ONLY. non_null from the marker now counts the marker
 *    plus elementary streams, PSI excluded (headend_part7_sender). We apply the
 *    same rule here -- exclude 0x0000-0x001F plus the PMT PID(s) learned from
 *    the PAT -- and compare ES against non_null - 1.
 *
 *    Nulls are NOT compared. The box's capture is PID-filtered post-demux, so it
 *    only ever sees our service: video, audio, marker and the box's own injected
 *    PAT+PMT. There are no nulls and no other services to see, so the observed
 *    null count is always 0 while the marker reports 3-7. null_count is used
 *    solely to reinsert. PSI is not compared either: the box's PSI is its own
 *    injection, not the headend's.
 *
 * 2. STRUCTURAL BLOCK SIZE. non_null + null no longer sums to the block total by
 *    design, so the total cannot be derived from the marker any more. It is the
 *    structural constant BLOCK_TOTAL_PACKETS, and the headend's PSI count is
 *    recovered as psi = BLOCK_TOTAL_PACKETS - non_null - null.
 *
 * 3. RECONSTRUCTION. The box holds only ES (plus its own PSI); the headend's
 *    nulls and PSI were never captured. Emitting only what was captured would
 *    put the RTP payload boundaries in the wrong place, so the block is rebuilt
 *    to exactly BLOCK_CONTENT_PACKETS: the ES, then psi filler, then null_count
 *    nulls. That is 35 = 5 x 7, so the payload boundaries land where the headend
 *    put them and the recovery peer's flow stays substitutable packet for packet.
 *
 * 4. VALIDATION AGAINST THE RECONSTRUCTED BLOCK. buffer_match and aligned are
 *    checked against the reconstructed 35-packet block, not the raw buffered
 *    count. Against the raw count they could never pass: the box buffers roughly
 *    32 packets (no nulls, its own PSI on a timer), so buffer_match and the
 *    %7 alignment both failed on every block even before the count problem.
 *
 * Circuit breaker: re-expressed as a failure RATIO over a rolling window rather
 * than an absolute count. The old "10 failures in 10 seconds" was calibrated
 * against a state machine that burned three markers per failed attempt; once
 * validation passes, blocks arrive at ~31/s and ten failures is well under a
 * second of a transient. See BREAKER_* below.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <librist/librist.h>
#include <librist/udpsocket.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <getopt.h>
#include <stdbool.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#define RIST_MARK_UNUSED(unused_param) ((void)(unused_param))
#define RISTSENDER_VERSION "38-DerivedPacing"
#define MAX_INPUT_COUNT 20
#define MAX_OUTPUT_COUNT 20

// VSF TR-06-4 Part 7 constants
#define TS_PACKET_SIZE 188
#define TS_PACKETS_PER_RTP 7
#define MARKER_PID 0x1FF0
#define MAX_BLOCK_RTP_PAYLOADS 200
/* Packets, not bytes. This was MAX_BLOCK_RTP_PAYLOADS * TS_PACKET_SIZE, which
 * multiplied by the packet SIZE where it meant packets-per-payload: 37,600
 * instead of 1,400. The buffer is allocated as this * TS_PACKET_SIZE, so the
 * box was reserving 6.7 MB of its 128 MB per callback object, and the overflow
 * guard in buffer_ts_packet() could never trip. */
#define MAX_BLOCK_TS_PACKETS (MAX_BLOCK_RTP_PAYLOADS * TS_PACKETS_PER_RTP)

/* Emission pacing.
 *
 * PACKET_INTERVAL_US is now only the FALLBACK used before enough markers have
 * been seen to measure the real cadence. It must not be the steady-state value:
 * a fixed interval encodes one service bitrate, and the block period is set by
 * the headend's marker rate, which varies per service.
 *
 * Why this matters more than it looks. The receiver runs with timing-mode=1
 * (RIST_TIMING_MODE_ARRIVAL), so it stamps packets on ARRIVAL and releases them
 * at arrival+buffer -- there is no re-clocking anywhere downstream. Whatever
 * emission pattern this loop produces is exactly what reaches the decoder. The
 * recovery peer looks smooth because the headend paces it off a real CBR TS;
 * this path has to do that job itself.
 *
 * A fixed interval fails in BOTH directions and neither is benign:
 *   too fast -> the schedule falls behind real time, `now < target` is never
 *               true, usleep never runs, and all payloads of a block go out
 *               back-to-back in microseconds followed by an idle gap;
 *   too slow -> the sender throttles below the input rate, the 6000 socket
 *               backlog grows without bound, latency creeps up and the buffer
 *               eventually overflows into 7-packet-quantised loss.
 * Deriving the interval from the measured cadence avoids both. */
#define PACKET_INTERVAL_US 5560ULL        /* fallback only, until measured */

/* Emit slightly faster than the stream arrives so the backlog can never grow;
 * the sender then idles waiting for input rather than the input queueing behind
 * the sender. 94% leaves headroom without bunching the payloads together. */
#define PACE_DUTY_PCT 94

/* Sanity bounds on the measured block period. Outside these the measurement is
 * not believable (startup, a resync, a stalled feed) and the fallback is used. */
#define PACE_PERIOD_MIN_US 2000ULL
#define PACE_PERIOD_MAX_US 200000ULL

/* EWMA weight for the measured period: new = (old*7 + sample)/8. Slow enough to
 * ignore per-block jitter, fast enough to follow a genuine bitrate change. */
#define PACE_EWMA_SHIFT 3

/* If the schedule drifts more than one block period from now -- in either
 * direction -- resynchronise it instead of trying to catch up or hold back.
 * Without this the absolute schedule accumulates error indefinitely, which is
 * how a small constant mismatch turns into a permanent burst pattern. */
#define PACE_RESYNC_PERIODS 1

#define STATUS_UPDATE_INTERVAL_US 5000000ULL  // 5 seconds

// Circuit breaker thresholds
/* Block geometry, mirrored from headend_part7_sender. The headend emits
 * RTP_PAYLOADS_PER_MARKER payloads of TS_PACKETS_PER_RTP packets, then appends
 * one marker. The marker is NOT part of the content, so the flow we must rebuild
 * is BLOCK_CONTENT_PACKETS long. If the headend's geometry ever changes, this
 * must change with it -- it is no longer derivable from the marker. */
#define RTP_PAYLOADS_PER_MARKER 5
#define BLOCK_CONTENT_PACKETS   (RTP_PAYLOADS_PER_MARKER * TS_PACKETS_PER_RTP)   /* 35 */
#define BLOCK_TOTAL_PACKETS     (BLOCK_CONTENT_PACKETS + 1)                      /* 36 */

/* Circuit breaker, as a failure RATIO over a rolling window. Needs at least
 * BREAKER_MIN_SAMPLES blocks in the window before it can trip, so a short
 * transient cannot fire it at ~31 blocks/s. */
#define BREAKER_WINDOW_US       10000000ULL   /* 10 s */
#define BREAKER_MIN_SAMPLES     60            /* ~2 s of blocks at 31/s */
#define BREAKER_FAIL_RATIO      0.50          /* trip above 50% failing */
#define BREAKER_RING            512           /* rolling outcome history */

/* Marker-silence timeout. The ratio breaker measures QUALITY: it needs blocks
 * to judge, so total silence gives it a zero denominator and it can never trip.
 * Losing the feed entirely is a different failure and needs its own detector.
 *
 * Key on markers, not datagrams: the box's own capture keeps injecting PAT+PMT
 * on a 100ms timer, so datagrams keep arriving with the RF disconnected and any
 * datagram-liveness test stays green. Markers are the only signal that actually
 * tracks the feed -- and keying on them also covers the headend's marker tool
 * dying with RF perfectly healthy, which a tuner-lock test would miss.
 *
 * 400ms is ~12 missed markers at the observed ~32ms cadence, comfortably above
 * the isolated single-marker losses seen in normal operation, and only 5% of the
 * 8s downstream buffer -- so FSR has most of the buffer left to take over. */
#define MARKER_SILENCE_TIMEOUT_US 400000ULL
#define EXIT_MARKER_SILENCE 3

#define CONSECUTIVE_MARKER_LOSS_THRESHOLD 10

/* A marker sequence jump larger than this is an OUTAGE we have already come
 * back from, not a degrading feed. The headend keeps marking while the box is
 * off air, so on RF return the sequence has legitimately advanced by however
 * long the outage lasted -- observed as a 937-marker jump after 23s, which the
 * breaker counted as "937 consecutive marker losses" and tripped on. That cost
 * ~14s of SHUTDOWN/RECOVERY on the recovery link for a feed that was already
 * healthy. Total absence of the feed is NO_SIGNAL's job (400ms); the breaker's
 * consecutive counter is for a feed that is present but rotten. Above this
 * bound we simply resync. ~100 markers is around 3s of stream. */
#define MARKER_RESYNC_JUMP 100
#define VALIDATION_FAILURE_THRESHOLD 10
#define VALIDATION_FAILURE_WINDOW_US 10000000ULL  // 10 seconds
#define RECOVERY_GOOD_BLOCKS_REQUIRED 10
#define RECOVERY_TIME_REQUIRED_US 10000000ULL     // 10 seconds
#define RESTART_MESSAGE_INTERVAL_US 5000000ULL    // 5 seconds

static int signalReceived = 0;
static int peer_connected_count = 0;
static struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;

// Circuit breaker states
enum circuit_state {
    STATE_NORMAL,      // Normal operation
    STATE_SHUTDOWN,    // Catastrophic failure, discarding everything
    STATE_RECOVERY,    // Validating recovery
    STATE_NO_SIGNAL    // Marker silence: the feed is gone, not merely degraded
};

struct rist_ctx_wrap {
    struct rist_ctx *ctx;
    uintptr_t id;
    bool sender;
};

struct marker_data {
    uint32_t marker_sequence;
    uint16_t non_null_count;
    uint16_t null_count;
    uint16_t rtp_sequence_start;
    uint16_t rtp_sequence_next;
    uint32_t source_ssrc;
    uint32_t crc32;
};

struct rist_peer_args {
    char *token;
    enum rist_profile profile;
    enum rist_log_level loglevel;
    char *shared_secret;
    int encryption_type;
    uint32_t recovery_maxbitrate;
    uint32_t recovery_maxbitrate_return;
    uint32_t recovery_length_min;
    uint32_t recovery_length_max;
    uint32_t recovery_reorder_buffer;
    uint32_t recovery_rtt_min;
    uint32_t recovery_rtt_max;
    uint32_t buffer_size;
    uint32_t statsinterval;
    uint32_t stream_id;
    size_t peer_config_count;
    int compression_type;
};

struct rist_callback_object {
    int sd;
    struct evsocket_ctx *evctx;
    struct rist_ctx_wrap *receiver_ctx;
    struct rist_ctx_wrap *sender_ctx;
    struct rist_udp_config *udp_config;
    uint8_t recv[1500];

    // Part 7 block state
    uint8_t *block_buffer;
    size_t block_buffer_capacity;
    size_t block_ts_count;
    uint16_t block_non_null_count;
    uint16_t block_null_count;
    uint16_t block_es_count;      // elementary-stream packets only (compared vs non_null-1)
    uint16_t block_psi_count;     // the box's own PSI: counted, never compared
    uint8_t  send_buffer[BLOCK_CONTENT_PACKETS * TS_PACKET_SIZE];  // reconstructed block
    size_t   send_packet_count;

    // Marker tracking
    bool first_marker_seen;
    bool synchronized;
    bool sender_started;
    uint16_t pending_rtp_seq_start;
    uint32_t pending_ssrc;
    uint32_t last_marker_sequence;

    // CBR timing control
    uint64_t next_send_time_us;

    // RTP timestamp synthesis
    uint64_t rtp_base_ntp_time;
    uint16_t rtp_base_sequence;
    bool rtp_timing_initialized;

    // Statistics
    uint64_t markers_processed;
    uint64_t markers_lost;
    uint64_t blocks_validated;
    uint64_t blocks_dropped;
    uint64_t blocks_sent;
    uint64_t rtp_packets_sent;
    uint64_t dropped_time_held_us;
    uint64_t resync_count;

    /* Datagrams and TS packets actually taken off the input socket. Diff the
     * per-window datagram delta against the capture's own "sent=" over the same
     * wall clock: any shortfall is loss on the UDP hop, which is the only path
     * that can lose in whole 7-packet units. Without this the loss is only
     * visible downstream as "es=22 (expected 29)", which does not say where. */
    uint64_t datagrams_received;
    uint64_t ts_packets_received;
    uint64_t datagrams_last_status;

    // Marker liveness (see MARKER_SILENCE_TIMEOUT_US)
    uint64_t last_marker_time_us;

    /* Measured block cadence, and pacing accounting. paced_slept vs paced_total
     * is the diagnostic that says which way a mismatch went: near-zero sleeps
     * means the schedule was behind and we were bursting; sleeps on nearly
     * every payload means we were throttling the stream. */
    uint64_t marker_period_us;      /* EWMA of the inter-marker interval */
    uint64_t pace_interval_us;      /* per-payload interval actually used */
    uint64_t paced_slept;
    uint64_t paced_total;
    uint64_t paced_resyncs;
    uint64_t paced_slept_last_status;
    uint64_t paced_total_last_status;
    uint64_t paced_resyncs_last_status;

    // Periodic status tracking
    uint64_t last_status_time;
    uint64_t markers_last_status;
    uint64_t blocks_sent_last_status;
    uint64_t blocks_dropped_last_status;

    // Circuit breaker state
    enum circuit_state circuit_state;
    uint32_t consecutive_marker_losses;
    uint64_t validation_failure_timestamps[VALIDATION_FAILURE_THRESHOLD];  // legacy, unused
    uint64_t outcome_time[BREAKER_RING];     // rolling block outcomes for the breaker
    uint8_t  outcome_failed[BREAKER_RING];
    uint64_t outcome_index;
    size_t validation_failure_index;
    uint32_t circuit_breaker_trips;
    
    // Recovery tracking
    uint32_t recovery_good_blocks;
    uint64_t recovery_start_time;
    uint64_t last_restart_message_time;  // New: for restart message timing
    
    // Saved config for sender recreation (kept for future use)
    uint32_t saved_ssrc;
    char *saved_output_url;
    struct rist_peer_args saved_peer_args;
    bool saved_npd;
};

static struct rist_callback_object callback_object[MAX_INPUT_COUNT];
static struct evsocket_event *event[MAX_INPUT_COUNT];
static bool thread_started[MAX_INPUT_COUNT + 1];
static pthread_t thread_main_loop[MAX_INPUT_COUNT + 1];

// CRC-32 ISO/IEC 13818-1
static uint32_t crc32_table[256];
static int crc32_init = 0;

static void crc32_init_table(void) {
    if (crc32_init) return;
    const uint32_t poly = 0x04C11DB7U;
    for (int i = 0; i < 256; ++i) {
        uint32_t c = (uint32_t)i << 24;
        for (int j = 0; j < 8; ++j)
            c = (c & 0x80000000U) ? ((c << 1) ^ poly) : (c << 1);
        crc32_table[i] = c;
    }
    crc32_init = 1;
}

static uint32_t psi_crc32(const uint8_t *buf, int len) {
    if (!crc32_init) crc32_init_table();
    uint32_t crc = 0xFFFFFFFFU;
    for (int i = 0; i < len; ++i) {
        uint8_t idx = (uint8_t)(((crc >> 24) ^ buf[i]) & 0xFF);
        crc = (crc << 8) ^ crc32_table[idx];
    }
    return crc;
}

static uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int is_marker_packet(const uint8_t *ts_packet) {
    if (ts_packet[0] != 0x47) return 0;
    uint16_t pid = ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
    return (pid == MARKER_PID);
}

static int is_null_packet(const uint8_t *ts_packet) {
    if (ts_packet[0] != 0x47) return 0;
    uint16_t pid = ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
    return (pid == 0x1FFF);
}

// -------------------- PSI classification (mirrors headend_part7_sender) ------
// Same exclusion rule as the sender, or the two ends would disagree again:
//   * 0x0000-0x001F, the DVB-reserved range (PAT/CAT/NIT/SDT/BAT/EIT/RST/TDT)
//   * the PMT PID(s), learned from the PAT so a remap is followed untold
//   * anything forced with -x
#define MAX_PMT_PIDS 16
static uint16_t psi_pmt_pids[MAX_PMT_PIDS];
static int      psi_pmt_pid_count = 0;
static uint16_t psi_extra_pids[MAX_PMT_PIDS];
static int      psi_extra_pid_count = 0;

// Last PAT/PMT packets seen, reused as reconstruction filler so the rebuilt
// block still carries real tables rather than only nulls -- the receiver's
// output is decoded by the box's player, which needs PSI to lock on.
static uint8_t  psi_cache[2][TS_PACKET_SIZE];
static int      psi_cache_valid[2] = {0, 0};

static uint16_t ts_pid(const uint8_t *ts_packet) {
    return (uint16_t)(((ts_packet[1] & 0x1F) << 8) | ts_packet[2]);
}

static int psi_pmt_pid_known(uint16_t pid) {
    for (int i = 0; i < psi_pmt_pid_count; i++)
        if (psi_pmt_pids[i] == pid) return 1;
    return 0;
}

static void psi_pmt_pid_add(uint16_t pid) {
    if (pid == 0 || pid >= 0x1FFF) return;
    if (psi_pmt_pid_known(pid)) return;
    if (psi_pmt_pid_count >= MAX_PMT_PIDS) return;
    psi_pmt_pids[psi_pmt_pid_count++] = pid;
    rist_log(&logging_settings, RIST_LOG_INFO,
             "PSI: learned PMT pid 0x%04X from PAT (%d known)\n", pid, psi_pmt_pid_count);
}

static void psi_learn_from_pat(const uint8_t *ts_packet) {
    uint8_t afc = (uint8_t)((ts_packet[3] >> 4) & 0x03);
    int off = 4;

    if ((ts_packet[1] & 0x40) == 0) return;
    if ((afc & 0x01) == 0) return;
    if (afc & 0x02) {
        int af_len = ts_packet[4];
        off = 5 + af_len;
        if (off >= TS_PACKET_SIZE) return;
    }

    int pointer = ts_packet[off];
    off += 1 + pointer;
    if (off + 8 > TS_PACKET_SIZE) return;
    if (ts_packet[off] != 0x00) return;              // table_id must be PAT

    int section_length = ((ts_packet[off + 1] & 0x0F) << 8) | ts_packet[off + 2];
    int section_end    = off + 3 + section_length;
    if (section_length < 9 || section_end > TS_PACKET_SIZE) return;

    for (int p = off + 8; p + 4 <= section_end - 4; p += 4) {
        uint16_t prog_num = (uint16_t)((ts_packet[p] << 8) | ts_packet[p + 1]);
        uint16_t map_pid  = (uint16_t)(((ts_packet[p + 2] & 0x1F) << 8) | ts_packet[p + 3]);
        if (prog_num == 0) continue;                 // network_PID
        psi_pmt_pid_add(map_pid);
    }
}

static int is_psi_packet(const uint8_t *ts_packet) {
    uint16_t pid;

    if (ts_packet[0] != 0x47) return 0;
    pid = ts_pid(ts_packet);

    if (pid <= 0x001F) {
        if (pid == 0x0000) {
            psi_learn_from_pat(ts_packet);
            memcpy(psi_cache[0], ts_packet, TS_PACKET_SIZE);
            psi_cache_valid[0] = 1;
        }
        return 1;
    }
    if (psi_pmt_pid_known(pid)) {
        memcpy(psi_cache[1], ts_packet, TS_PACKET_SIZE);
        psi_cache_valid[1] = 1;
        return 1;
    }
    for (int i = 0; i < psi_extra_pid_count; i++)
        if (psi_extra_pids[i] == pid) return 1;

    return 0;
}

// A TS null packet: valid filler, ignored by every decoder.
static void make_null_packet(uint8_t *out) {
    memset(out, 0xFF, TS_PACKET_SIZE);
    out[0] = 0x47;
    out[1] = 0x1F;      // PID 0x1FFF
    out[2] = 0xFF;
    out[3] = 0x10;      // payload only, cc 0
}

static int parse_marker_packet(const uint8_t *ts_packet, struct marker_data *marker) {
    if (!ts_packet || !marker) return -1;
    if (ts_packet[0] != 0x47) return -1;
    if ((ts_packet[1] & 0x40) == 0) return -1;

    uint8_t pointer = ts_packet[4];
    if (pointer != 0) {
        if (5 + pointer + 27 > TS_PACKET_SIZE)
            return -1;
    }

    const uint8_t *sec = &ts_packet[5 + pointer];
    if (sec[0] != 0xBF) return -1;

    uint8_t b1 = sec[1];
    uint8_t b2 = sec[2];
    if ((b1 & 0x30) != 0x30) return -1;
    uint16_t section_length = (uint16_t)(((b1 & 0x0F) << 8) | b2);
    if (section_length != 24) return -1;

    uint32_t crc_calc = psi_crc32(sec, 23);
    uint32_t crc_pkt =
        ((uint32_t)sec[23] << 24) |
        ((uint32_t)sec[24] << 16) |
        ((uint32_t)sec[25] <<  8) |
         (uint32_t)sec[26];

    if (crc_calc != crc_pkt) return -1;

    marker->marker_sequence =
        ((uint32_t)sec[3]  << 24) |
        ((uint32_t)sec[4]  << 16) |
        ((uint32_t)sec[5]  <<  8) |
         (uint32_t)sec[6];

    marker->non_null_count = (uint16_t)(((uint16_t)sec[7]  << 8) | sec[8]);
    marker->null_count     = (uint16_t)(((uint16_t)sec[9]  << 8) | sec[10]);
    marker->rtp_sequence_start = (uint16_t)(((uint16_t)sec[13] << 8) | sec[14]);
    marker->rtp_sequence_next  = (uint16_t)(((uint16_t)sec[17] << 8) | sec[18]);

    marker->source_ssrc =
        ((uint32_t)sec[19] << 24) |
        ((uint32_t)sec[20] << 16) |
        ((uint32_t)sec[21] <<  8) |
         (uint32_t)sec[22];

    marker->crc32 = crc_pkt;
    return 0;
}

// Circuit breaker as a failure RATIO over a rolling window.
//
// The old rule was an absolute "10 failures in 10 seconds". That was calibrated
// against a state machine which burned three markers per failed attempt, so ten
// failures represented a sustained fault. Once validation passes, blocks arrive
// at roughly 31/s, and ten failures is a third of a second -- a brief transient
// would trip the breaker and take the satellite peer down. A ratio scales with
// bitrate and distinguishes "a few bad blocks" from "this link is broken".
//
// Every block outcome, pass or fail, is recorded, so the denominator is real.
static void record_block_outcome(struct rist_callback_object *cb, bool failed) {
    size_t i = cb->outcome_index % BREAKER_RING;

    cb->outcome_time[i]   = get_timestamp_us();
    cb->outcome_failed[i] = failed ? 1 : 0;
    cb->outcome_index++;
}

static bool check_validation_failure_threshold(struct rist_callback_object *cb) {
    uint64_t now = get_timestamp_us();
    uint64_t cutoff = (now > BREAKER_WINDOW_US) ? (now - BREAKER_WINDOW_US) : 0;
    size_t seen = 0, failed = 0, i;
    double ratio;

    for (i = 0; i < BREAKER_RING; i++) {
        if (cb->outcome_time[i] == 0 || cb->outcome_time[i] < cutoff)
            continue;
        seen++;
        if (cb->outcome_failed[i]) failed++;
    }

    if (seen < BREAKER_MIN_SAMPLES)
        return false;                       // too little evidence to condemn the link

    ratio = (double)failed / (double)seen;
    if (ratio >= BREAKER_FAIL_RATIO) {
        rist_log(&logging_settings, RIST_LOG_WARN,
                 "Breaker: %zu/%zu blocks failed (%.0f%%) over the last %llus\n",
                 failed, seen, ratio * 100.0,
                 (unsigned long long)(BREAKER_WINDOW_US / 1000000ULL));
        return true;
    }
    return false;
}

// Kept for call-site compatibility; the ratio needs both outcomes, so passes are
// recorded too (see record_block_outcome at the success path).
static void record_validation_failure(struct rist_callback_object *cb) {
    record_block_outcome(cb, true);
}

// Trigger circuit breaker shutdown
static void trigger_circuit_breaker(struct rist_callback_object *cb, const char *reason) {
    rist_log(&logging_settings, RIST_LOG_ERROR,
             "\n!!! CIRCUIT BREAKER TRIGGERED !!!\nReason: %s\nShutting down sender, waiting for recovery...\n\n",
             reason);
    
    // Destroy sender context
    if (cb->sender_ctx) {
        rist_destroy(cb->sender_ctx->ctx);
        free(cb->sender_ctx);
        cb->sender_ctx = NULL;
    }
    
    // Reset all state
    cb->sender_started = false;
    cb->first_marker_seen = false;
    cb->synchronized = false;
    cb->block_ts_count = 0;
    cb->block_non_null_count = 0;
    cb->block_null_count = 0;
    cb->consecutive_marker_losses = 0;
    
    // Enter shutdown state
    cb->circuit_state = STATE_SHUTDOWN;
    cb->circuit_breaker_trips++;
    
    // Clear validation failure history
    for (size_t i = 0; i < VALIDATION_FAILURE_THRESHOLD; i++) {
        cb->validation_failure_timestamps[i] = 0;
    }
    cb->validation_failure_index = 0;
}

// Forward declaration
static void connection_status_callback(void *arg, struct rist_peer *peer, enum rist_connection_status st);

// Print restart message instead of recreating sender
static void print_restart_message(struct rist_callback_object *cb) {
    uint64_t now = get_timestamp_us();
    
    // Only print every 5 seconds
    if (cb->last_restart_message_time == 0 || 
        (now - cb->last_restart_message_time) >= RESTART_MESSAGE_INTERVAL_US) {
        
        uint64_t recovery_elapsed = (now - cb->recovery_start_time) / 1000000ULL;
        
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "RESTART SENDER - Recovery complete! %u good blocks over %.1fs (SSRC=0x%08X)\n",
                 cb->recovery_good_blocks, recovery_elapsed / 10.0, cb->saved_ssrc);
        
        cb->last_restart_message_time = now;
    }
}

// Periodic status update
static void print_periodic_status(struct rist_callback_object *cb) {
    uint64_t now = get_timestamp_us();
    
    if (cb->last_status_time == 0) {
        cb->last_status_time = now;
        cb->markers_last_status = cb->markers_processed;
        cb->blocks_sent_last_status = cb->blocks_sent;
        cb->blocks_dropped_last_status = cb->blocks_dropped;
        return;
    }
    
    if ((now - cb->last_status_time) < STATUS_UPDATE_INTERVAL_US) {
        return;
    }
    
    const char *state_str = "UNKNOWN";
    switch (cb->circuit_state) {
        case STATE_NORMAL: state_str = "NORMAL"; break;
        case STATE_SHUTDOWN: state_str = "SHUTDOWN"; break;
        case STATE_RECOVERY: state_str = "RECOVERY"; break;
        case STATE_NO_SIGNAL: state_str = "NO_SIGNAL"; break;
    }

    uint64_t markers_delta = cb->markers_processed - cb->markers_last_status;
    uint64_t dgram_delta   = cb->datagrams_received - cb->datagrams_last_status;
    uint64_t sent_delta = cb->blocks_sent - cb->blocks_sent_last_status;
    uint64_t dropped_delta = cb->blocks_dropped - cb->blocks_dropped_last_status;
    uint64_t total_delta = sent_delta + dropped_delta;
    
    double loss_percent = 0.0;
    if (total_delta > 0) {
        loss_percent = (double)dropped_delta / (double)total_delta * 100.0;
    }
    
    rist_log(&logging_settings, RIST_LOG_INFO,
             "[Status] state=%s markers=%llu (lost=%llu) blocks: sent=%llu dropped=%llu (%.1f%%)"
             " breaker_trips=%llu udp: dgrams=%llu (total=%llu pkts=%llu)"
             " pace: period=%lluus interval=%lluus slept=%llu/%llu resync=%llu\n",
             state_str,
             (unsigned long long)markers_delta,
             (unsigned long long)cb->markers_lost,
             (unsigned long long)sent_delta,
             (unsigned long long)dropped_delta,
             loss_percent,
             (unsigned long long)cb->circuit_breaker_trips,
             (unsigned long long)dgram_delta,
             (unsigned long long)cb->datagrams_received,
             (unsigned long long)cb->ts_packets_received,
             (unsigned long long)cb->marker_period_us,
             (unsigned long long)cb->pace_interval_us,
             (unsigned long long)(cb->paced_slept - cb->paced_slept_last_status),
             (unsigned long long)(cb->paced_total - cb->paced_total_last_status),
             (unsigned long long)(cb->paced_resyncs - cb->paced_resyncs_last_status));
    
    if (cb->circuit_state == STATE_RECOVERY) {
        uint64_t recovery_elapsed = (now - cb->recovery_start_time) / 1000000ULL;
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "[Recovery] good_blocks=%u/%u elapsed=%llus/10s\n",
                 cb->recovery_good_blocks, RECOVERY_GOOD_BLOCKS_REQUIRED,
                 (unsigned long long)recovery_elapsed);
    }
    
    cb->last_status_time = now;
    cb->markers_last_status = cb->markers_processed;
    cb->blocks_sent_last_status = cb->blocks_sent;
    cb->blocks_dropped_last_status = cb->blocks_dropped;
    cb->datagrams_last_status = cb->datagrams_received;
    cb->paced_slept_last_status = cb->paced_slept;
    cb->paced_total_last_status = cb->paced_total;
    cb->paced_resyncs_last_status = cb->paced_resyncs;
}

/* Marker silence => the feed is gone. Exit, so the RIST sender context dies with
 * the process and the satellite peer goes genuinely dead downstream.
 *
 * Exiting rather than pausing internally is the whole point. librist's peer-death
 * test counts ANY packet, including RTCP keepalives, so a process that merely
 * stops emitting data keeps its peer alive -- observed on hardware as
 * "received=0, dead=NO, time_since_pkt=20ms". FSR activation is gated on
 * !sat_peer, so a peer that never dies means FSR never engages, which is exactly
 * the bug. Only tearing the context down produces a dead peer.
 *
 * The watchdog restarts us into wait-for-first-marker, where rist_start() is
 * only reached inside "if (!cb->first_marker_seen)". So the restarted process
 * emits nothing and the peer STAYS dead until markers actually return. */
static void check_marker_silence(struct rist_callback_object *cb) {
    uint64_t now, silent_us;

    /* Only once synchronised: before the first marker there is nothing to have
     * lost, and tripping during startup would fight the watchdog. */
    if (!cb->first_marker_seen || cb->last_marker_time_us == 0)
        return;
    if (cb->circuit_state == STATE_NO_SIGNAL)
        return;

    now = get_timestamp_us();
    if (now < cb->last_marker_time_us)
        return;                         /* clock went backwards; wait it out */

    silent_us = now - cb->last_marker_time_us;
    if (silent_us < MARKER_SILENCE_TIMEOUT_US)
        return;

    cb->circuit_state = STATE_NO_SIGNAL;

    /* Report datagrams too: if those are still climbing while markers stopped,
     * the input socket is healthy and the FEED is gone -- which distinguishes
     * this from the socket-overrun loss that shows up as short blocks. */
    rist_log(&logging_settings, RIST_LOG_ERROR,
             "\n!!! NO SIGNAL !!!\nNo marker for %llu ms (threshold %llu ms) after %llu markers.\n"
             "udp still flowing: dgrams=%llu pkts=%llu (PSI injection continues with no feed)\n"
             "The ratio breaker cannot see this -- no blocks means no denominator.\n"
             "Exiting so the satellite peer dies and FSR can take over; the watchdog\n"
             "will restart us into wait-for-first-marker.\n\n",
             (unsigned long long)(silent_us / 1000ULL),
             (unsigned long long)(MARKER_SILENCE_TIMEOUT_US / 1000ULL),
             (unsigned long long)cb->markers_processed,
             (unsigned long long)cb->datagrams_received,
             (unsigned long long)cb->ts_packets_received);

    fflush(stdout);
    fflush(stderr);
    exit(EXIT_MARKER_SILENCE);
}

// Rebuild the block to exactly BLOCK_CONTENT_PACKETS before emitting.
//
// The box captured only ES: its PID-filtered capture never saw the headend's
// nulls, and its own PSI is an unrelated local injection. Emitting just what was
// captured would produce a short block and put every subsequent RTP payload
// boundary in the wrong place, so the recovery peer's flow would no longer be
// substitutable packet for packet. Refill with the counts the marker implies:
// ES, then psi_needed PSI packets, then null_count nulls.
//
// psi_needed comes from the structural total, NOT from non_null + null -- that
// sum no longer equals the block size now that the sender excludes PSI.
static size_t reconstruct_block(struct rist_callback_object *cb,
                                const struct marker_data *marker,
                                uint8_t *out, size_t out_max_packets) {
    size_t es = cb->block_ts_count;
    size_t nulls = marker->null_count;
    size_t psi_needed;
    size_t n = 0, i;
    uint16_t non_null = marker->non_null_count;

    // non_null always includes the marker, so 0 means a malformed or very-first
    // marker. Unreachable in practice (counts_match gates it), but refuse rather
    // than rebuild a block of pure filler.
    if (non_null == 0) return 0;

    if ((size_t)(non_null - 1) + nulls > BLOCK_TOTAL_PACKETS - 1)
        return 0;                                   // marker implies more than a block
    psi_needed = BLOCK_TOTAL_PACKETS - non_null - nulls;

    if (es + psi_needed + nulls != BLOCK_CONTENT_PACKETS) return 0;
    if (es + psi_needed + nulls > out_max_packets)   return 0;

    memcpy(out, cb->block_buffer, es * TS_PACKET_SIZE);
    n = es;

    // Real tables where we have them, so the rebuilt stream still carries PSI
    // for the box's own decoder; nulls only as a fallback before the first PAT.
    for (i = 0; i < psi_needed; i++, n++) {
        int which = (int)(i % 2);
        if (psi_cache_valid[which])
            memcpy(out + n * TS_PACKET_SIZE, psi_cache[which], TS_PACKET_SIZE);
        else if (psi_cache_valid[!which])
            memcpy(out + n * TS_PACKET_SIZE, psi_cache[!which], TS_PACKET_SIZE);
        else
            make_null_packet(out + n * TS_PACKET_SIZE);
    }

    for (i = 0; i < nulls; i++, n++)
        make_null_packet(out + n * TS_PACKET_SIZE);

    return n;
}

static void send_block_to_rist(struct rist_callback_object *cb) {
    if (!peer_connected_count) {
        return;
    }

    size_t total_packets = cb->send_packet_count;   // the RECONSTRUCTED block
    if (total_packets == 0) return;

    size_t num_rtp_payloads = total_packets / TS_PACKETS_PER_RTP;

    uint16_t current_rtp_seq = cb->pending_rtp_seq_start;

    /* Per-payload interval derived from the MEASURED block period, spread over
     * PACE_DUTY_PCT of it so we drain a little faster than the stream arrives.
     * Falls back to the compiled constant only until enough markers have been
     * seen to measure anything. */
    {
        uint64_t period = cb->marker_period_us;
        if (period < PACE_PERIOD_MIN_US || period > PACE_PERIOD_MAX_US || num_rtp_payloads == 0)
            cb->pace_interval_us = PACKET_INTERVAL_US;
        else
            cb->pace_interval_us = (period * PACE_DUTY_PCT / 100) / num_rtp_payloads;
        if (cb->pace_interval_us == 0)
            cb->pace_interval_us = 1;
    }

    /* Resynchronise the absolute schedule whenever it has drifted more than one
     * block period from now, in either direction. An unclamped schedule
     * accumulates every small mismatch until it is permanently ahead (throttling
     * the stream) or permanently behind (never sleeping, emitting bursts) -- the
     * latter is what a fixed interval produced here. */
    {
        uint64_t now0 = get_timestamp_us();
        uint64_t span = (cb->pace_interval_us * num_rtp_payloads) * PACE_RESYNC_PERIODS;
        if (!cb->next_send_time_us ||
            cb->next_send_time_us + span < now0 ||
            now0 + span < cb->next_send_time_us) {
            if (cb->next_send_time_us)
                cb->paced_resyncs++;
            cb->next_send_time_us = now0;
        }
    }

    for (size_t i = 0; i < num_rtp_payloads; i++) {
        uint64_t now = get_timestamp_us();
        uint64_t target = cb->next_send_time_us;
        cb->paced_total++;
        if (now < target) {
            uint64_t wait_us = target - now;
            if (wait_us < 500000ULL) {
                cb->paced_slept++;
                usleep((useconds_t)wait_us);
            }
        }

        struct rist_data_block db = {0};
        db.payload     = &cb->send_buffer[i * TS_PACKETS_PER_RTP * TS_PACKET_SIZE];
        db.payload_len = TS_PACKETS_PER_RTP * TS_PACKET_SIZE;
        db.seq         = current_rtp_seq;
        db.flags       = RIST_DATA_FLAGS_USE_SEQ;

        uint64_t current_ntp = get_timestamp_us() * ((1ULL << 32) / 1000000ULL);
        uint64_t buffering_delay_ntp = 100000ULL * ((1ULL << 32) / 1000000ULL);
        db.ts_ntp = current_ntp - buffering_delay_ntp;

        db.flow_id     = cb->pending_ssrc;

        int ret = rist_sender_data_write(cb->sender_ctx->ctx, &db);
        if (ret < 0) {
            rist_log(&logging_settings, RIST_LOG_ERROR,
                     "Failed to send RTP payload (seq=%u): %d\n", current_rtp_seq, ret);
        } else {
            cb->rtp_packets_sent++;
        }

        cb->next_send_time_us = target + cb->pace_interval_us;
        current_rtp_seq++;
    }

    cb->blocks_sent++;
}

static void process_marker(struct rist_callback_object *cb, const uint8_t *marker_packet) {
    struct marker_data marker;

    // STEP 1: CRC32 check - must trust the marker data
    if (parse_marker_packet(marker_packet, &marker) != 0) {
        rist_log(&logging_settings, RIST_LOG_WARN, 
                 "Marker parse/CRC failed - ignoring\n");
        return;
    }

    cb->markers_processed++;

    /* Measure the real block cadence here, before last_marker_time_us is
     * overwritten. This is the headend's marker rate, i.e. the true period the
     * emitter has to match; deriving it beats any compiled-in constant, which
     * can only ever be right for one service bitrate. Bounded so a startup
     * gap, a resync or a stalled feed cannot poison the average. */
    {
        uint64_t mnow = get_timestamp_us();
        if (cb->last_marker_time_us && mnow > cb->last_marker_time_us) {
            uint64_t sample = mnow - cb->last_marker_time_us;
            if (sample >= PACE_PERIOD_MIN_US && sample <= PACE_PERIOD_MAX_US) {
                if (!cb->marker_period_us)
                    cb->marker_period_us = sample;
                else
                    cb->marker_period_us +=
                        ((int64_t)sample - (int64_t)cb->marker_period_us) >> PACE_EWMA_SHIFT;
            }
        }
        cb->last_marker_time_us = mnow;   /* liveness, see check_marker_silence */
    }

    // Handle different circuit breaker states
    if (cb->circuit_state == STATE_SHUTDOWN) {
        // In SHUTDOWN, accept ANY good marker as potential recovery start
        // Don't require exact sequence - we may have missed packets during failure
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "Good marker in SHUTDOWN (seq=%u), entering RECOVERY\n",
                 marker.marker_sequence);
        
        cb->circuit_state = STATE_RECOVERY;
        cb->recovery_good_blocks = 0;
        cb->recovery_start_time = get_timestamp_us();
        cb->last_restart_message_time = 0;  // Reset restart message timer
        cb->synchronized = false;
        cb->first_marker_seen = true;
        cb->last_marker_sequence = marker.marker_sequence;
        cb->saved_ssrc = marker.source_ssrc;
        cb->block_ts_count = 0;
        cb->block_non_null_count = 1;
        cb->block_null_count = 0;
        cb->block_es_count = 0;
        cb->block_psi_count = 0;
        cb->block_es_count = 0;
        cb->block_psi_count = 0;
        cb->consecutive_marker_losses = 0;
        return;
    }

    // STEP 2: First marker initialization (NORMAL state only)
    if (!cb->first_marker_seen) {
        cb->first_marker_seen = true;
        cb->synchronized = false;
        cb->pending_ssrc = marker.source_ssrc;
        cb->last_marker_sequence = marker.marker_sequence;
        cb->saved_ssrc = marker.source_ssrc;

        cb->rtp_base_ntp_time = get_timestamp_us() * ((1ULL << 32) / 1000000ULL);
        cb->rtp_base_sequence = marker.rtp_sequence_start;
        cb->rtp_timing_initialized = true;

        rist_log(&logging_settings, RIST_LOG_INFO,
                 "First marker received: SSRC=0x%08X rtp_seq=%u marker_seq=%u (waiting for sync)\n",
                 marker.source_ssrc, marker.rtp_sequence_start, marker.marker_sequence);

        if (!cb->sender_started && cb->circuit_state == STATE_NORMAL) {
            if (rist_sender_flow_id_set(cb->sender_ctx->ctx, marker.source_ssrc) != 0) {
                rist_log(&logging_settings, RIST_LOG_ERROR,
                         "Failed to set sender flow_id to 0x%08X\n", marker.source_ssrc);
            }

            if (rist_start(cb->sender_ctx->ctx) == -1) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start RIST sender\n");
            } else {
                cb->sender_started = true;
                rist_log(&logging_settings, RIST_LOG_INFO,
                         "RIST sender started with SSRC=0x%08X\n", marker.source_ssrc);
            }
        }

        cb->block_ts_count = 0;
        cb->block_non_null_count = 1;
        cb->block_null_count = 0;
        cb->block_es_count = 0;
        cb->block_psi_count = 0;
        return;
    }

    // STEP 3: Check marker sequence
    uint32_t expected_marker_seq = cb->last_marker_sequence + 1;
    bool sequence_good = (marker.marker_sequence == expected_marker_seq);
    
    if (!sequence_good) {
        uint32_t lost_markers = marker.marker_sequence - expected_marker_seq;
        
        rist_log(&logging_settings, RIST_LOG_WARN,
                 "Marker sequence discontinuity! Expected %u, got %u (%u lost)\n",
                 expected_marker_seq, marker.marker_sequence, lost_markers);
        
        cb->markers_lost += lost_markers;

        /* A jump this large is an outage we have already returned from, not a
         * degrading feed: the headend keeps marking while the box is off air,
         * so the sequence advances by the whole outage. Counting that as
         * "consecutive marker losses" tripped the breaker on RF RETURN --
         * observed as a 937-marker jump after 23s, costing ~14s of
         * SHUTDOWN/RECOVERY on the recovery link for a feed that was already
         * healthy again. Feed absence is NO_SIGNAL's job at 400ms; the breaker
         * is for a feed that is present but rotten. Resync and carry on. */
        if (lost_markers > MARKER_RESYNC_JUMP) {
            rist_log(&logging_settings, RIST_LOG_INFO,
                     "Marker jump of %u (>%d) -- treating as resync after an outage,"
                     " not loss; not counting toward the breaker\n",
                     lost_markers, MARKER_RESYNC_JUMP);
            cb->consecutive_marker_losses = 0;
            cb->resync_count++;
            cb->synchronized = false;
            cb->last_marker_sequence = marker.marker_sequence;
            cb->block_ts_count = 0;
            cb->block_non_null_count = 1;
            cb->block_null_count = 0;
            cb->block_es_count = 0;
            cb->block_psi_count = 0;
            /* Drop the schedule too: it is stale by the length of the outage. */
            cb->next_send_time_us = 0;
            return;
        }

        cb->consecutive_marker_losses += lost_markers;

        // Check for circuit breaker trigger
        if (cb->consecutive_marker_losses >= CONSECUTIVE_MARKER_LOSS_THRESHOLD) {
            char reason[256];
            snprintf(reason, sizeof(reason), 
                     "%u consecutive marker losses", cb->consecutive_marker_losses);
            trigger_circuit_breaker(cb, reason);
            return;
        }
        
        if (cb->circuit_state == STATE_RECOVERY) {
            // Recovery failed, back to shutdown
            rist_log(&logging_settings, RIST_LOG_WARN,
                     "Marker loss during RECOVERY, back to SHUTDOWN\n");
            cb->circuit_state = STATE_SHUTDOWN;
            cb->first_marker_seen = false;
            cb->synchronized = false;
            return;
        }
        
        // Not circuit breaker, just resync
        cb->blocks_dropped += lost_markers;
        cb->resync_count++;
        cb->synchronized = false;
        cb->last_marker_sequence = marker.marker_sequence;
        
        if (cb->block_ts_count > 0) {
            rist_log(&logging_settings, RIST_LOG_INFO,
                     "Discarding %zu buffered packets for resync\n", cb->block_ts_count);
        }
        
        cb->block_ts_count = 0;
        cb->block_non_null_count = 1;
        cb->block_null_count = 0;
        cb->block_es_count = 0;
        cb->block_psi_count = 0;
        cb->block_es_count = 0;
        cb->block_psi_count = 0;

        return;
    }
    
    // Sequence is good
    cb->last_marker_sequence = marker.marker_sequence;
    cb->consecutive_marker_losses = 0;

    // STEP 4: Validate block (if synchronized)
    bool validation_succeeded = false;
    
    if (cb->synchronized && cb->block_ts_count > 0) {
        // ES ONLY. The marker's non_null is marker + ES, so ours must be
        // non_null - 1. Nulls are not compared (a PID-filtered capture never
        // sees any) and PSI is not compared (ours is a local injection).
        size_t expected_es = marker.non_null_count ? (size_t)marker.non_null_count - 1 : 0;
        bool counts_match  = ((size_t)cb->block_es_count == expected_es);

        // Reconstruct, then judge the REBUILT block. Against the raw buffered
        // count these two could never pass: the box holds ~32 packets, so the
        // total was never 36 and never a multiple of 7.
        cb->send_packet_count = counts_match
            ? reconstruct_block(cb, &marker, cb->send_buffer, BLOCK_CONTENT_PACKETS)
            : 0;

        bool buffer_match = (cb->send_packet_count == BLOCK_CONTENT_PACKETS);
        bool aligned      = (cb->send_packet_count % TS_PACKETS_PER_RTP == 0);

        if (counts_match && aligned && buffer_match) {
            validation_succeeded = true;
            
            // Only send in NORMAL state
            if (cb->circuit_state == STATE_NORMAL) {
                cb->pending_rtp_seq_start = marker.rtp_sequence_start;
                cb->pending_ssrc = marker.source_ssrc;
                send_block_to_rist(cb);
            }
            
            cb->blocks_validated++;
            record_block_outcome(cb, false);   // the breaker needs passes as well
            
            // Handle recovery state - just print message instead of recreating sender
            if (cb->circuit_state == STATE_RECOVERY) {
                cb->recovery_good_blocks++;
                uint64_t elapsed = get_timestamp_us() - cb->recovery_start_time;
                
                if (cb->recovery_good_blocks >= RECOVERY_GOOD_BLOCKS_REQUIRED && 
                    elapsed >= RECOVERY_TIME_REQUIRED_US) {
                    
                    // Print restart message every 5 seconds instead of recreating sender
                    print_restart_message(cb);
                    
                    // Stay in recovery state and keep printing messages
                    // Don't transition back to NORMAL state
                }
            }
        } else {
            // Validation failed
            rist_log(&logging_settings, RIST_LOG_WARN,
                     "Block #%u validation failed: es=%u (expected %zu) psi_seen=%u null_seen=%u"
                     " | marker: non_null=%u null=%u -> psi_needed=%d | rebuilt=%zu/%d\n",
                     marker.marker_sequence, cb->block_es_count, expected_es,
                     cb->block_psi_count, cb->block_null_count,
                     marker.non_null_count, marker.null_count,
                     (int)BLOCK_TOTAL_PACKETS - (int)marker.non_null_count - (int)marker.null_count,
                     cb->send_packet_count, BLOCK_CONTENT_PACKETS);
            
            record_validation_failure(cb);
            cb->blocks_dropped++;
            
            // Check for circuit breaker trigger
            if (check_validation_failure_threshold(cb)) {
                trigger_circuit_breaker(cb, "10 validation failures within 10 seconds");
                return;
            }
            
            // Check if in recovery
            if (cb->circuit_state == STATE_RECOVERY) {
                rist_log(&logging_settings, RIST_LOG_WARN,
                         "Validation failure during RECOVERY, back to SHUTDOWN\n");
                cb->circuit_state = STATE_SHUTDOWN;
                cb->first_marker_seen = false;
                cb->synchronized = false;
                return;
            }
        }
    } else if (!cb->synchronized) {
        // Second marker after resync/first marker
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "Synchronized! Marker %u received, will validate next block\n",
                 marker.marker_sequence);
        cb->synchronized = true;
    }

    // ALWAYS reset for next block
    cb->block_ts_count = 0;
    cb->block_non_null_count = 1;   // the marker that starts the next block
    cb->block_null_count = 0;
    cb->block_es_count = 0;
    cb->block_psi_count = 0;
}

// Classify and count. PSI is recognised (and cached) but NOT buffered for emit:
// the box's PSI is its own injection on a 100ms timer, unrelated to the
// headend's, so it is replaced during reconstruction by exactly the PSI count
// the marker implies. Only ES is buffered -- it is the payload that must survive
// byte for byte.
static void buffer_ts_packet(struct rist_callback_object *cb, const uint8_t *ts_packet) {
    int psi;

    if (is_null_packet(ts_packet)) {
        cb->block_null_count++;       // never happens on a PID-filtered capture
        return;
    }

    psi = is_psi_packet(ts_packet);   // also refreshes the PAT/PMT cache
    if (psi) {
        cb->block_psi_count++;
        return;
    }

    cb->block_non_null_count++;
    cb->block_es_count++;

    // In RECOVERY state, count only - don't buffer, to save memory
    if (cb->circuit_state == STATE_RECOVERY) {
        cb->block_ts_count++;
        return;
    }

    if (cb->block_ts_count >= MAX_BLOCK_TS_PACKETS) {
        rist_log(&logging_settings, RIST_LOG_ERROR,
                 "Block buffer overflow! count=%zu\n", cb->block_ts_count);
        return;
    }

    memcpy(&cb->block_buffer[cb->block_ts_count * TS_PACKET_SIZE], ts_packet, TS_PACKET_SIZE);
    cb->block_ts_count++;
}

static void input_udp_recv(struct evsocket_ctx *evctx, int fd, short revents, void *arg)
{
    struct rist_callback_object *cb = (void *) arg;
    RIST_MARK_UNUSED(evctx);
    RIST_MARK_UNUSED(revents);
    RIST_MARK_UNUSED(fd);

    ssize_t recv_bufsize = -1;
    struct sockaddr_in addr4 = {0};
    struct sockaddr_in6 addr6 = {0};
    uint8_t *recv_buf = cb->recv;
    socklen_t addrlen = 0;
    unsigned drained = 0;

    uint16_t address_family = (uint16_t)cb->udp_config->address_family;

    /* Drain everything pending, not one datagram per readiness event.
     *
     * The socket is where our loss happens: whole 1316-byte datagrams are
     * discarded when it overflows, which is the only path that can lose in
     * exact multiples of 7 TS packets. Everything below this point -- block
     * reconstruction, the paced emit, logging -- runs on this thread, so any
     * time spent there is time the socket is filling. Draining one datagram per
     * poll() meant a backlog took as many poll/recv round trips to clear as it
     * had datagrams, and the capture can hand us up to 36 at once (it reads
     * 188*256 bytes per demux read and sends them back to back).
     *
     * DRAIN_MAX bounds the loop so a sustained overrun cannot starve the rest
     * of the event loop -- including the marker-silence check. */
    #define DRAIN_MAX 256
    while (drained < DRAIN_MAX) {
    if (address_family == AF_INET6) {
        addrlen = sizeof(struct sockaddr_in6);
        recv_bufsize = udpsocket_recvfrom(cb->sd, recv_buf, sizeof(cb->recv), MSG_DONTWAIT, (struct sockaddr *) &addr6, &addrlen);
    } else {
        addrlen = sizeof(struct sockaddr_in);
        recv_bufsize = udpsocket_recvfrom(cb->sd, recv_buf, sizeof(cb->recv), MSG_DONTWAIT, (struct sockaddr *) &addr4, &addrlen);
    }

    if (recv_bufsize > 0) {
        int offset = 0;

        drained++;
        cb->datagrams_received++;

        while (offset + TS_PACKET_SIZE <= recv_bufsize) {
            uint8_t *ts_packet = &recv_buf[offset];

            if (ts_packet[0] != 0x47) {
                static int sync_error_count = 0;
                if (sync_error_count++ < 1) {
                    rist_log(&logging_settings, RIST_LOG_ERROR,
                             "Invalid TS sync byte: 0x%02X (further sync errors suppressed)\n", 
                             ts_packet[0]);
                }
                offset += TS_PACKET_SIZE;
                continue;
            }

            if (is_marker_packet(ts_packet)) {
                process_marker(cb, ts_packet);
            } else {
                // In SHUTDOWN, discard non-marker packets
                if (cb->circuit_state == STATE_SHUTDOWN) {
                    offset += TS_PACKET_SIZE;
                    continue;
                }
                
                if (cb->first_marker_seen) {
                    buffer_ts_packet(cb, ts_packet);
                }
            }

            cb->ts_packets_received++;
            offset += TS_PACKET_SIZE;
        }
    } else {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            rist_log(&logging_settings, RIST_LOG_ERROR,
                     "Input receive failed: errno=%d\n", errno);
        }
        break;                          /* socket drained (or a real error) */
    }
    }
    #undef DRAIN_MAX

    /* print_periodic_status() used to be called here, once per datagram. It is
     * a blocking write to a serial console shared with every other process on
     * the box, on the same thread that has to get back to recvfrom(). It now
     * runs from input_loop(), which also reaches it when no datagrams arrive at
     * all -- so the status line keeps coming during a signal loss instead of
     * going quiet exactly when it is most wanted. */
}

static void input_udp_sockerr(struct evsocket_ctx *evctx, int fd, short revents, void *arg)
{
    struct rist_callback_object *cb = (void *) arg;
    RIST_MARK_UNUSED(evctx);
    RIST_MARK_UNUSED(revents);
    RIST_MARK_UNUSED(fd);
    rist_log(&logging_settings, RIST_LOG_ERROR, "Socket error on sd=%d\n", cb->sd);
}

static char help_str[] = "librist, VSF TR-06-4 Part 7 RIST Sender (marker stripper with circuit breaker)";

static void usage(char *cmd)
{
    fprintf(stderr, "%s\n%s version %s\nlibRIST: %s API: %s\n",
             cmd, help_str, RISTSENDER_VERSION, librist_version(), librist_api_version());
    exit(1);
}

static void connection_status_callback(void *arg, struct rist_peer *peer, enum rist_connection_status st)
{
    (void)arg;
    (void)peer;
    if (st == RIST_CONNECTION_ESTABLISHED || st == RIST_CLIENT_CONNECTED) {
        peer_connected_count++;
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "Peer connected (total: %d)\n", peer_connected_count);
    } else {
        peer_connected_count--;
        rist_log(&logging_settings, RIST_LOG_WARN,
                 "Peer disconnected (total: %d)\n", peer_connected_count);
    }
}

static int cb_auth_connect(void *arg, const char* connecting_ip, uint16_t connecting_port, const char* local_ip, uint16_t local_port, struct rist_peer *peer)
{
    struct rist_ctx_wrap *w = (struct rist_ctx_wrap *)arg;
    (void)w; (void)peer; (void)local_ip; (void)local_port;
    rist_log(&logging_settings, RIST_LOG_INFO,"Peer auth: %s:%d\n", connecting_ip, connecting_port);
    return 0;
}

static int cb_auth_disconnect(void *arg, struct rist_peer *peer)
{
    struct rist_ctx_wrap *w = (struct rist_ctx_wrap *)arg;
    (void)w; (void)peer;
    return 0;
}

static void intHandler(int signal) {
    if (signal == SIGINT) {
        rist_log(&logging_settings, RIST_LOG_INFO, "Shutting down (signal %d)\n", signal);
    }
    signalReceived = signal;
}

static struct rist_peer *setup_rist_peer(struct rist_ctx_wrap *ctx_wrap, struct rist_peer_args *peer_args)
{
    struct rist_peer_config *peer_config_link = NULL;
    if (rist_parse_address2(peer_args->token, &peer_config_link))
    {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse address %s\n", peer_args->token);
        goto cleanup;
    }

    if (peer_args->profile != RIST_PROFILE_SIMPLE && peer_config_link)
    {
        if (peer_args->shared_secret && strlen(peer_args->shared_secret) > 0)
        {
            peer_config_link->secret[0] = 0;
            strncpy(peer_config_link->secret, peer_args->shared_secret, RIST_MAX_STRING_SHORT -1);
            if (peer_args->encryption_type)
                peer_config_link->key_size = peer_args->encryption_type;
            else
                peer_config_link->key_size = 128;
        }
        peer_config_link->recovery_mode = RIST_RECOVERY_MODE_TIME;
    }

    struct rist_peer *peer;
    if (rist_peer_create(ctx_wrap->ctx, &peer, peer_config_link) == -1) {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not add peer %s\n", peer_args->token);
        goto cleanup;
    }

    rist_peer_config_free2(&peer_config_link);
    return peer;

cleanup:
    rist_peer_config_free2(&peer_config_link);
    return NULL;
}

static void *input_loop(void *arg)
{
    struct rist_callback_object *cb = (void *) arg;

    while (!signalReceived) {
        if (cb->receiver_ctx)
        {
            struct rist_data_block *b = NULL;
            int queue_size = rist_receiver_data_read2(cb->receiver_ctx->ctx, &b, 5);
            if (queue_size > 0) {
                if (queue_size % 10 == 0 || queue_size > 50)
                    rist_log(&logging_settings, RIST_LOG_WARN, "Falling behind: queue=%d\n", queue_size);
                if (b && b->payload) {
                    if (peer_connected_count) {
                        int w = rist_sender_data_write(cb->sender_ctx->ctx, b);
                        (void) w;
                    }
                    rist_receiver_data_block_free2(&b);
                }
            }
        }
        else
        {
            evsocket_loop_single(cb->evctx, 5, 100);

            /* Off the per-datagram path deliberately. This branch runs every
             * ~5ms whether or not anything arrived, which is what the silence
             * check needs: with the RF pulled, datagrams keep coming (the
             * capture's own PSI injection) but markers stop, and with the feed
             * fully gone neither arrives. Both cases are covered from here. */
            check_marker_silence(cb);
            print_periodic_status(cb);
        }
    }
    return 0;
}

static struct rist_ctx_wrap *configure_rist_output_context(char* outputurl,
    struct rist_peer_args *peer_args, const struct rist_udp_config *udp_config,
    bool npd, enum rist_profile profile)
{
    (void)udp_config; (void)profile;

    struct rist_ctx *sender_ctx;
    if (rist_sender_create(&sender_ctx, peer_args->profile, 0, &logging_settings) != 0) {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not create sender context\n");
        return NULL;
    }
    struct rist_ctx_wrap *w = malloc(sizeof(*w));
    memset(w, 0, sizeof(*w));
    w->ctx = sender_ctx;
    w->sender = true;

    if (rist_auth_handler_set(sender_ctx, cb_auth_connect, cb_auth_disconnect, w) != 0) {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not init auth handler\n");
        goto fail;
    }

    if (rist_connection_status_callback_set(sender_ctx, connection_status_callback, w) == -1) {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not init connection callback\n");
        goto fail;
    }

    if (npd) {
        rist_log(&logging_settings, RIST_LOG_INFO, "NPD enabled\n");
        if (rist_sender_npd_enable(sender_ctx) != 0) {
            rist_log(&logging_settings, RIST_LOG_ERROR, "Failed to enable NPD\n");
        }
    }

    char *saveptroutput;
    char *tmpoutputurl = malloc(strlen(outputurl) + 1);
    strcpy(tmpoutputurl, outputurl);
    char *outputtoken = strtok_r(tmpoutputurl, ",", &saveptroutput);

    while (outputtoken) {
        peer_args->token = outputtoken;
        peer_args->stream_id = udp_config ? udp_config->stream_id : 0;
        struct rist_peer *peer = setup_rist_peer(w, peer_args);
        if (peer == NULL) {
            free(tmpoutputurl);
            goto fail;
        }
        outputtoken = strtok_r(NULL, ",", &saveptroutput);
    }
    free(tmpoutputurl);

    return w;

fail:
    rist_destroy(sender_ctx);
    free(w);
    return NULL;
}

static void print_statistics(struct rist_callback_object *cb) {
    double loss_percent = 0.0;
    if (cb->blocks_validated + cb->blocks_dropped > 0) {
        loss_percent = (double)cb->blocks_dropped / 
                      (double)(cb->blocks_validated + cb->blocks_dropped) * 100.0;
    }
    
    const char *state_str = "UNKNOWN";
    switch (cb->circuit_state) {
        case STATE_NORMAL: state_str = "NORMAL"; break;
        case STATE_SHUTDOWN: state_str = "SHUTDOWN"; break;
        case STATE_RECOVERY: state_str = "RECOVERY"; break;
        case STATE_NO_SIGNAL: state_str = "NO_SIGNAL"; break;
    }

    rist_log(&logging_settings, RIST_LOG_INFO,
             "\n=== Final Statistics ===\n");
    rist_log(&logging_settings, RIST_LOG_INFO, 
             "State: %s | Circuit breaker trips: %llu\n",
             state_str, (unsigned long long)cb->circuit_breaker_trips);
    rist_log(&logging_settings, RIST_LOG_INFO, 
             "Markers: %llu (lost=%llu resyncs=%llu) | Blocks: sent=%llu dropped=%llu (%.2f%%) | RTP: %llu\n",
             (unsigned long long)cb->markers_processed,
             (unsigned long long)cb->markers_lost,
             (unsigned long long)cb->resync_count,
             (unsigned long long)cb->blocks_sent,
             (unsigned long long)cb->blocks_dropped,
             loss_percent,
             (unsigned long long)cb->rtp_packets_sent);
    /* Whole-run totals for the UDP hop. Compare "dgrams" against the capture's
     * final "udp sent=" for the same run: they should be equal, and any deficit
     * is datagrams the kernel discarded at our socket. */
    rist_log(&logging_settings, RIST_LOG_INFO,
             "UDP input: dgrams=%llu ts_packets=%llu (compare dgrams against the capture's 'udp sent=')\n",
             (unsigned long long)cb->datagrams_received,
             (unsigned long long)cb->ts_packets_received);
}

int main(int argc, char *argv[])
{
    int c;
    char *inputurl = NULL;
    char *outputurl = NULL;
    enum rist_profile profile = RIST_PROFILE_MAIN;
    char *shared_secret = NULL;
    int encryption_type = 0;
    int statsinterval = 0;
    int verbosity = RIST_LOG_INFO;
    int buffer_size = 0;
    bool npd = false;

    struct rist_peer_args peer_args;
    memset(&peer_args, 0, sizeof(peer_args));
    peer_args.recovery_length_min = 1000;
    peer_args.recovery_length_max = 1000;
    peer_args.recovery_reorder_buffer = 25;
    peer_args.recovery_rtt_min = 50;
    peer_args.recovery_rtt_max = 500;

    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        memset(&callback_object[i], 0, sizeof(callback_object[i]));
        callback_object[i].circuit_state = STATE_NORMAL;
        event[i] = NULL;
        thread_started[i] = false;
    }
    thread_started[MAX_INPUT_COUNT] = false;

    struct option long_options[] = {
        { "help",            no_argument,        NULL, 'h' },
        { "url",             required_argument,  NULL, 'u' },
        { "input-url",       required_argument,  NULL, 'i' },
        { "profile",         required_argument,  NULL, 'p' },
        { "shared-secret",   required_argument,  NULL, 's' },
        { "encryption-type", required_argument,  NULL, 'e' },
        { "stats",           required_argument,  NULL, 'S' },
        { "verbose-level",   required_argument,  NULL, 'v' },
        { "buffer",          required_argument,  NULL, 'b' },
        { "npd",             no_argument,        NULL, 'n' },
        { 0, 0, 0, 0 },
    };

    while ((c = getopt_long(argc, argv, "hi:u:p:s:e:S:v:b:n", long_options, NULL)) != -1) {
        switch (c) {
        case 'i': inputurl = strdup(optarg); break;
        case 'u': outputurl = strdup(optarg); break;
        case 'p': profile = (enum rist_profile)atoi(optarg); break;
        case 's': shared_secret = strdup(optarg); break;
        case 'e': encryption_type = atoi(optarg); break;
        case 'S': statsinterval = atoi(optarg); break;
        case 'v': verbosity = atoi(optarg); break;
        case 'b': buffer_size = atoi(optarg); break;
        case 'n': npd = true; break;
        case 'h':
        default: usage(argv[0]);
        }
    }

    if (!inputurl || !outputurl) {
        usage(argv[0]);
    }

    struct rist_logging_settings *log_ptr = &logging_settings;
    if (rist_logging_set(&log_ptr, verbosity, NULL, NULL, NULL, stderr) != 0) {
        fprintf(stderr, "Failed to setup logging!\n");
        exit(1);
    }

    rist_log(&logging_settings, RIST_LOG_INFO,
             "VSF TR-06-4 Part 7 Sender - %s\n", RISTSENDER_VERSION);
    rist_log(&logging_settings, RIST_LOG_INFO,
             "libRIST: %s API: %s | Profile: %d\n", 
             librist_version(), librist_api_version(), profile);

    signal(SIGINT,  intHandler);
    signal(SIGTERM, intHandler);
    signal(SIGPIPE, intHandler);

    peer_args.loglevel = verbosity;
    peer_args.profile = profile;
    peer_args.encryption_type = encryption_type;
    peer_args.shared_secret = shared_secret;
    peer_args.buffer_size = buffer_size;
    peer_args.statsinterval = statsinterval;

    bool rist_listens = false;
    if (strstr(outputurl, "://@") != NULL) {
        rist_listens = true;
    }

    int32_t stream_id_check[MAX_INPUT_COUNT];
    for (size_t j = 0; j < MAX_INPUT_COUNT; j++)
        stream_id_check[j] = -1;
    struct evsocket_ctx *evctx = NULL;
    bool atleast_one_socket_opened = false;
    char *saveptrinput;
    char *inputtoken = strtok_r(inputurl, ",", &saveptrinput);
    struct rist_udp_config *udp_config = NULL;

    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        if (!inputtoken)
            break;

        if (rist_parse_udp_address2(inputtoken, &udp_config)) {
            rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse input %s\n", inputtoken);
            goto next;
        }

        bool found_empty = false;
        for (size_t j = 0; j < MAX_INPUT_COUNT; j++) {
            if (stream_id_check[j] == -1 && !found_empty) {
                stream_id_check[j] = (int32_t)udp_config->stream_id;
                found_empty = true;
            } else if ((uint16_t)stream_id_check[j] == udp_config->stream_id) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Duplicate stream-id %d\n", udp_config->stream_id);
                goto shutdown;
            }
        }

        if (rist_listens && i > 0) {
            callback_object[i].sender_ctx = callback_object[0].sender_ctx;
        } else {
            callback_object[i].sender_ctx = configure_rist_output_context(outputurl, &peer_args, udp_config, npd, profile);
            if (callback_object[i].sender_ctx == NULL)
                goto shutdown;
                
            // Save configuration for potential future use
            callback_object[i].saved_output_url = strdup(outputurl);
            memcpy(&callback_object[i].saved_peer_args, &peer_args, sizeof(peer_args));
            if (peer_args.shared_secret) {
                callback_object[i].saved_peer_args.shared_secret = strdup(peer_args.shared_secret);
            }
            callback_object[i].saved_npd = npd;
        }

        if (strcmp(udp_config->prefix, "rist") == 0) {
            struct rist_ctx_wrap *w = calloc(1, sizeof(*w));
            w->id = 0;
            callback_object[i].receiver_ctx = w;
            if (rist_receiver_create(&callback_object[i].receiver_ctx->ctx, peer_args.profile, &logging_settings) != 0) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Could not create receiver\n");
                goto next;
            }
            peer_args.token = inputtoken;
            struct rist_peer *peer = setup_rist_peer(callback_object[i].receiver_ctx, &peer_args);
            if (peer == NULL)
                atleast_one_socket_opened = true;
            rist_udp_config_free2(&udp_config);
            udp_config = NULL;
        } else {
            callback_object[i].block_buffer_capacity = MAX_BLOCK_TS_PACKETS * TS_PACKET_SIZE;
            callback_object[i].block_buffer = (uint8_t *)malloc(callback_object[i].block_buffer_capacity);
            if (!callback_object[i].block_buffer) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Failed to allocate buffer\n");
                goto shutdown;
            }

            if (!evctx)
                evctx = evsocket_create();

            char hostname[200] = {0};
            int inputlisten;
            uint16_t inputport;
            if (udpsocket_parse_url((void *)udp_config->address, hostname, 200, &inputport, &inputlisten) || !inputport || strlen(hostname) == 0) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse %s\n", inputtoken);
                goto next;
            }

            callback_object[i].sd = udpsocket_open_bind(hostname, inputport, udp_config->miface);
            if (callback_object[i].sd < 0) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Could not bind %s:%d\n", hostname, inputport);
                goto next;
            } else {
                uint32_t rcvbuf_before, rcvbuf_after;

                udpsocket_set_nonblocking(callback_object[i].sd);

                /* Size the receive buffer. udpsocket_open_bind() sets SO_REUSEADDR
                 * and binds and nothing else -- it does NOT call this -- so the
                 * socket was running at the kernel's net.core.rmem_default. That
                 * is the buffer that overflows and discards whole datagrams, and
                 * whole-datagram loss is the only thing that can make a block come
                 * up short by exactly 7 or 14 TS packets.
                 *
                 * Log both values: the "before" figure IS rmem_default on this
                 * box, which we have never been able to read directly, and the
                 * "after" tells us whether the kernel honoured the request or
                 * capped us at rmem_max. Both matter and neither needs a shell. */
                rcvbuf_before = udpsocket_get_buffer_size(callback_object[i].sd);
                if (udpsocket_set_optimal_buffer_size(callback_object[i].sd) < 0) {
                    rist_log(&logging_settings, RIST_LOG_WARN,
                             "Could not raise SO_RCVBUF; the kernel capped us. Raise "
                             "net.core.rmem_max if datagram drops persist.\n");
                }
                rcvbuf_after = udpsocket_get_buffer_size(callback_object[i].sd);

                rist_log(&logging_settings, RIST_LOG_INFO,
                         "Input bound: %s:%d  SO_RCVBUF %u -> %u bytes (~%u datagrams of %d)\n",
                         hostname, inputport,
                         (unsigned)rcvbuf_before, (unsigned)rcvbuf_after,
                         (unsigned)(rcvbuf_after / (2 * (TS_PACKETS_PER_RTP * TS_PACKET_SIZE))),
                         TS_PACKETS_PER_RTP * TS_PACKET_SIZE);
                atleast_one_socket_opened = true;
            }
            callback_object[i].udp_config = udp_config;
            udp_config = NULL;
            callback_object[i].evctx = evctx;
            event[i] = evsocket_addevent(callback_object[i].evctx, callback_object[i].sd, EVSOCKET_EV_READ,
                                         input_udp_recv, input_udp_sockerr, (void *)&callback_object[i]);
        }

next:
        inputtoken = strtok_r(NULL, ",", &saveptrinput);
    }

    if (!atleast_one_socket_opened) {
        goto shutdown;
    }

    if (evctx && pthread_create(&thread_main_loop[0], NULL, input_loop, (void *)callback_object) != 0)
    {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start thread\n");
        goto shutdown;
    }
    thread_started[0] = true;

    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        if (callback_object[i].receiver_ctx && rist_start(callback_object[i].receiver_ctx->ctx) == -1) {
            rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start receiver\n");
            goto shutdown;
        }
        if (callback_object[i].receiver_ctx && pthread_create(&thread_main_loop[i+1], NULL, input_loop, (void *)&callback_object[i]) != 0)
        {
            rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start thread\n");
            goto shutdown;
        } else if (callback_object[i].receiver_ctx) {
            thread_started[i+1] = true;
        }
    }

    rist_log(&logging_settings, RIST_LOG_INFO, "Sender started. Waiting for first marker...\n");

#ifndef _WIN32
    pause();
#endif

shutdown:
    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        if (callback_object[i].markers_processed > 0) {
            print_statistics(&callback_object[i]);
        }
    }

    if (udp_config) {
        rist_udp_config_free2(&udp_config);
    }
    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        if (event[i])
            evsocket_delevent(callback_object[i].evctx, event[i]);
        if (callback_object[i].block_buffer)
            free(callback_object[i].block_buffer);
        if ((void *)callback_object[i].udp_config)
            rist_udp_config_free2(&callback_object[i].udp_config);
        if (callback_object[i].receiver_ctx) {
            rist_destroy(callback_object[i].receiver_ctx->ctx);
            free(callback_object[i].receiver_ctx);
        }
        if (callback_object[i].sender_ctx) {
            rist_destroy(callback_object[i].sender_ctx->ctx);
            free(callback_object[i].sender_ctx);
        }
        if (callback_object[i].saved_output_url) {
            free(callback_object[i].saved_output_url);
        }
        if (callback_object[i].saved_peer_args.shared_secret) {
            free(callback_object[i].saved_peer_args.shared_secret);
        }
    }

    for (size_t i = 0; i <= MAX_INPUT_COUNT; i++) {
        if (thread_started[i])
            pthread_join(thread_main_loop[i], NULL);
    }

    rist_logging_unset_global();
    free(inputurl);
    free(outputurl);
    if (shared_secret) free(shared_secret);

    return 0;
}