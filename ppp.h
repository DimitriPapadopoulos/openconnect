/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2020 David Woodhouse, Daniel Lenski
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>, Daniel Lenski <dlenski@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#ifndef __OPENCONNECT_PPP_H__
#define __OPENCONNECT_PPP_H__

/* PPP protocol field values */
#define PPP_LCP		0xc021
#define PPP_IPCP	0x8021
#define PPP_IP6CP	0x8057
#define PPP_IP		0x21
#define PPP_IP6		0x57

/* NCP packet formats (https://tools.ietf.org/html/rfc1661#section-3.2) */
#define CONFREQ 1
#define CONFACK 2
#define CONFNAK 3
#define CONFREJ 4
#define TERMREQ 5
#define TERMACK 6
#define CODEREJ 7
#define PROTREJ 8
#define ECHOREQ 9
#define ECHOREP 10
#define DISCREQ 11

/* HDLC (https://tools.ietf.org/html/rfc1662) */
#define PPPINITFCS16    0xffff  /* Initial FCS value */
#define PPPGOODFCS16    0xf0b8  /* Good final FCS value */
#define ASYNCMAP_LCP 0xffffffffUL /* When sending LCP, always escape characters < 0x20 */

/* Negotiable options (our own values, not wire protocol) */
#define ACCOMP 1		/* https://tools.ietf.org/html/rfc1661#section-6.6 */
#define PFCOMP 2		/* https://tools.ietf.org/html/rfc1661#section-6.5 */
#define VJCOMP 4		/* https://tools.ietf.org/html/rfc1332#section-4 */

/* PPP states (https://tools.ietf.org/html/rfc1661#section-3.2) */
#define PPPS_DEAD		0
#define PPPS_ESTABLISH		1
#define PPPS_OPENED		2
#define PPPS_AUTHENTICATE	3
#define PPPS_NETWORK		4
#define PPPS_TERMINATE		5

/* NCP states */
#define NCP_CONF_REQ_RECEIVED	1
#define NCP_CONF_REQ_SENT	2
#define NCP_CONF_ACK_RECEIVED	4
#define NCP_CONF_ACK_SENT	8
#define NCP_TERM_REQ_SENT	16
#define NCP_TERM_REQ_RECEIVED	32
#define NCP_TERM_ACK_SENT	16
#define NCP_TERM_ACK_RECEIVED	32

struct oc_ncp {
	int state;
	int id;
	time_t last_req;
};

struct oc_ppp {
	/* We need to know these before we start */
	int encap;
	int encap_len;
	int hdlc;
	int want_ipv4;
	int want_ipv6;

	int ppp_state;
	struct oc_ncp lcp;
	struct oc_ncp ipcp;
	struct oc_ncp ip6cp;

	/* Outgoing options */
	uint32_t out_asyncmap;
	int out_lcp_opts;
	int32_t out_lcp_magic; /* stored in on-the-wire order */
	struct in_addr out_peer_addr;
	uint64_t out_ipv6_int_ident;

	/* Incoming options */
	int exp_ppp_hdr_size;
	uint32_t in_asyncmap;
	int in_lcp_opts;
	int32_t in_lcp_magic; /* stored in on-the-wire order */
	struct in_addr in_peer_addr;
	uint64_t in_ipv6_int_ident;
};

#endif /* __OPENCONNECT_PPP_H__ */
