/*
 * Copyright (C) 2006-2012, Julien Ridoux <julien@synclab.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "../config.h"
#ifdef HAVE_POSIX_TIMER
#include <sys/time.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
//#include <netinet/in.h>

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "radclock.h"
#include "radclock-private.h"
#include "radclock_daemon.h"
#include "misc.h"
#include "verbose.h"
#include "sync_history.h"
#include "sync_algo.h"
#include "config_mgr.h"
#include "proto_ntp.h"
#include "pthread_mgr.h"
#include "jdebug.h"

#define NTP_MIN_SO_TIMEOUT 100000		/* 100 ms */

/*
 * POSIX timer and signal catching mask
 * This requires FreeBSD 7.0 and above for POSIX timers.
 * Also, sigsuspend does not work on Linux in a multi-thread environment
 * (apparently) so use pthread condition wait to sync the thread to SIGALRM
 */
#ifdef HAVE_POSIX_TIMER
timer_t ntpclient_timerid;
#endif
extern pthread_mutex_t alarm_mutex;
extern pthread_cond_t alarm_cwait;


// TODO Ugly as hell
static long double last_xmt = 0.0;


int
ntp_client_init(struct radclock_handle *handle)
{
	/* Socket data */
	struct hostent *he;
	struct timeval so_timeout;

	/* Signal catching */
	struct sigaction sig_struct;
	sigset_t alarm_mask;

	/* Do we have what it takes? */
	if (strlen(handle->conf->time_server) == 0) {
		verbose(LOG_ERR, "No NTP server specified, I cannot not be a client!");
		return (1);
	}

	/* Build server infos */
	NTP_CLIENT(handle)->s_to.sin_family = PF_INET;
	NTP_CLIENT(handle)->s_to.sin_port = ntohs(handle->conf->ntp_upstream_port);
	if((he=gethostbyname(handle->conf->time_server)) == NULL) {
		herror("gethostbyname");
		return (1);
	}
	NTP_CLIENT(handle)->s_to.sin_addr.s_addr = *(in_addr_t *)he->h_addr_list[0];

	/* Create the socket */
	if ((NTP_CLIENT(handle)->socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("socket");
		return (1);
	}

	/*
	 * Set a timeout on the recv side to avoid blocking for lost packets. We set
	 * it to 800ms. Don't make me believe you are sync'ing to a server with a
	 * RTT of 800ms, that would be stupid, no?
	 */
	so_timeout.tv_sec = 0;
	so_timeout.tv_usec = NTP_MIN_SO_TIMEOUT;
	setsockopt(NTP_CLIENT(handle)->socket, SOL_SOCKET,
			SO_RCVTIMEO, (void *)(&so_timeout), sizeof(struct timeval));

	/* Initialise the signal data */
	sigemptyset(&alarm_mask);
	sigaddset(&alarm_mask, SIGALRM);
	sig_struct.sa_handler = catch_alarm; /* Not so dummy handler */
	sig_struct.sa_mask = alarm_mask;
	sig_struct.sa_flags = 0;
	sigaction(SIGALRM, &sig_struct, NULL);

	/* Initialize mutex and condition variable objects */
	pthread_mutex_init(&alarm_mutex, NULL);
	pthread_cond_init (&alarm_cwait, NULL);

#ifdef HAVE_POSIX_TIMER
	 /* CLOCK_REALTIME_HR does not exist on FreeBSD */
	if (timer_create (CLOCK_REALTIME, NULL, &ntpclient_timerid) < 0) {
		verbose(LOG_ERR, "ntp_init: POSIX timer create failed");
		return (1);
	}
	if (set_ptimer(ntpclient_timerid, 0.5 /* !0 */,
				(float) handle->conf->poll_period) < 0) {
		verbose(LOG_ERR, "ntp_init: POSIX timer cannot be set");
		return (1);
	}
#else
	if (set_itimer(0.5 /* !0 */, (float) handle->conf->poll_period) < 0) {
		verbose(LOG_ERR, "ntp_init: itimer cannot be set");
		return (1);
	}
#endif
	return (0);
}



static int
create_ntp_request(struct radclock_handle *handle, struct ntp_pkt *pkt,
		struct timeval *xmt)
{
	struct timeval reftime;
	long double time;
	vcounter_t vcount;
	int err;

	JDEBUG

	pkt->li_vn_mode = PKT_LI_VN_MODE(LEAP_NOTINSYNC, NTP_VERSION,
			MODE_CLIENT);
	pkt->stratum		= STRATUM_UNSPEC;
	pkt->stratum		= NTP_SERVER(handle)->stratum + 1;
	pkt->ppoll			= NTP_MINPOLL;
	pkt->precision		= -6;		/* Like ntpdate */
	pkt->rootdelay		= htonl(FP_SECOND);
	pkt->rootdispersion	= htonl(FP_SECOND);
	pkt->refid			= htonl(NTP_SERVER(handle)->refid);

	/* Reference time
	 * The NTP timestamp format (a bit tricky):
	 * - NTP timestamps start on 1 Jan 1900
	 * - the frac part uses higher end bits as negative power of two
	 *   (expressed in sec)
	 */
	vcount = RAD_DATA(handle)->last_changed;
	counter_to_time(&handle->rad_data, &vcount, &time);
	timeld_to_timeval(&time, &reftime);
	pkt->reftime.l_int = htonl(reftime.tv_sec + JAN_1970);
	pkt->reftime.l_fra = htonl(reftime.tv_usec * 4294967296.0 / 1e6);

	// TODO: need a more symmetric version of the packet exchange?
	pkt->org.l_int		= 0;
	pkt->org.l_fra		= 0;
	pkt->rec.l_int		= 0;
	pkt->rec.l_fra		= 0;

	/* Transmit time */

	err = radclock_get_vcounter(handle->clock, &vcount);
	if (err < 0)
		return (1);
	counter_to_time(&handle->rad_data, &vcount, &time);

	// FIXME : this test is on long double, but the conversion to NTP timestamps
	// below is based on timeval. It is then possible to pass the test but
	// convert to the same NTP key if counters read within the same micro-sec.
	// Need to remove the timeval conversion step
	if (last_xmt > 0 && time == last_xmt) {
		verbose (LOG_ERR, "xmt and last_xmt are the same !! vcount= %llu",
				(long long unsigned) vcount);
		return (1);
	}
	last_xmt = time;

	timeld_to_timeval(&time, xmt);
	pkt->xmt.l_int = htonl(xmt->tv_sec + JAN_1970);
	pkt->xmt.l_fra = htonl(xmt->tv_usec * 4294967296.0 / 1e6);

	return (0);
}


/*
 * So far this is a very basic test, we should probably do something a bit
 * smarter at one point
 */
static int
unmatched_ntp_pair(struct ntp_pkt *spkt, struct ntp_pkt *rpkt)
{
	JDEBUG

	if ((spkt->xmt.l_int == rpkt->org.l_int) &&	
			(spkt->xmt.l_fra == rpkt->org.l_fra))
		return (0);
	else
		return (1);
}




int
ntp_client(struct radclock_handle *handle)
{
	/* Timer and polling grid data */
	float adjusted_period;
	float starve_ratio;
	int attempt;

	/* Packet stuff */
	struct ntp_pkt spkt;
	struct ntp_pkt rpkt;
	unsigned int socklen;
	socklen_t tvlen;
	int ret;

	struct bidir_peer *peer;
	struct timeval tv;
	double timeout;

	JDEBUG

	peer = (struct bidir_peer *)handle->active_peer;
	starve_ratio = 1.0;
	attempt = 3;
	socklen = sizeof(struct sockaddr_in);
	tvlen = (socklen_t) sizeof(struct timeval);

	/* We are a client so we know nothing happens until we send and receive some
	 * NTP packets in here.
	 * Send a burst of requests at startup complying with ntpd implementation
	 * (to be nice). After burst period, send packets on the adjusted period
	 * grid. A bit of a luxury to benefit from the POSIX timer in here but it
	 * makes the code cleaner ... so why not :)
	 */
	if (NTP_SERVER(handle)->burst > 0) {
		NTP_SERVER(handle)->burst -= 1;
		adjusted_period = BURST_DELAY;
	} else {
		/* The logic to change the rate of polling due to starvation is
		 * delegated to the sync algo
		 */

		// TODO implement logic for starvation ratio for sleep defined by the sync algo
		adjusted_period = handle->conf->poll_period / starve_ratio;
	}

	/* Limit the number of attempts to be sure attempt * SO_TIMEOUT never
	 * exceeds the poll period or we end up in unnecessary complex situation. Of
	 * course it doesn't help us in case RTT > RAD_MINPOLL.
	 */
	getsockopt(NTP_CLIENT(handle)->socket, SOL_SOCKET, SO_RCVTIMEO,
			(void *)(&tv), &tvlen);
	timeout = tv.tv_sec + 1e-6 * tv.tv_usec;
	if (attempt > adjusted_period / timeout) {
		attempt = MAX(1, (int) adjusted_period / timeout);
	}

	/* Timer will hiccup in the 1-2 ms range if reset */
#ifdef HAVE_POSIX_TIMER
	assess_ptimer(ntpclient_timerid, adjusted_period);
#else
	assess_itimer(adjusted_period);
#endif

	/* Sleep until next grid point. Try to do as less as possible in between
	 * here and the actual sendto()
	 */
	pthread_mutex_lock(&alarm_mutex);
	pthread_cond_wait(&alarm_cwait, &alarm_mutex);
	pthread_mutex_unlock(&alarm_mutex);

	/* Keep trying to send requests that make sense.
	 * The receive call will timeout if we do not get a reply quick enough. This
	 * is good since packets can be lost and we do not want to hang with nobody
	 * on the other end of the line.
	 * On the other hand, we do not want to try continuously if the server is
	 * dead or not reachable. So limit to a certain number of attempts.
	 */
	while (attempt > 0) {
		/* Create and send an NTP packet */
		ret = create_ntp_request(handle, &spkt, &tv);
		if (ret)
			continue;

		ret = sendto(NTP_CLIENT(handle)->socket,
				(char *)&spkt, LEN_PKT_NOMAC /* No auth */, 0,
				(struct sockaddr *) &(NTP_CLIENT(handle)->s_to),
				socklen);

		if (ret < 0) {
			verbose(LOG_ERR, "NTP request failed, sendto: %s", strerror(errno));
			return (1);
		}

		verbose(VERB_DEBUG, "Sent NTP request to %s at %lu.%lu with id %llu",
				inet_ntoa(NTP_CLIENT(handle)->s_to.sin_addr),
				tv.tv_sec, tv.tv_usec,
				((uint64_t) ntohl(spkt.xmt.l_int)) << 32 |
				(uint64_t) ntohl(spkt.xmt.l_fra));

		/* This will block then timeout if nothing received
		 * (see init of the socket)
		 */
		ret = recvfrom(NTP_CLIENT(handle)->socket,
				&rpkt, sizeof(struct ntp_pkt), 0,
				(struct sockaddr*)&NTP_CLIENT(handle)->s_from,
				&socklen);

		/* If we got something, check it is a valid pair. If it is the case,
		 * then our job is finished in here. Otherwise, we send a new request.
		 */
		if (ret > 0) {
			verbose(VERB_DEBUG, "Received NTP reply from %s with id %llu",
				inet_ntoa(NTP_CLIENT(handle)->s_from.sin_addr),
				((uint64_t) ntohl(rpkt.xmt.l_int)) << 32 |
				(uint64_t) ntohl(rpkt.xmt.l_fra));

			if (unmatched_ntp_pair(&spkt, &rpkt))
				verbose(LOG_WARNING, "NTP client got a non matching pair. "
						"Increase socket timeout?");
			break;
		}
		else
			verbose(VERB_DEBUG, "No reply after 800ms. Socket timed out");

		attempt--;
	}

	/*
	 * Update socket timeout to adjust to server conditions. Athough the delay
	 * may be large, the jitter is usually fairly low (< 1ms). Give an extra 5ms
	 * to cover ugly cases. Make sure we never go below the minimum socket
	 * timeout value, and bound upper values.
	 * FIXME: peer should be initialised at this stage, but seems that has not
	 * been the case
	 */
	if (peer) {
		timeout = peer->RTThat * RAD_DATA(handle)->phat + 5e-3;
		if (timeout * 1e6 < NTP_MIN_SO_TIMEOUT)
			timeout = NTP_MIN_SO_TIMEOUT * 1e-6;
		if (timeout > adjusted_period / 3)
			timeout = adjusted_period / 3;

		tv.tv_sec = (time_t)timeout;
		tv.tv_usec = (useconds_t)(1e6 * timeout - (time_t)timeout);
		setsockopt(NTP_CLIENT(handle)->socket, SOL_SOCKET, SO_RCVTIMEO,
				(void *)(&tv), tvlen);
		verbose(VERB_DEBUG, "Adjusting NTP client socket timeout to %.3f [ms]",
				1e3 * timeout);
	}

	return (0);
}

