/*
 *
 *	Modified for AF_INET6 by Pedro Roque
 *
 *	<roque@di.fc.ul.pt>
 *
 *	Original copyright notice included below
 */

/*
 * Copyright (c) 1989 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Muuss.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 *			P I N G . C
 *
 * Using the InterNet Control Message Protocol (ICMP) "ECHO" facility,
 * measure round-trip-delays and packet loss across network paths.
 *
 * Author -
 *	Mike Muuss
 *	U. S. Army Ballistic Research Laboratory
 *	December, 1983
 *
 * Status -
 *	Public Domain.  Distribution Unlimited.
 * Bugs -
 *	More statistics could always be gathered.
 *	If kernel does not support ICMP datagram sockets or
 *	if -N option is used, this program has to run SUID to ROOT or
 *	with net_cap_raw enabled.
 */

// local changes by yvs@
// part of ping.c

#include "iputils_common.h"
#include "iputils_ni.h"
#include "node_info.h"
#include "ipv6.h"
#include "common.h"
#include "ping_aux.h"
#include "ping6_aux.h"
#include "ping6.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>

#include <netinet/ip6.h>
#include <linux/in6.h>
#include <linux/errqueue.h>

#ifndef IPV6_FLOWLABEL_MGR
# define IPV6_FLOWLABEL_MGR 32
#endif
#ifndef IPV6_FLOWINFO_SEND
# define IPV6_FLOWINFO_SEND 33
#endif

// func_set:send_probe
static ssize_t ping6_send_probe(struct ping_rts *rts, int sockfd,
		void *packet, unsigned packet_size)
{
	ssize_t len = niquery_is_enabled(&rts->ni) ?
		build_niquery(rts, packet, packet_size) :
		build_echo   (rts, packet);
	ssize_t rc = 0;
	if (rts->cmsglen == 0) {
		rc = sendto(sockfd, (char *)packet, len, rts->confirm,
			    (struct sockaddr *)&rts->whereto,
			    sizeof(struct sockaddr_in6));
	} else {
		struct iovec iov = { .iov_len = len, .iov_base = packet };
		struct msghdr mhdr = {
			.msg_name       = &rts->whereto,
			.msg_namelen    = sizeof(struct sockaddr_in6),
			.msg_iov        = &iov,
			.msg_iovlen     = 1,
			.msg_control    = rts->cmsgbuf,
			.msg_controllen = rts->cmsglen,
		};
		rc = sendmsg(sockfd, &mhdr, rts->confirm);
	}
	rts->confirm = 0;
	return (rc == len) ? 0 : rc;
}

