/*
 * headend_part7_sender.c -- VSF TR-06-4 Part 7 marker SENDER (headend, x86)
 *
 * WHERE THIS RUNS: on the headend, before the uplink. It takes the RIST flow in,
 * inserts a Part 7 metadata marker every RTP_PAYLOADS_PER_MARKER payloads, and
 * emits 7-packet-aligned UDP towards the uplink mux. In Part 7 terms this is the
 * SENDER: it is the side that marks. The STB side, which validates markers, is
 * the Part 7 receiver.
 *
 * (The legacy name for this tool was "ristreceiver_with_markers", which read as
 * an STB-side receiver despite being a headend sender. That file is left in
 * place untouched; this is a new variant alongside it.)
 *
 * WHAT DIFFERS FROM THE LEGACY TOOL: non_null_count excludes PSI.
 *
 * The marker design assumes the counter sits last before the uplink and first
 * after the tuner, so both ends see byte-identical transport. We cannot take the
 * whole uplink -- it is live with subscribers -- so the marker is applied to a
 * single service inside a third-party multiplex. That breaks the assumption in
 * two places: PSI is regenerated (by their mux, and again by the STB's own
 * capture, which injects its own PAT+PMT), and null packets belong to the
 * transport rather than to the service, so the STB never captures them.
 *
 * Measured over 241 validation records, observed-minus-expected non_null was
 * exactly psi_stb - psi_headend: +2 when the STB injected its PAT and PMT (never
 * +1, because it injects both together), 0 when neither side had PSI in the
 * block, and -1/-2/-3 when the headend block carried PSI the STB could not see.
 * Excluding PSI from both sides collapses that distribution to zero.
 *
 * So here non_null counts ONLY the marker plus elementary-stream packets.
 * null_count is unchanged. THE OUTPUT STREAM IS UNCHANGED -- PSI packets are
 * still forwarded on the wire exactly as before; they are simply not counted.
 *
 * WIRE SEMANTICS CHANGE (intended): non_null + null no longer equals the block
 * size. PSI has left the sum, so the receiver can no longer derive the block
 * total from the marker and must use the structural constant
 * (RTP_PAYLOADS_PER_MARKER * TS_PACKETS_PER_RTP + 1). The receiver can recover
 * the PSI count as: psi = block_total - non_null - null.
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

// -------------------- Constants --------------------
#define TS_PACKET_SIZE             188
#define TS_PACKETS_PER_RTP         7
#define UDP_PAYLOAD_SIZE           (TS_PACKETS_PER_RTP * TS_PACKET_SIZE)  // 1316 bytes
#define RTP_PAYLOADS_PER_MARKER    5
#define MARKER_PID                 0x1FF0

// -------------------- Global state --------------------
static struct rist_ctx *ctx = NULL;
static struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;
static int running = 1;
static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
static int signal_received = 0;

// Output socket
static int output_fd = -1;
static struct sockaddr_in output_addr;

// Marker generation state
static uint32_t marker_sequence = 1;
static uint32_t rtp_packets_in_block = 0;
static uint16_t current_block_start_rtp_seq = 0;
static uint16_t current_non_null_count = 0;
static uint16_t current_null_count = 0;
static uint16_t current_psi_count = 0;   // excluded from non_null; logged for validation
static uint32_t current_ssrc = 0;
static uint8_t  marker_cc = 0;
static bool first_marker_sent = false;

/* Per-payload layout, one entry per RTP payload in the block.
 *
 * The block-wide counts say how many nulls and PSI packets a block held; they
 * do not say WHICH payload each one sat in, and that is the difference between
 * a repair that splices cleanly and one that glitches.
 *
 * The STB rebuilds these payloads from a PID-filtered capture that never sees a
 * null. Given totals alone it can only pack every ES packet first and pad at the
 * end, so from the first null onward its payload N holds different ES packets
 * than the copy in the sender queue -- the copy a NACK or an FSR switch hands to
 * the receiver. Substituting one for the other then repeats some ES packets and
 * drops others.
 *
 * Order WITHIN a payload does not matter: the decoder discards PID 0x1FFF before
 * anything else looks at the stream, so seven slots carrying the same ES packets
 * in the same order are equivalent however the nulls are interleaved. Only the
 * per-payload ES count has to agree, and that is what these give the STB. */
