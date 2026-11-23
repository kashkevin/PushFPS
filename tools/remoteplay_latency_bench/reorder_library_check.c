// SPDX-License-Identifier: AGPL-3.0-only
// Runs the video reorder queue of a staged chiaki-ng tree (the pin with patches
// 0001-0004 applied) in virtual time, on a packet arrival trace, and prints
// what it released and what patch 0004's counters say, the per-frame
// late-packet evidence (CHIAKI_VEYRA_STAT_LATE_FRAMES_FAILED) among them. reorder_library_check.sh
// compares that, line for line, with include/veyra/remoteplay/ReorderQueueModel.h
// replaying the same trace (reorder_replay.cpp).
//
// It includes the library's lib/src/takion.c to reach its static functions, so
// this file is AGPL-3.0 like the library, and it is a development check only:
// never built into the application. Linux, no network, no console: the socket,
// crypto and protocol parts of takion.c are compiled but never called (the
// linker is told to leave them unresolved).
//
// Trace on stdin, times in microseconds and rising:
//   S <timeout_us>            the wait the connection starts with (first line)
//   O <options>               CHIAKI_VEYRA_OPTION_* latched for the connection (optional, after S)
//   P <time> <packet index> <frame index> <units in frame> <FEC units> <unit index>
//                             a video packet read from the socket
// The frame side is a stand-in for the video receiver, the same rule as the
// model's (reorder_replay.cpp): a frame is complete when its source-unit count
// of packets has been released; a frame that is not when a packet of a later
// frame is released failed (FRAMES_FEC_FAILED, then
// chiaki_veyra_takion_video_frame_failed, as videoreceiver.c does).
//   W <time> <timeout_us>     chiaki_veyra_transport_set_av_reorder_timeout_live
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t virtual_now_us;
#include "lib/src/takion.c"

CHIAKI_EXPORT uint64_t chiaki_time_now_monotonic_us() { return virtual_now_us; }
CHIAKI_EXPORT void chiaki_log(ChiakiLog *log, ChiakiLogLevel level, const char *fmt, ...) { (void)log; (void)level; (void)fmt; }

static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

static ChiakiErrorCode check_parse(ChiakiTakionAVPacket *packet, ChiakiKeyState *key_state, uint8_t *buf, size_t buf_size)
{
	(void)key_state;
	if(buf_size < 10)
		return CHIAKI_ERR_BUF_TOO_SMALL;
	memset(packet, 0, sizeof(*packet));
	packet->is_video = true;
	packet->packet_index = (ChiakiSeqNum16)le16(buf);
	packet->frame_index = (ChiakiSeqNum16)le16(buf + 2);
	packet->units_in_frame_total = le16(buf + 4);
	packet->units_in_frame_fec = le16(buf + 6);
	packet->unit_index = (ChiakiSeqNum16)le16(buf + 8);
	return CHIAKI_ERR_SUCCESS;
}

static bool frame_started;
static uint16_t frame_current;
static unsigned int frame_released;
static bool frame_handed;

static void check_cb(ChiakiTakionEvent *event, void *user)
{
	ChiakiTakion *takion = user;
	if(event->type != CHIAKI_TAKION_EVENT_TYPE_AV)
		return;
	ChiakiTakionAVPacket *av = event->av;
	printf("R %u %llu\n", (unsigned int)av->packet_index, (unsigned long long)virtual_now_us);
	uint16_t frame = (uint16_t)av->frame_index;
	if(frame_started && chiaki_seq_num_16_lt(frame, frame_current))
		return;
	if(!frame_started || chiaki_seq_num_16_gt(frame, frame_current))
	{
		if(frame_started && !frame_handed)
		{
			chiaki_veyra_transport_stat_add(CHIAKI_VEYRA_STAT_FRAMES_FEC_FAILED, 1);
			chiaki_veyra_takion_video_frame_failed(takion, frame_current);
		}
		frame_started = true;
		frame_current = frame;
		frame_released = 0;
		frame_handed = false;
	}
	if(++frame_released >= (unsigned int)(av->units_in_frame_total - av->units_in_frame_fec))
		frame_handed = true;
}