// func_set:receive_error
static int ping6_receive_error(struct ping_rts *rts, const socket_st *sock) {
	int saved_errno = errno;
	//
	char cbuf[512];
	struct icmp6_hdr icmph = {0};
	struct iovec iov = { .iov_base = &icmph, .iov_len = sizeof(icmph) };
	struct sockaddr_in6 target = {0};
	struct msghdr msg = {
		.msg_name       = &target,
		.msg_namelen    = sizeof(target),
		.msg_iov        = &iov,
		.msg_iovlen     = 1,
		.msg_control    = cbuf,
		.msg_controllen = sizeof(cbuf),
	};
	//
	int net_errors = 0;
	int local_errors = 0;
	ssize_t res = recvmsg(sock->fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
	if (res < 0) {
		if (errno == EAGAIN || errno == EINTR)
			local_errors++;
	} else {
		struct sock_extended_err *ee = NULL;
		for (struct cmsghdr *m = CMSG_FIRSTHDR(&msg); m; m = CMSG_NXTHDR(&msg, m))
			if ((m->cmsg_level == IPPROTO_IPV6) && (m->cmsg_type == IPV6_RECVERR))
				ee = (struct sock_extended_err *)CMSG_DATA(m);
		if (!ee)
			abort();
		if (ee->ee_origin == SO_EE_ORIGIN_LOCAL) {
			local_errors++;
			rts->nerrors++;
			if (!rts->opt.quiet)
				print_local_ee(rts, ee);
		} else if (ee->ee_origin == SO_EE_ORIGIN_ICMP6) {
			struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&rts->whereto;
			if ((res < (ssize_t)sizeof(icmph))                  ||
			    memcmp(&target.sin6_addr, &sin6->sin6_addr, 16) ||
			    (icmph.icmp6_type != ICMP6_ECHO_REQUEST)        ||
			    !IS_OURS(rts, sock->raw, icmph.icmp6_id))
				/* Not our error, not an error at all, clear */
				saved_errno = 0;
			else {
				net_errors++;
				rts->nerrors++;
				print_addr_seq(rts, ntohs(icmph.icmp6_seq), ee,
					sizeof(struct sockaddr_in6));
			}
		}
	}
	errno = saved_errno;
	return net_errors ? net_errors : -local_errors;
}


// func_set:parse_reply:fin
static inline void ping6_parse_reply_fin(bool audible, bool flood) {
	if (audible) {
		putchar('\a');
		if (flood)
			fflush(stdout);
	}
	if (!flood) {
		putchar('\n');
		fflush(stdout);
	}
}

static inline int ping6_icmp_extra_type(struct ping_rts *rts,
		const struct icmp6_hdr *icmp, size_t received, bool raw,
		const struct sockaddr_in6 *src, const struct sockaddr_in6 *dst)
{
	const struct ip6_hdr *iph = (struct ip6_hdr *)(icmp + 1);
	const struct icmp6_hdr *orig = (struct icmp6_hdr *)(iph + 1);
	if (received < (8 + sizeof(struct ip6_hdr) + 8))
		return true;
	if (memcmp(&iph->ip6_dst, &dst->sin6_addr, sizeof(dst->sin6_addr)))
		return true;
	//
	uint8_t next = iph->ip6_nxt;
	if (next == NEXTHDR_FRAGMENT) {
		next = *(uint8_t *)orig;
		orig++;
	}
	if (next == IPPROTO_ICMPV6) {
		if (orig->icmp6_type != ICMP6_ECHO_REQUEST ||
		    !IS_OURS(rts, raw, orig->icmp6_id))
			return true;
		acknowledge(rts, ntohs(orig->icmp6_seq));
		return false;
	}
	//
	/* We've got something other than an ECHOREPLY */
	if (!rts->opt.verbose || rts->uid)
		return true;
	PRINT_TIMESTAMP;
	printf(_("From %s: "), sprint_addr(rts, src, sizeof(*src)));
	print6_icmp(icmp->icmp6_type, icmp->icmp6_code, ntohl(icmp->icmp6_mtu));
	return -1;
}

/*
 *	Print out the packet, if it came from us.  This logic is necessary
 * because ALL readers of the ICMP socket get a copy of ALL ICMP packets
 * which arrive ('tis only fair).  This permits multiple copies of this
 * program to be run without having intermingled output (or statistics!).
 */
// func_set:parse_reply
static bool ping6_parse_reply(struct ping_rts *rts, bool raw,
	struct msghdr *msg, size_t received, void *addr, const struct timeval *at)
{
	struct sockaddr_in6 *from = addr;
	uint8_t *base = msg->msg_iov->iov_base;

	int hops = -1;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
		if (c->cmsg_level == IPPROTO_IPV6)
			switch (c->cmsg_type) {
			case IPV6_HOPLIMIT:
#ifdef IPV6_2292HOPLIMIT
			case IPV6_2292HOPLIMIT:
#endif
				if (c->cmsg_len >= CMSG_LEN(sizeof(int)))
					memcpy(&hops, CMSG_DATA(c), sizeof(hops));
				break;
			default: break;
			}
	}

	if (received < 8) {
		if (rts->opt.verbose)
			warnx(_("packet too short: %zd bytes"), received);
		return true;
	}

	/* Now the ICMP part */
	struct icmp6_hdr *icmp = (struct icmp6_hdr *)base;
	struct sockaddr_in6 *whereto = (struct sockaddr_in6 *)&rts->whereto;

	if (icmp->icmp6_type == ICMP6_ECHO_REPLY) {
		if (!IS_OURS(rts, raw, icmp->icmp6_id))
			return true;
		bool okay =
		    !memcmp(&from->sin6_addr.s6_addr, &whereto->sin6_addr.s6_addr, 16)
		    || rts->multicast || rts->subnet_router_anycast;
		if (gather_stats(rts, (uint8_t *)icmp, sizeof(*icmp), received,
			ntohs(icmp->icmp6_seq), hops, at, NULL,
			sprint_addr(rts, from, sizeof(*from)), true, !okay))
				return false;
	} else if (icmp->icmp6_type == IPUTILS_NI_ICMP6_REPLY) {
		struct ni_hdr *nih = (struct ni_hdr *)icmp;
		int seq = niquery_check_nonce(&rts->ni, nih->ni_nonce);
		if (seq < 0)
			return true;
		if (gather_stats(rts, (uint8_t *)icmp, sizeof(*icmp), received,
			seq, hops, at, print6_ni_reply,
			sprint_addr(rts, from, sizeof(*from)), true, false))
				return false;
	} else {
		/* We must not ever fall here. All the messages but
		 * echo reply are blocked by filter and error are
		 * received with IPV6_RECVERR. Ugly code is preserved
		 * however, just to remember what crap we avoided
		 * using RECVRERR. :-)
		 */
		int rc = ping6_icmp_extra_type(rts, icmp, received, raw, from, whereto);
		if (rc >= 0)
			return rc;
	}
	ping6_parse_reply_fin(rts->opt.audible, rts->opt.flood);
	return false;
}