static uint8_t payload_null_count[RTP_PAYLOADS_PER_MARKER];
static uint8_t payload_psi_count[RTP_PAYLOADS_PER_MARKER];

// CRITICAL: UDP payload accumulator to maintain 7-packet alignment
static uint8_t udp_buffer[UDP_PAYLOAD_SIZE];
static size_t udp_buffer_fill = 0;

// CRC-32 table (ISO/IEC 13818-1)
static uint32_t crc32_table[256];
static int crc32_table_initialized = 0;

// -------------------- Signal handling --------------------
static void signal_handler(int sig) {
    pthread_mutex_lock(&signal_lock);
    signal_received = sig;
    running = 0;
    pthread_mutex_unlock(&signal_lock);
}

// -------------------- CRC utilities --------------------
static void init_crc32_table(void) {
    if (crc32_table_initialized) return;
    const uint32_t poly = 0x04C11DB7;
    for (int i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i << 24;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80000000) ? ((crc << 1) ^ poly) : (crc << 1);
        crc32_table[i] = crc;
    }
    crc32_table_initialized = 1;
}

static uint32_t calculate_crc32(const uint8_t *data, int length) {
    if (!crc32_table_initialized) init_crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < length; i++) {
        uint8_t idx = (uint8_t)(((crc >> 24) ^ data[i]) & 0xFF);
        crc = (crc << 8) ^ crc32_table[idx];
    }
    return crc;
}

// -------------------- TS helpers --------------------
static int is_null_packet(const uint8_t *ts_packet) {
    if (ts_packet[0] != 0x47) return 0;
    uint16_t pid = ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
    return (pid == 0x1FFF);
}

// -------------------- PSI classification --------------------
// PSI/SI to exclude from non_null:
//   * PIDs 0x0000-0x001F: the DVB-reserved range (PAT, CAT, NIT, SDT/BAT, EIT,
//     RST, TDT/TOT). This is exactly what a third-party mux regenerates, so
//     excluding the whole range rather than just PAT keeps the count stable no
//     matter which tables their mux decides to insert into the service.
//   * the PMT PID(s): service-specific and normally well outside 0x1F, so they
//     have to be identified separately. Discovered by parsing the PAT rather
//     than configured, so the tool follows a remap without being told.
// Extra PIDs can be forced with -x <pid> when a stream carries private tables
// that should not count as elementary content.
#define MAX_PMT_PIDS 16
static uint16_t psi_pmt_pids[MAX_PMT_PIDS];
static int      psi_pmt_pid_count = 0;
static uint16_t psi_extra_pids[MAX_PMT_PIDS];
static int      psi_extra_pid_count = 0;
static uint32_t psi_pat_seen = 0;

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
    printf("[PSI] learned PMT pid 0x%04X from PAT (%d known)\n", pid, psi_pmt_pid_count);
}

