/*
 * stb_part8_receiver.c -- VSF TR-06-4 Part 8 box-side sender (STB, ARM)
 *
 * WHERE THIS RUNS: on the set-top box. It takes the box's dmx2 capture as UDP
 * on loopback, cuts it into RTP payloads at PCR boundaries, and emits RIST to
 * the box's own local ristreceiver.
 *
 * SEPARATE BINARY FROM stb_part7_receiver ON PURPOSE. The Part 7 tool is 1800
 * lines of marker validation, block reconstruction and a circuit breaker, and
 * it is running in the field. Part 8 needs none of that and must not be able to
 * disturb it, so this is a new binary rather than a mode.
 *
 * THE CUT IS NOT IMPLEMENTED HERE. librist/src/pcr_cut.c is compiled into this
 * binary -- the SAME FILE the headend's sender uses -- because the whole point
 * of PCR-boundary framing is that both ends split identical bytes identically.
 * A second implementation, however careful, is a second thing to keep in step.
 * The PID arrives as ?pcr_cut=<pid> on the input URL and is parsed by librist's
 * own rist_parse_udp_address2(), exactly as ristsender does it.
 *
 * WHAT THIS IS NOT, IN STEP 1: there is no recovery peer, no NACK, no FSR, no
 * error detection and no sync-to-server. One local peer, one direction. The
 * question this answers is only "does the box's own cut-and-reassemble chain
 * produce a decodable stream on the box's own screen".
 *
 * SHUTDOWN IS BOUNDED, and that is not decoration. A zap restarts this process;
 * one that will not die holds the capture port and the next start binds nothing
 * and shows a black screen. Same three-part guard as the headend's recovery
 * server: sigaction without SA_RESTART, a second signal exits immediately, and
 * the first signal arms an alarm whose handler _exit()s whatever cleanup is
 * doing.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <librist/librist.h>
#include <librist/udpsocket.h>

#include "pcr_cut.h"

/* SO_RCVBUF request. Was 1MB, sized for the per-service Part 8 capture. The box
 * now feeds this the WHOLE TRANSPONDER -- 59 Mb/s measured, 7.4 MB/s -- at which
 * 1MB is ~135ms of headroom and one scheduling hiccup in this process silently
 * costs datagrams on loopback. 4MB is ~540ms and matches the sender's
 * SO_SNDBUF. The kernel clamps to net.core.rmem_max, so what matters is the
 * GRANTED value logged below, not this number. */
#define P8_RECV_BUF          (4 * 1024 * 1024)
/* One UDP datagram. RIST_MAX_PACKET_SIZE is private to librist, and the box's
 * reader emits 1316-byte chunks anyway; this is just a ceiling. */
#define P8_READ_MAX          65536
#define P8_SHUTDOWN_DEADLINE 5               /* seconds; see on_signal() */
#define P8_LOG_EVERY_US      5000000ULL
#define P8_IDLE_DEFAULT_S    0               /* 0 = never exit on silence */

static struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;

static volatile int          running   = 1;
static volatile sig_atomic_t signalled = 0;

/* Counters. Diagnostic only; nothing here steers behaviour. */
static uint64_t g_bytes_in;
static uint64_t g_datagrams;
static uint64_t g_writes_failed;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/*
 * Shutdown, bounded three ways -- the same guard the headend's recovery server
 * carries, for the same reason and after the same surprise there.
 *
 *   1. sigaction with NO SA_RESTART, so a blocking call returns EINTR instead
 *      of resuming. signal() on glibc/uClibc is BSD semantics, i.e. SA_RESTART,
 *      which is the opposite of what a shutdown path wants.
 *   2. A second signal exits immediately.
 *   3. The first signal arms alarm(); its handler _exit()s. Wherever cleanup
 *      blocks -- and rist_destroy() with a peer attached is the candidate -- the
 *      process is gone within the deadline and the kernel releases the capture
 *      port. That release is the property that matters: an orphan holding
 *      udp/6300 makes the next zap bind nothing and show a black screen.
 */
static void on_signal(int sig)
{
	(void)sig;
	if (signalled)
		_exit(1);
	signalled = 1;
	running = 0;
	alarm(P8_SHUTDOWN_DEADLINE);
}

static void on_alarm(int sig)
{
	(void)sig;
	_exit(0);
}

static void install_handler(int sig, void (*fn)(int))
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fn;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;                 /* deliberately NOT SA_RESTART */
	sigaction(sig, &sa, NULL);
}