/* return >= 0: exit with this code, < 0: go on to next addrinfo result */
int ping6_run(struct ping_rts *rts, int argc, char **argv,
		struct addrinfo *ai, const socket_st *sock)
{
	static ping_func_set_st ping6_func_set = {
		.send_probe     = ping6_send_probe,
		.receive_error  = ping6_receive_error,
		.parse_reply    = ping6_parse_reply,
	};
	static uint32_t scope_id = 0;

	rts->ip6 = true;
	struct sockaddr_in6 *source   = (struct sockaddr_in6 *)&rts->source;
	struct sockaddr_in6 *firsthop = (struct sockaddr_in6 *)&rts->firsthop;
	struct sockaddr_in6 *whereto  = (struct sockaddr_in6 *)&rts->whereto;
	source->sin6_family = AF_INET6;

	if (niquery_is_enabled(&rts->ni)) {
		niquery_init_nonce(&rts->ni);
		if (!niquery_is_subject_valid(&rts->ni)) {
			rts->ni.subject      = &whereto->sin6_addr;
			rts->ni.subject_len  = sizeof(whereto->sin6_addr);
			rts->ni.subject_type = IPUTILS_NI_ICMP6_SUBJ_IPV6;
		}
	}

	char *target = NULL;
	if (argc > 1) {
		usage();
	} else if (argc == 1) {
		target = *argv;
	} else {
		if ((rts->ni.query < 0) && (rts->ni.subject_type != IPUTILS_NI_ICMP6_SUBJ_FQDN))
			usage();
		target = rts->ni.group;
	}

	memcpy(whereto, ai->ai_addr, sizeof(*whereto));
	whereto->sin6_port = htons(IPPROTO_ICMPV6);

	if (target && memchr(target, ':', strlen(target)))
		rts->opt.numeric = true;

	if (IN6_IS_ADDR_UNSPECIFIED(&firsthop->sin6_addr)) {
		memcpy(&firsthop->sin6_addr, &whereto->sin6_addr, sizeof(firsthop->sin6_addr));
		firsthop->sin6_scope_id = whereto->sin6_scope_id;
		/* Verify scope_id is the same as intermediate nodes */
		if (firsthop->sin6_scope_id && scope_id && (firsthop->sin6_scope_id != scope_id))
			errx(2, _("scope discrepancy among the nodes"));
		else if (!scope_id)
			scope_id = firsthop->sin6_scope_id;
	}

	rts->hostname = target;