// Parse a PAT packet and learn every program_map_PID it declares. Only handles a
// PAT whose section starts in this packet, which covers the single-section PAT
// every practical service carries; anything malformed is ignored rather than
// guessed at.
static void psi_learn_from_pat(const uint8_t *ts_packet) {
    uint8_t afc = (uint8_t)((ts_packet[3] >> 4) & 0x03);
    int off = 4;

    if ((ts_packet[1] & 0x40) == 0) return;          // need payload_unit_start
    if ((afc & 0x01) == 0) return;                   // no payload
    if (afc & 0x02) {                                // skip adaptation field
        int af_len = ts_packet[4];
        off = 5 + af_len;
        if (off >= TS_PACKET_SIZE) return;
    }

    int pointer = ts_packet[off];                    // pointer_field
    off += 1 + pointer;
    if (off + 8 > TS_PACKET_SIZE) return;

    if (ts_packet[off] != 0x00) return;              // table_id must be PAT

    int section_length = ((ts_packet[off + 1] & 0x0F) << 8) | ts_packet[off + 2];
    int section_end    = off + 3 + section_length;   // includes the 4-byte CRC
    if (section_length < 9 || section_end > TS_PACKET_SIZE) return;

    for (int p = off + 8; p + 4 <= section_end - 4; p += 4) {
        uint16_t prog_num = (uint16_t)((ts_packet[p] << 8) | ts_packet[p + 1]);
        uint16_t map_pid  = (uint16_t)(((ts_packet[p + 2] & 0x1F) << 8) | ts_packet[p + 3]);
        if (prog_num == 0) continue;                 // network_PID (NIT), already <= 0x1F
        psi_pmt_pid_add(map_pid);
    }
    psi_pat_seen++;
}

// Is this packet PSI/SI, i.e. excluded from non_null?
static int is_psi_packet(const uint8_t *ts_packet) {
    uint16_t pid;

    if (ts_packet[0] != 0x47) return 0;
    pid = ts_pid(ts_packet);

    if (pid <= 0x001F) {                             // DVB-reserved range
        if (pid == 0x0000) psi_learn_from_pat(ts_packet);
        return 1;
    }
    if (psi_pmt_pid_known(pid)) return 1;

    for (int i = 0; i < psi_extra_pid_count; i++)
        if (psi_extra_pids[i] == pid) return 1;

    return 0;
}

// -------------------- UDP buffer management --------------------
// Flush the UDP buffer when it's full (7 TS packets)
static void flush_udp_buffer(void) {
    if (udp_buffer_fill == 0) return;
    
    if (udp_buffer_fill != UDP_PAYLOAD_SIZE) {
        fprintf(stderr, "[WARNING] Flushing partial UDP buffer: %zu bytes (expected %d)\n",
                udp_buffer_fill, UDP_PAYLOAD_SIZE);
    }
    
    ssize_t sent = sendto(output_fd, udp_buffer, udp_buffer_fill, 0,
                          (struct sockaddr*)&output_addr, sizeof(output_addr));
    if (sent != (ssize_t)udp_buffer_fill) {
        fprintf(stderr, "[UDP] Send error: %s\n", strerror(errno));
    }
    
    udp_buffer_fill = 0;
}

// Add a TS packet to the UDP buffer, flushing when full
static void add_ts_to_udp_buffer(const uint8_t *ts_packet) {
    if (udp_buffer_fill + TS_PACKET_SIZE > UDP_PAYLOAD_SIZE) {
        flush_udp_buffer();
    }
    
    memcpy(&udp_buffer[udp_buffer_fill], ts_packet, TS_PACKET_SIZE);
    udp_buffer_fill += TS_PACKET_SIZE;
    
    if (udp_buffer_fill == UDP_PAYLOAD_SIZE) {
        flush_udp_buffer();
    }
}