// The Takion thread's loop between two socket reads: timed waits that run out
// before `until`.
static void run_timers(ChiakiTakion *takion, uint64_t until)
{
	for(;;)
	{
		uint64_t timeout_ms = takion_av_queues_next_timeout_ms(takion);
		if(timeout_ms == UINT64_MAX)
			return;
		if(timeout_ms == 0)
		{
			virtual_now_us += 1; // the loop comes straight back; real time has moved on
			takion_av_queues_flush_with_timeout(takion);
			continue;
		}
		if(virtual_now_us + timeout_ms * 1000 > until)
			return;
		virtual_now_us += timeout_ms * 1000;
		takion_av_queues_flush_with_timeout(takion);
	}
}

int main(void)
{
	ChiakiTakion *takion = calloc(1, sizeof(ChiakiTakion));
	if(!takion)
		return 2;
	takion->cb = check_cb;
	takion->cb_user = takion;
	takion->av_packet_parse = check_parse;
	takion->av_reorder_timeout_us = TAKION_AV_REORDER_TIMEOUT_US;
	char kind;
	unsigned long long a, b, frame, total, fec, unit;
	while(scanf(" %c %llu", &kind, &a) == 2)
	{
		if(kind == 'S')
		{
			// chiaki_takion_connect latches the tuning; the same lines, without a socket.
			chiaki_veyra_transport_set_tuning((uint32_t)a, 0);
			chiaki_veyra_transport_stats_reset();
			takion->av_reorder_timeout_us = a ? (int64_t)a : TAKION_AV_REORDER_TIMEOUT_US;
			chiaki_veyra_transport_stat_set(CHIAKI_VEYRA_STAT_REORDER_TIMEOUT_US, (uint64_t)takion->av_reorder_timeout_us);
			continue;
		}
		if(kind == 'O')
		{
			takion->veyra_options = (uint32_t)a; // as chiaki_takion_connect latches chiaki_veyra_transport_set_options
			continue;
		}
		if(scanf("%llu", &b) != 1)
			return 2;
		run_timers(takion, a);
		virtual_now_us = a;
		if(kind == 'W')
			chiaki_veyra_transport_set_av_reorder_timeout_live((uint32_t)b);
		else if(kind == 'P')
		{
			if(scanf("%llu %llu %llu %llu", &frame, &total, &fec, &unit) != 4)
				return 2;
			uint8_t *buf = malloc(10);
			if(!buf)
				return 2;
			const unsigned long long fields[5] = { b, frame, total, fec, unit };
			for(int i = 0; i < 5; i++)
			{
				buf[2 * i] = (uint8_t)(fields[i] & 0xff);
				buf[2 * i + 1] = (uint8_t)((fields[i] >> 8) & 0xff);
			}
			takion_handle_packet_av(takion, TAKION_PACKET_TYPE_VIDEO, buf, 10);
		}
		else
			return 2;
	}
	run_timers(takion, UINT64_MAX / 2);
	printf("C videoPackets=%llu gaps=%llu timeouts=%llu skipped=%llu dropped=%llu late=%llu overshootUs=%llu windowUs=%llu fecFailed=%llu lateFramesFailed=%llu earlyCuts=%llu earlyWrong=%llu lateness=",
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_VIDEO_PACKETS),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_GAPS),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_TIMEOUTS),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_SKIPPED),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_DROPPED),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_LATE_ARRIVALS),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_OVERSHOOT_US),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_TIMEOUT_US),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_FRAMES_FEC_FAILED),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_LATE_FRAMES_FAILED),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_EARLY_LOSS_CUTS),
		(unsigned long long)chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_EARLY_LOSS_WRONG));
	for(int i = 0; i < CHIAKI_VEYRA_REORDER_LATENESS_BUCKETS; i++)
		printf("%s%llu", i ? "," : "", (unsigned long long)chiaki_veyra_transport_stat_get((ChiakiVeyraTransportStat)(CHIAKI_VEYRA_STAT_REORDER_LATENESS_0 + i)));
	printf("\n");
	return 0;
}