	if (IN6_IS_ADDR_UNSPECIFIED(&source->sin6_addr)) {
		int probe_fd = socket(AF_INET6, SOCK_DGRAM, 0);
		if (probe_fd < 0)
			err(errno, "socket");

		bool scoped = IN6_IS_ADDR_LINKLOCAL(&firsthop->sin6_addr) ||
			      IN6_IS_ADDR_MC_LINKLOCAL(&firsthop->sin6_addr);
		if (rts->device) {
			unsigned iface = if_name2index(rts->device);
			socklen_t slen = strlen(rts->device) + 1;
			set_device(true, rts->device, slen, iface, 0, probe_fd, sock->fd);
			if (scoped)
				firsthop->sin6_scope_id = iface;
		}
		if (!scoped)
			firsthop->sin6_family = AF_INET6;
		sock_settos(probe_fd, rts->qos, rts->ip6);
		sock_setmark(rts, probe_fd);

		firsthop->sin6_port = htons(1025);
		if (connect(probe_fd, (struct sockaddr *)firsthop, sizeof(*firsthop)) < 0) {
			if ((errno == EHOSTUNREACH || errno == ENETUNREACH) && ai->ai_next) {
				close(probe_fd);
				return -1;
			}
			err(errno, "connect");
		}
		socklen_t socklen = sizeof(struct sockaddr_in6);
		if (getsockname(probe_fd, (struct sockaddr *)source, &socklen) < 0)
			err(errno, "getsockname");
		source->sin6_port = 0;
		close(probe_fd);

		if (rts->device)
			cmp_srcdev(rts);

	} else if (rts->device && (IN6_IS_ADDR_LINKLOCAL(&source->sin6_addr) ||
			      IN6_IS_ADDR_MC_LINKLOCAL(&source->sin6_addr)))
		source->sin6_scope_id = if_name2index(rts->device);

	if (rts->device) {
		struct cmsghdr *cmsg = (struct cmsghdr *)(rts->cmsgbuf + rts->cmsglen);
		rts->cmsglen += CMSG_SPACE(sizeof(struct in6_pktinfo));
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
		cmsg->cmsg_level = IPPROTO_IPV6;
		cmsg->cmsg_type = IPV6_PKTINFO;

		struct in6_pktinfo *ipi = (struct in6_pktinfo *)CMSG_DATA(cmsg);
		memset(ipi, 0, sizeof(*ipi));
		ipi->ipi6_ifindex = if_name2index(rts->device);

		if (rts->opt.strictsource) {
			ENABLE_CAPABILITY_RAW;
			int rc = setsockopt(sock->fd, SOL_SOCKET, SO_BINDTODEVICE,
					rts->device, strlen(rts->device) + 1);
			int keep = errno;
			DISABLE_CAPABILITY_RAW;
			if (rc < 0) {
				errno = keep;
				err(errno, "SO_BINDTODEVICE %s", rts->device);
			}
		}
	}

	if (IN6_IS_ADDR_MULTICAST(&whereto->sin6_addr)) {
		rts->multicast = true;
		if (rts->uid) {
			if (rts->interval < MIN_MCAST_INTERVAL_MS)
				errx(2,
_("minimal interval for multicast ping for user must be >= %d ms, use -i %s (or higher)"),
					  MIN_MCAST_INTERVAL_MS,
					  str_interval(MIN_MCAST_INTERVAL_MS));
			if ((rts->pmtudisc >= 0) && (rts->pmtudisc != IPV6_PMTUDISC_DO))
				errx(2, _("multicast ping does not fragment"));
		}
		if (rts->pmtudisc < 0)
			rts->pmtudisc = IPV6_PMTUDISC_DO;
	}

	/* detect Subnet-Router anycast at least for the default prefix 64 */
	rts->subnet_router_anycast = 1;
	for (size_t i = 8; i < sizeof(struct in6_addr); i++) {
		if (whereto->sin6_addr.s6_addr[i]) {
			rts->subnet_router_anycast = 0;
			break;
		}
	}

	mtudisc_n_bind(rts, sock);