// -------------------- Marker builder --------------------
static void generate_metadata_marker(uint8_t *marker_packet) {
    memset(marker_packet, 0xFF, TS_PACKET_SIZE);

    // TS header for PID 0x1FF0, payload only, PUSI=1
    marker_packet[0] = 0x47;
    marker_packet[1] = 0x40 | ((MARKER_PID >> 8) & 0x1F);
    marker_packet[2] = (uint8_t)(MARKER_PID & 0xFF);
    marker_packet[3] = 0x10 | (marker_cc & 0x0F);
    marker_cc = (uint8_t)((marker_cc + 1) & 0x0F);

    marker_packet[4] = 0x00;  // pointer_field

    uint8_t *sec = &marker_packet[5];
    int off = 0;

    sec[off++] = 0xBF;  // table_id

    /* 24 was the layout-free marker; +RTP_PAYLOADS_PER_MARKER bytes of
     * per-payload layout takes it to 29. The STB switches on this length, so an
     * STB that predates the layout sees a length it does not recognise and
     * rejects the marker rather than misreading it -- deploy the two together. */
    const uint16_t section_length = 24 + RTP_PAYLOADS_PER_MARKER;
    sec[off++] = (uint8_t)(0x30 | ((section_length >> 8) & 0x0F));
    sec[off++] = (uint8_t)(section_length & 0xFF);

    // marker_sequence_number (32)
    sec[off++] = (uint8_t)((marker_sequence >> 24) & 0xFF);
    sec[off++] = (uint8_t)((marker_sequence >> 16) & 0xFF);
    sec[off++] = (uint8_t)((marker_sequence >>  8) & 0xFF);
    sec[off++] = (uint8_t)( marker_sequence        & 0xFF);

    // non_null_count (16)
    sec[off++] = (uint8_t)((current_non_null_count >> 8) & 0xFF);
    sec[off++] = (uint8_t)( current_non_null_count       & 0xFF);

    // null_count (16)
    sec[off++] = (uint8_t)((current_null_count >> 8) & 0xFF);
    sec[off++] = (uint8_t)( current_null_count       & 0xFF);

    // rtp_sequence_start_msb (16)
    sec[off++] = 0x00; sec[off++] = 0x00;

    // rtp_sequence_start_lsb (16)
    sec[off++] = (uint8_t)((current_block_start_rtp_seq >> 8) & 0xFF);
    sec[off++] = (uint8_t)( current_block_start_rtp_seq       & 0xFF);

    // rtp_sequence_next_msb (16)
    sec[off++] = 0x00; sec[off++] = 0x00;

    // rtp_sequence_next_lsb (16)
    uint16_t next_lsb = (uint16_t)(current_block_start_rtp_seq + RTP_PAYLOADS_PER_MARKER);
    sec[off++] = (uint8_t)((next_lsb >> 8) & 0xFF);
    sec[off++] = (uint8_t)( next_lsb       & 0xFF);

    // source_ssrc (32)
    sec[off++] = (uint8_t)((current_ssrc >> 24) & 0xFF);
    sec[off++] = (uint8_t)((current_ssrc >> 16) & 0xFF);
    sec[off++] = (uint8_t)((current_ssrc >>  8) & 0xFF);
    sec[off++] = (uint8_t)( current_ssrc        & 0xFF);

    /* payload_layout[RTP_PAYLOADS_PER_MARKER]: high nibble = PSI packets in that
     * payload, low nibble = null packets. ES count is the remainder of the seven
     * slots, so the STB fills payload k with (7 - psi - null) captured packets
     * and tops it up -- which is what keeps its payload k carrying the same ES
     * packets as the copy in the sender queue. A nibble each rather than three
     * packed bits: both max out at 7, and this stays readable in a hex dump. */
    for (int p = 0; p < RTP_PAYLOADS_PER_MARKER; p++) {
        sec[off++] = (uint8_t)(((payload_psi_count[p] & 0x0F) << 4) |
                                (payload_null_count[p] & 0x0F));
    }

    // CRC
    uint32_t crc = calculate_crc32(sec, off);
    sec[off++] = (uint8_t)((crc >> 24) & 0xFF);
    sec[off++] = (uint8_t)((crc >> 16) & 0xFF);
    sec[off++] = (uint8_t)((crc >>  8) & 0xFF);
    sec[off++] = (uint8_t)( crc        & 0xFF);

    // block_total is structural (marker + RTP_PAYLOADS_PER_MARKER*TS_PACKETS_PER_RTP),
    // NOT derived from the counts -- non_null + null no longer sums to it.
    printf("[MARKER] seq=%u non_null=%u null=%u psi=%u total=%u start=%u next=%u ssrc=0x%08X"
           " (non_null = marker + ES only; PSI excluded)\n",
           marker_sequence, current_non_null_count, current_null_count,
           current_psi_count,
           (unsigned)(current_non_null_count + current_null_count + current_psi_count),
           current_block_start_rtp_seq, next_lsb, current_ssrc);

    printf("[MARKER] layout es/psi/null per payload:");
    for (int p = 0; p < RTP_PAYLOADS_PER_MARKER; p++) {
        printf(" %d/%u/%u",
               TS_PACKETS_PER_RTP - payload_psi_count[p] - payload_null_count[p],
               payload_psi_count[p], payload_null_count[p]);
    }
    printf("\n");
}