static void usage(const char *me)
{
	fprintf(stderr,
		"VSF TR-06-4 Part 8 box sender (Step 1: video path, no recovery)\n\n"
		"Usage: %s -i udp://@127.0.0.1:PORT[?pcr_cut=PID] -u rist://@127.0.0.1:PORT[?buffer=MS]\n"
		"  -i URL   capture input. ?pcr_cut=<pid> turns on PCR-boundary framing;\n"
		"           without it this is one datagram in, one payload out.\n"
		"  -u URL   local RIST output peer\n"
		"  -t SECS  exit if no input arrives for SECS (0 = never, default %d)\n"
		"  -v       verbose\n", me, P8_IDLE_DEFAULT_S);
}

int main(int argc, char *argv[])
{
	char *inurl = NULL, *outurl = NULL;
	int   idle_s = P8_IDLE_DEFAULT_S;
	int   c, ret = 1;

	struct rist_udp_config *udp_config = NULL;
	struct rist_peer_config *peer_config = NULL;
	struct rist_ctx  *sender_ctx = NULL;
	struct rist_peer *peer = NULL;
	struct rist_pcr_cut cut;
	bool cutting = false;
	int  sd = -1;

	while ((c = getopt(argc, argv, "i:u:t:vh")) != -1) {
		switch (c) {
		case 'i': inurl  = strdup(optarg); break;
		case 'u': outurl = strdup(optarg); break;
		case 't': idle_s = atoi(optarg);   break;
		case 'v': logging_settings.log_level = RIST_LOG_DEBUG; break;
		default:  usage(argv[0]); return 1;
		}
	}
	if (!inurl || !outurl) { usage(argv[0]); return 1; }

	/* LOGGING_SETTINGS_INITIALIZER defaults to RIST_LOG_DISABLE, so an
	 * unconfigured tool is silent -- including the line that says whether the
	 * cutter is on, which is the single most useful thing in the log. */
	if (logging_settings.log_level == RIST_LOG_DISABLE)
		logging_settings.log_level = RIST_LOG_INFO;
	{
		struct rist_logging_settings *lp = &logging_settings;
		rist_logging_set(&lp, logging_settings.log_level, NULL, NULL, NULL, stderr);
	}

	install_handler(SIGINT,  on_signal);
	install_handler(SIGTERM, on_signal);
	install_handler(SIGALRM, on_alarm);
	signal(SIGPIPE, SIG_IGN);

	/* librist parses the input URL, INCLUDING ?pcr_cut=. Parsing it here by
	 * hand would be a second parser to keep in step with the headend's. */
	if (rist_parse_udp_address2(inurl, &udp_config) != 0 || !udp_config) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "cannot parse input URL: %s\n", inurl);
		goto out;
	}

	{
		char host[256] = {0};
		uint16_t port = 0;
		int listening = 0;

		if (udpsocket_parse_url((void *)udp_config->address, host, sizeof(host),
		                        &port, &listening) || !port) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "cannot parse %s\n", udp_config->address);
			goto out;
		}
		sd = udpsocket_open_bind(host, port, udp_config->miface);
		if (sd < 0) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "cannot bind %s:%u: %s\n",
				host, (unsigned)port, strerror(errno));
			goto out;
		}
		udpsocket_set_nonblocking(sd);
		{
			int want = P8_RECV_BUF, got = 0;
			socklen_t glen = sizeof(got);
			/* Report what the kernel GRANTED, not what we asked for. It clamps
			 * silently to net.core.rmem_max and this box has bitten us there
			 * before: the default held ~33 datagrams against 36-datagram bursts
			 * and the loss looked like a satellite problem for days. */
			if (setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want)) < 0)
				rist_log(&logging_settings, RIST_LOG_WARN, "SO_RCVBUF: %s\n", strerror(errno));
			getsockopt(sd, SOL_SOCKET, SO_RCVBUF, &got, &glen);
			rist_log(&logging_settings, RIST_LOG_INFO,
				"[IN] bound %s:%u  rcvbuf asked %d got %d (%d ms at 59 Mb/s)\n",
				host, (unsigned)port, want, got, got / 7400);
			if (got < 2 * 1024 * 1024)
				rist_log(&logging_settings, RIST_LOG_WARN,
					"[IN] rcvbuf %d is under 2MB -- net.core.rmem_max is clamping us. "
					"At whole-TP rates this hop will shed under load.\n", got);
		}
	}

	if (rist_sender_create(&sender_ctx, RIST_PROFILE_MAIN, 0, &logging_settings) != 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "could not create sender\n");
		goto out;
	}
	if (rist_parse_address2(outurl, &peer_config) != 0 || !peer_config) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "cannot parse output URL: %s\n", outurl);
		goto out;
	}
	if (rist_peer_create(sender_ctx, &peer, peer_config) != 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "could not create peer for %s\n", outurl);
		goto out;
	}
	if (rist_start(sender_ctx) != 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "could not start sender\n");
		goto out;
	}

	/* THE line to look for on the serial console. If the framing is off here
	 * and on at the headend, or on with the wrong PID, everything downstream
	 * looks fine and nothing lines up -- so say it once, plainly, at start. */
	if (udp_config->pcr_cut) {
		rist_pcr_cut_init(&cut, udp_config->pcr_cut);
		cutting = true;
		rist_log(&logging_settings, RIST_LOG_INFO,
			"[START] PCR-boundary framing ON, pcr_pid=0x%04X (%u)\n",
			udp_config->pcr_cut, udp_config->pcr_cut);
	} else {
		rist_log(&logging_settings, RIST_LOG_INFO,
			"[START] no pcr_cut on the input URL -- one datagram in, one payload out\n");
	}
	rist_log(&logging_settings, RIST_LOG_INFO, "[START] in=%s out=%s\n", inurl, outurl);

	{
		uint8_t  buf[P8_READ_MAX];
		uint64_t next_log = now_us() + P8_LOG_EVERY_US;
		uint64_t last_rx  = now_us();

		while (running) {
			struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
			fd_set rf;
			FD_ZERO(&rf);
			FD_SET(sd, &rf);

			if (select(sd + 1, &rf, NULL, NULL, &tv) > 0 && FD_ISSET(sd, &rf)) {
				struct sockaddr_storage ss;
				socklen_t sl = sizeof(ss);
				ssize_t n = udpsocket_recvfrom(sd, buf, sizeof(buf), MSG_DONTWAIT,
				                               (struct sockaddr *)&ss, &sl);
				if (n > 0) {
					g_bytes_in += (uint64_t)n;
					g_datagrams++;
					last_rx = now_us();

					if (cutting) {
						/* The cutter emits zero or more payloads and calls
						 * rist_sender_data_write() itself, once per payload --
						 * which is what makes the sequence advance per payload
						 * rather than per datagram. */
						if (rist_pcr_cut_feed(&cut, buf, (size_t)n, sender_ctx) < 0)
							g_writes_failed++;
					} else {
						struct rist_data_block db;
						memset(&db, 0, sizeof(db));
						db.payload     = buf;
						db.payload_len = (size_t)n;
						if (rist_sender_data_write(sender_ctx, &db) < 0)
							g_writes_failed++;
					}
				}
			}

			if (now_us() > next_log) {
				next_log = now_us() + P8_LOG_EVERY_US;
				if (cutting)
					rist_log(&logging_settings, RIST_LOG_INFO,
						"[RUN] dgrams=%" PRIu64 " bytes=%" PRIu64 " ts_pkts=%" PRIu64
						" pcrs=%" PRIu64 " pre_pcr_dropped=%" PRIu64
						" resyncs=%" PRIu64 " badsync=%" PRIu64 " write_err=%" PRIu64 "\n",
						g_datagrams, g_bytes_in, cut.pkts_in, cut.pcr_count,
						cut.dropped_pre_pcr, cut.resyncs, cut.bad_sync, g_writes_failed);
				else
					rist_log(&logging_settings, RIST_LOG_INFO,
						"[RUN] dgrams=%" PRIu64 " bytes=%" PRIu64 " write_err=%" PRIu64 "\n",
						g_datagrams, g_bytes_in, g_writes_failed);
			}

			/* Optional silence timeout. Off by default: on the box the capture
			 * legitimately pauses across a tuner relock, and exiting there would
			 * turn a blip into a restart. It exists for bench runs where a
			 * process left behind holding the port is the worse failure. */
			if (idle_s > 0 && (now_us() - last_rx) > (uint64_t)idle_s * 1000000ULL) {
				rist_log(&logging_settings, RIST_LOG_WARN,
					"[STOP] no input for %ds -- exiting so the port is released\n", idle_s);
				break;
			}
		}
	}

	ret = 0;

out:
	/* Staged and logged before each stage is entered, so if this ever hangs on
	 * hardware the last line printed names the call that blocked. */
	rist_log(&logging_settings, RIST_LOG_INFO,
		"[STOP] shutting down (hard deadline %ds)\n", P8_SHUTDOWN_DEADLINE);
	if (sd >= 0) {
		rist_log(&logging_settings, RIST_LOG_INFO, "[STOP] closing input\n");
		close(sd);
	}
	if (sender_ctx) {
		rist_log(&logging_settings, RIST_LOG_INFO, "[STOP] rist_destroy\n");
		rist_destroy(sender_ctx);
	}
	if (peer_config) rist_peer_config_free2(&peer_config);
	if (udp_config)  rist_udp_config_free2(&udp_config);
	free(inurl);
	free(outurl);
	alarm(0);
	rist_log(&logging_settings, RIST_LOG_INFO,
		"[STOP] clean exit (dgrams=%" PRIu64 " bytes=%" PRIu64 ")\n",
		g_datagrams, g_bytes_in);
	return ret;
}