	if ((rts->datalen >= sizeof(struct timeval)) && (rts->ni.query < 0)) {
		/* can we time transfer */
		rts->timing = true;
	}
	int packlen = rts->datalen + 8 + 4096 + 40 + 8; /* 4096 for rthdr */
	unsigned char *packet = malloc(packlen);
	if (!packet)
		err(errno, _("memory allocation failed"));

	{ int hold = 1;
	  if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_RECVERR, &hold, sizeof(hold)) < 0)
		err(errno, "IPV6_RECVERR");
	  /* Estimate memory eaten by single packet. It is rough estimate.
	   * Actually, for small datalen's it depends on kernel side a lot. */
	  hold = rts->datalen + 8;
	  hold += ((hold + 511) / 512) * (40 + 16 + 64 + 160);
	  sock_setbufs(rts, sock->fd, hold);
	}

#ifdef __linux__
	if (sock->raw) {
		int csum_offset = 2;
		if (setsockopt(sock->fd, SOL_RAW, IPV6_CHECKSUM, &csum_offset, sizeof(csum_offset)) < 0)
		/* checksum should be enabled by default and setting this option might fail anyway */
			warn(_("setsockopt(RAW_CHECKSUM) failed - try to continue"));
#else
	{
#endif
		/* select icmp echo reply as icmp type to receive */
		struct icmp6_filter filter = {0};
		ICMP6_FILTER_SETBLOCKALL(&filter);
		if (niquery_is_enabled(&rts->ni))
			ICMP6_FILTER_SETPASS(IPUTILS_NI_ICMP6_REPLY, &filter);
		else
			ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter);
		if (setsockopt(sock->fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter)) < 0)
			err(errno, "setsockopt(ICMP6_FILTER)");
	}

	if (rts->opt.noloop) {
		int loop = 0;
		if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)
			err(errno, _("cannot disable multicast loopback"));
	}
	if (rts->opt.ttl) {
		if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
				&rts->ttl, sizeof(rts->ttl)) < 0)
			err(errno, _("cannot set multicast hop limit"));
		if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS,
				&rts->ttl, sizeof(rts->ttl)) < 0)
			err(errno, _("cannot set unicast hop limit"));
	}

	{ int on = 1;
	  if (
#ifdef IPV6_RECVHOPLIMIT
	(setsockopt(sock->fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on, sizeof(on)) < 0) &&
	(setsockopt(sock->fd, IPPROTO_IPV6, IPV6_2292HOPLIMIT, &on, sizeof(on)) < 0)
#else
	(setsockopt(sock->fd, IPPROTO_IPV6, IPV6_HOPLIMIT,     &on, sizeof(on)) < 0)
#endif
	  ) err(errno, _("cannot receive hop limit"));
	}

	if (rts->opt.flowinfo) {
		char buf[CMSG_ALIGN(sizeof(struct in6_flowlabel_req)) + rts->cmsglen];
		memset(buf, 0, sizeof(buf));
		struct in6_flowlabel_req *freq = (struct in6_flowlabel_req *)buf;
		freq->flr_label = htonl(rts->flowlabel & IPV6_FLOWINFO_FLOWLABEL);
		freq->flr_action = IPV6_FL_A_GET;
		freq->flr_flags = IPV6_FL_F_CREATE;
		freq->flr_share = IPV6_FL_S_EXCL;
		memcpy(&freq->flr_dst, &whereto->sin6_addr, sizeof(whereto->sin6_addr));
		if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_FLOWLABEL_MGR, freq, sizeof(*freq)) < 0)
			err(errno, _("cannot set flowlabel"));
		whereto->sin6_flowinfo = rts->flowlabel = freq->flr_label;
		int on = 1;
		if (setsockopt(sock->fd, IPPROTO_IPV6, IPV6_FLOWINFO_SEND, &on, sizeof(on)) < 0)
			err(errno, _("cannot send flowinfo"));
	}

	ping_setup(rts, sock);
	drop_capabilities();

	int rc = main_loop(rts, &ping6_func_set, sock, packet, packlen);
	free(packet);
	return rc;
}