// -------------------- RIST data callback --------------------
static int cb_recv(void *arg, struct rist_data_block *b) {
    (void)arg;
    if (!b || !b->payload || b->payload_len <= 0) return -1;

    uint16_t rtp_sequence = (uint16_t)b->seq;
    uint32_t ssrc = b->flow_id;
    current_ssrc = ssrc;

    if (rtp_packets_in_block == 0) {
        current_block_start_rtp_seq = rtp_sequence;
        
        // CRITICAL: If this is NOT the first marker ever, we need to count
        // the previous marker that starts this block
        if (first_marker_sent) {
            current_non_null_count = 1;  // The marker at the start of this block
            current_null_count = 0;
            current_psi_count = 0;
        } else {
            // Very first block - no marker yet
            current_non_null_count = 0;
            current_null_count = 0;
            current_psi_count = 0;
        }
    }

    // Count null/non-null TS packets in this RTP payload
    const uint8_t *ts_data = (const uint8_t *)b->payload;
    int ts_packets = b->payload_len / TS_PACKET_SIZE;
    /* Counted per payload as well as per block. This callback is already
     * invoked once per RTP payload, so the split costs nothing beyond two
     * locals -- the information was being accumulated away. */
    uint8_t this_payload_nulls = 0;
    uint8_t this_payload_psi = 0;

    for (int i = 0; i < ts_packets; i++) {
        const uint8_t *ts = ts_data + i * TS_PACKET_SIZE;
        if (ts[0] != 0x47) continue;
        if (is_null_packet(ts)) {
            current_null_count++;
            this_payload_nulls++;
        } else if (is_psi_packet(ts)) {
            // PSI is regenerated downstream (their mux) and re-injected upstream
            // (the STB capture), so it can never agree between the two ends.
            // Counted separately and left OUT of non_null; still forwarded below.
            current_psi_count++;
            this_payload_psi++;
        } else {
            current_non_null_count++;   // marker + elementary streams only
        }
    }

    if (rtp_packets_in_block < RTP_PAYLOADS_PER_MARKER) {
        payload_null_count[rtp_packets_in_block] = this_payload_nulls;
        payload_psi_count[rtp_packets_in_block]  = this_payload_psi;
    }

    // Add each TS packet to UDP buffer to maintain 7-packet alignment
    for (int i = 0; i < ts_packets; i++) {
        const uint8_t *ts = ts_data + i * TS_PACKET_SIZE;
        add_ts_to_udp_buffer(ts);
    }

    rtp_packets_in_block++;

    // Insert marker after RTP_PAYLOADS_PER_MARKER RTP packets
    if (rtp_packets_in_block >= RTP_PAYLOADS_PER_MARKER) {
        uint8_t marker_packet[TS_PACKET_SIZE];
        generate_metadata_marker(marker_packet);

        // Add marker to UDP buffer (maintains alignment)
        add_ts_to_udp_buffer(marker_packet);

        // Mark that we've sent at least one marker
        first_marker_sent = true;

        // Prepare for next block
        marker_sequence++;
        rtp_packets_in_block = 0;
        
        // NOTE: We DON'T reset counts here!
        // The next block will reset counts when rtp_packets_in_block == 0
        // and will start at 1 to count the marker we just inserted
        
        current_block_start_rtp_seq = (uint16_t)(current_block_start_rtp_seq + RTP_PAYLOADS_PER_MARKER);
    }

    return 0;
}

// -------------------- Output socket --------------------
static int setup_output_socket(const char *ip, int port) {
    output_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (output_fd < 0) { perror("socket"); return -1; }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        fprintf(stderr, "Invalid output IP: %s\n", ip);
        close(output_fd); output_fd = -1; return -1;
    }
    if (IN_MULTICAST(ntohl(addr.s_addr))) {
        unsigned char ttl = 1;
        if (setsockopt(output_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
            perror("setsockopt IP_MULTICAST_TTL");
        }
        printf("Configured multicast output\n");
    }

    memset(&output_addr, 0, sizeof(output_addr));
    output_addr.sin_family = AF_INET;
    output_addr.sin_port   = htons(port);
    output_addr.sin_addr   = addr;

    printf("Output UDP: %s:%d (aligned to %d-byte packets)\n", ip, port, UDP_PAYLOAD_SIZE);
    return 0;
}

// -------------------- Usage --------------------
static void usage(const char *prog) {
    printf("Usage: %s -i <rist_input_url> -o <udp_output_url> [-v <level>] [-x <psi_pid>]\n", prog);
    printf("Example: %s -i rist://192.168.110.107:5554 -o udp://239.6.6.6:6000\n", prog);
    printf("\nVSF TR-06-4 Part 7 marker SENDER (headend, runs before the uplink)\n");
    printf("- Extracts RTP metadata from RIST data blocks\n");
    printf("- Inserts metadata markers every %d RTP payloads\n", RTP_PAYLOADS_PER_MARKER);
    printf("- Maintains %d-byte UDP packet alignment (%d TS packets)\n",
           UDP_PAYLOAD_SIZE, TS_PACKETS_PER_RTP);
    printf("\nnon_null counts the marker plus ELEMENTARY STREAMS only. PSI is\n");
    printf("excluded: PIDs 0x0000-0x001F plus any PMT PID learned from the PAT,\n");
    printf("plus any -x pid. PSI is still forwarded on the wire, just not counted.\n");
    printf("Consequence: non_null + null no longer equals the block total\n");
    printf("(%d = marker + %d x %d). The receiver derives psi = total - non_null - null.\n",
           RTP_PAYLOADS_PER_MARKER * TS_PACKETS_PER_RTP + 1,
           RTP_PAYLOADS_PER_MARKER, TS_PACKETS_PER_RTP);
    printf("  -x <pid>  force an extra PID to count as PSI (repeatable)\n");
}

// -------------------- main --------------------
int main(int argc, char *argv[]) {
    char *inputurl = NULL;
    char *outputurl = NULL;
    int c;

    init_crc32_table();

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, signal_handler);

    while ((c = getopt(argc, argv, "i:o:v:x:h")) != -1) {
        switch (c) {
            case 'i': inputurl  = strdup(optarg); break;
            case 'o': outputurl = strdup(optarg); break;
            case 'x': {
                // Force an extra PID to count as PSI. PAT/PMT discovery covers
                // the normal case; this is the escape hatch for a stream that
                // carries private tables which must not count as ES.
                long v = strtol(optarg, NULL, 0);
                if (v > 0 && v < 0x1FFF && psi_extra_pid_count < MAX_PMT_PIDS) {
                    psi_extra_pids[psi_extra_pid_count++] = (uint16_t)v;
                    printf("[PSI] forcing pid 0x%04X to count as PSI\n", (unsigned)v);
                } else {
                    fprintf(stderr, "ERROR: bad -x pid: %s\n", optarg);
                    return 1;
                }
                break;
            }
            case 'v': {
                int lvl = atoi(optarg);
#ifdef RIST_LOG_EMERG
                if (lvl < RIST_LOG_EMERG) lvl = RIST_LOG_EMERG;
#else
                if (lvl < RIST_LOG_ERROR) lvl = RIST_LOG_ERROR;
#endif
                if (lvl > RIST_LOG_DEBUG) lvl = RIST_LOG_DEBUG;
                logging_settings.log_level = (enum rist_log_level)lvl;
                break;
            }
            case 'h':
            default: usage(argv[0]); return 1;
        }
    }

    if (!inputurl || !outputurl) {
        usage(argv[0]); return 1;
    }

    char output_ip[256] = {0};
    int output_port = 0;
    if (sscanf(outputurl, "udp://%255[^:]:%d", output_ip, &output_port) != 2) {
        fprintf(stderr, "ERROR: Could not parse output URL: %s\n", outputurl);
        return 1;
    }

    struct rist_logging_settings *log_ptr = &logging_settings;
    if (rist_logging_set(&log_ptr, logging_settings.log_level, NULL, NULL, NULL, stderr) != 0) {
        fprintf(stderr, "Failed to setup logging!\n");
        return 1;
    }

    printf("VSF TR-06-4 Part 7 RIST Receiver with Marker Multiplexer\n");
    printf("libRIST version: %s  API version: %s\n", librist_version(), librist_api_version());

    if (setup_output_socket(output_ip, output_port) != 0) return 1;

    if (rist_receiver_create(&ctx, RIST_PROFILE_MAIN, &logging_settings) != 0) {
        fprintf(stderr, "ERROR: Could not create RIST receiver context\n");
        return 1;
    }

    struct rist_peer_config *peer_config = NULL;
    if (rist_parse_address2(inputurl, &peer_config) != 0) {
        fprintf(stderr, "ERROR: Could not parse peer options\n");
        rist_destroy(ctx);
        return 1;
    }

    struct rist_peer *peer = NULL;
    if (rist_peer_create(ctx, &peer, peer_config) != 0) {
        fprintf(stderr, "ERROR: Could not add peer connector\n");
        rist_peer_config_free2(&peer_config);
        rist_destroy(ctx);
        return 1;
    }
    rist_peer_config_free2(&peer_config);

    if (rist_start(ctx) != 0) {
        fprintf(stderr, "ERROR: Could not start RIST receiver\n");
        rist_destroy(ctx);
        return 1;
    }

    printf("RIST receiver started successfully\n");
    printf("Input : %s\n", inputurl);
    printf("Output: %s (7-packet aligned UDP)\n", outputurl);
    printf("Marker insertion: Every %d RTP payloads\n\n", RTP_PAYLOADS_PER_MARKER);

    while (running) {
        struct rist_data_block *b = NULL;
        int queue_size = rist_receiver_data_read2(ctx, &b, 5);

        if (queue_size > 0) {
            if (queue_size % 10 == 0 || queue_size > 50) {
                uint32_t flow_id = b ? b->flow_id : 0;
                fprintf(stderr, "WARNING: Falling behind: count %d, flow id %u\n",
                        queue_size, flow_id);
            }
            if (b && b->payload && b->payload_len > 0) {
                cb_recv(NULL, b);
                rist_receiver_data_block_free2(&b);
            }
        }

        pthread_mutex_lock(&signal_lock);
        if (signal_received) {
            printf("\nSignal %d received\n", signal_received);
            pthread_mutex_unlock(&signal_lock);
            break;
        }
        pthread_mutex_unlock(&signal_lock);
    }

    // Flush any remaining data in UDP buffer
    flush_udp_buffer();

    printf("\nShutting down\n");
    printf("Final marker sequence: %u\n", (marker_sequence > 0) ? (marker_sequence - 1) : 0);

    if (ctx) rist_destroy(ctx);
    if (output_fd >= 0) close(output_fd);
    free(inputurl);
    free(outputurl);

    return 0;
}