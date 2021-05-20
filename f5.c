/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2020-2021 David Woodhouse, Daniel Lenski
 *
 * Author: David Woodhouse <dwmw2@infradead.org>, Daniel Lenski <dlenski@gmail.com>
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

#include <config.h>

#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdarg.h>
#include <sys/types.h>

#include <libxml/HTMLparser.h>
#include <libxml/HTMLtree.h>

#include "openconnect-internal.h"
#include "ppp.h"

#define XCAST(x) ((const xmlChar *)(x))

static struct oc_auth_form *plain_auth_form() {
	struct oc_auth_form *form;
	struct oc_form_opt *opt, *opt2;

	form = calloc(1, sizeof(*form));
	if (!form) {
	nomem:
		free_auth_form(form);
		return NULL;
	}
	opt = form->opts = calloc(1, sizeof(*opt));
	if (!opt)
		goto nomem;
	opt->label = strdup("username:");
	opt->name = strdup("username");
	opt->type = OC_FORM_OPT_TEXT;

	opt2 = opt->next = calloc(1, sizeof(*opt2));
	if (!opt2)
		goto nomem;
	opt2->label = strdup("password:");
	opt2->name = strdup("password");
	opt2->type = OC_FORM_OPT_PASSWORD;
	return form;
}

static int check_cookie_success(struct openconnect_info *vpninfo)
{
	struct oc_vpn_option *cookie;
	const char *session = NULL, *f5_st = NULL;

	/* XX: The MRHSession cookie is often set repeatedly to new values prior
	 * to the completion of authentication, so it is not a sufficient
	 * indicator of successfully completed authentication by itself.
	 *
	 * The combination of the MRHSession and F5_ST cookies appears to be
	 * reliable indicator. F5_ST is a "session timeout" cookie (see
	 * https://support.f5.com/csp/article/K15387).
	 */
	for (cookie = vpninfo->cookies; cookie; cookie = cookie->next) {
		if (!strcmp(cookie->option, "MRHSession"))
			session = cookie->value;
		else if (!strcmp(cookie->option, "F5_ST"))
			f5_st = cookie->value;
	}
	if (session && f5_st) {
		free(vpninfo->cookie);
		if (asprintf(&vpninfo->cookie, "MRHSession=%s; F5_ST=%s", session, f5_st) <= 0)
			return -ENOMEM;
		return 0;
	}
	return -ENOENT;
}

int f5_obtain_cookie(struct openconnect_info *vpninfo)
{
	int ret, form_order=0;
	xmlDocPtr doc = NULL;
	xmlNode *node;
	struct oc_text_buf *req_buf = NULL;
	struct oc_auth_form *form = NULL;
	char *form_id = NULL;

	req_buf = buf_alloc();
	if ((ret = buf_error(req_buf)))
		goto out;

	while (++form_order) {
		char *resp_buf = NULL;
		char *url;

		if (req_buf && req_buf->pos)
			ret = do_https_request(vpninfo, "POST",
					       "application/x-www-form-urlencoded",
					       req_buf, &resp_buf, 2);
		else
			ret = do_https_request(vpninfo, "GET", NULL, NULL,
					       &resp_buf, 2);

		if (!check_cookie_success(vpninfo)) {
			free(resp_buf);
			ret = 0;
			break;
		}

		if (ret < 0)
			break;

		url = internal_get_url(vpninfo);
		if (!url) {
			free(resp_buf);
		nomem:
			ret = -ENOMEM;
			break;
		}

		doc = htmlReadMemory(resp_buf, ret, url, NULL,
				     HTML_PARSE_RECOVER|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING|HTML_PARSE_NONET);
		free(url);
		free(resp_buf);
		if (!doc) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to parse HTML document\n"));
			ret = -EINVAL;
			break;
		}

		buf_truncate(req_buf);

		node = find_form_node(doc);
		if (!node && form_order==1) {
			/* XX: some F5 VPNs simply do not have a static HTML form to parse */
			vpn_progress(vpninfo, PRG_ERR,
				     _("WARNING: no HTML login form found; assuming username and password fields\n"));
			if ((form = plain_auth_form()) == NULL)
				goto nomem;
		} else {
			form = parse_form_node(vpninfo, node, NULL, NULL);
			if (form_order==1 && (xmlnode_get_prop(node, "id", &form_id) || strcmp(form_id, "auth_form"))) {
				vpn_progress(vpninfo, PRG_ERR, _("Unknown form ID '%s' (expected 'auth_form')\n"),
					     form_id);

				fprintf(stderr, _("Dumping unknown HTML form:\n"));
				htmlNodeDumpFileFormat(stderr, node->doc, node, NULL, 1);
				ret = -EINVAL;
				break;
			}
		}

		if (!form) {
			ret = -EINVAL;
			break;
		}

		/* XX: do_gen_tokencode would go here, if we knew of any
		 * token-based 2FA options for F5.
		 */

		do {
			ret = process_auth_form(vpninfo, form);
		} while (ret == OC_FORM_RESULT_NEWGROUP);
		if (ret)
			goto out;

		append_form_opts(vpninfo, form, req_buf);
		if ((ret = buf_error(req_buf)))
			goto out;

		if (form->action) {
			vpninfo->redirect_url = form->action;
			form->action = NULL;
		}
		free_auth_form(form);
		form = NULL;
		if (vpninfo->redirect_url)
			handle_redirect(vpninfo);

		xmlFreeDoc(doc);
		doc = NULL;
	}

 out:
	if (doc)
		xmlFreeDoc(doc);
	free(form_id);
	if (form) free_auth_form(form);
	if (req_buf) buf_free(req_buf);
	return ret;
}

/*
 * Parse the 'favorites' profile information from
 * /vdesk/vpn/index.php3?outform=xml&client_version=2.0
 * which looks something like this:
 *
 *   <?xml version="1.0" encoding="utf-8"?>
 *     <favorites type="VPN" limited="YES">
 *       <favorite id="/Common/demo_vpn_resource">
 *         <caption>demo_vpn_resource</caption>
 *         <name>/Common/demo_vpn_resource</name>
 *         <params>resourcename=/Common/demo_vpn_resource</params>
 *       </favorite>
 *     </favorites>
 *
 * Extract the content of the "params" node which is needed for the
 * next request.
 */
static int parse_profile(struct openconnect_info *vpninfo, char *buf, int len,
			 char **params)
{
	xmlDocPtr xml_doc;
	xmlNode *xml_node, *xml_node2, *xml_node3;
	char *type = NULL;
	int ret;

	if (!buf || !len)
		return -EINVAL;

	xml_doc = xmlReadMemory(buf, len, "noname.xml", NULL,
				XML_PARSE_NOERROR|XML_PARSE_RECOVER);
	if (!xml_doc) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to parse F5 profile response\n"));
		vpn_progress(vpninfo, PRG_DEBUG,
			     _("Response was:%s\n"), buf);
		return -EINVAL;
	}
	xml_node = xmlDocGetRootElement(xml_doc);
	for (; xml_node; xml_node = xml_node->next) {
		if (xml_node->type != XML_ELEMENT_NODE)
			continue;

		if (!xmlnode_is_named(xml_node, "favorites"))
			continue;

		type = (char *)xmlGetProp(xml_node, XCAST("type"));
		if (!type)
			continue;

		if (strcmp(type, "VPN")) {
			free(type);
			continue;
		}
		free(type);

		for (xml_node2 = xmlFirstElementChild(xml_node);
		     xml_node2;
		     xml_node2 = xmlNextElementSibling(xml_node2)) {
			if (!xmlnode_is_named(xml_node2, "favorite"))
				continue;

			for (xml_node3 = xmlFirstElementChild(xml_node2);
			     xml_node3;
			     xml_node3 = xmlNextElementSibling(xml_node3)) {
				if (!xmlnode_is_named(xml_node3, "params"))
					continue;
				*params = (char *)xmlNodeGetContent(xml_node3);
				ret = 0;
				goto out;
			}
		}
	}

	vpn_progress(vpninfo, PRG_ERR,
		     _("Failed to find VPN profile parameters\n"));
	vpn_progress(vpninfo, PRG_DEBUG,
		     _("Response was:%s\n"), buf);
	ret = -EINVAL;
 out:
	xmlFreeDoc(xml_doc);
	return ret;
}

static int xmlnode_bool_or_int_value(struct openconnect_info *vpninfo, xmlNode *node)
{
	int ret = -1;
	char *content = (char *)xmlNodeGetContent(node);
	if (!content)
		return -1;

	if (isdigit(content[0]))
		ret = atoi(content);
	if (!strcasecmp(content, "yes") || !strcasecmp(content, "on"))
		ret = 1;
	if (!strcasecmp(content, "no") || !strcasecmp(content, "off"))
		ret = 0;

	free(content);
	return ret;
}

static int parse_options(struct openconnect_info *vpninfo, char *buf, int len,
			 char **session_id, char **ur_z, int *ipv4, int *ipv6, int *hdlc)
{
	xmlNode *fav_node, *obj_node, *xml_node;
	xmlDocPtr xml_doc;
	int ret = 0, n_dns = 0, n_nbns = 0, default_route = 0, dtls = 0, dtls_port = 0;
	char *s = NULL;
	struct oc_text_buf *domains = NULL;

	if (!buf || !len)
		return -EINVAL;

	xml_doc = xmlReadMemory(buf, len, "noname.xml", NULL,
				XML_PARSE_NOERROR|XML_PARSE_RECOVER);
	if (!xml_doc) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to parse F5 options response\n"));
		vpn_progress(vpninfo, PRG_DEBUG,
			     _("Response was:%s\n"), buf);
		return -EINVAL;
	}
	fav_node = xmlDocGetRootElement(xml_doc);
	if (!xmlnode_is_named(fav_node, "favorite"))
		goto err;

	obj_node = xmlFirstElementChild(fav_node);
	if (!xmlnode_is_named(obj_node, "object"))
		goto err;

	struct oc_vpn_option *new_opts = NULL;
	struct oc_ip_info new_ip_info = {};

	domains = buf_alloc();

	for (xml_node = xmlFirstElementChild(obj_node);
	     xml_node;
	     xml_node = xmlNextElementSibling(xml_node)) {
		if (xmlnode_is_named(xml_node, "ur_Z"))
			*ur_z = (char *)xmlNodeGetContent(xml_node);
		else if (xmlnode_is_named(xml_node, "Session_ID"))
			*session_id = (char *)xmlNodeGetContent(xml_node);
		else if (xmlnode_is_named(xml_node, "IPV4_0"))
			*ipv4 = xmlnode_bool_or_int_value(vpninfo, xml_node);
		else if (xmlnode_is_named(xml_node, "IPV6_0")) {
			if (!vpninfo->disable_ipv6)
				*ipv6 = xmlnode_bool_or_int_value(vpninfo, xml_node);
		} else if (xmlnode_is_named(xml_node, "hdlc_framing"))
			*hdlc = xmlnode_bool_or_int_value(vpninfo, xml_node);
		else if (xmlnode_is_named(xml_node, "idle_session_timeout")) {
			int sec = vpninfo->idle_timeout = xmlnode_bool_or_int_value(vpninfo, xml_node);
			vpn_progress(vpninfo, PRG_INFO, _("Idle timeout is %d minutes\n"), sec/60);
		} else if (xmlnode_is_named(xml_node, "tunnel_dtls"))
			dtls = xmlnode_bool_or_int_value(vpninfo, xml_node);
		else if (xmlnode_is_named(xml_node, "tunnel_port_dtls"))
			dtls_port = xmlnode_bool_or_int_value(vpninfo, xml_node);
		else if (xmlnode_is_named(xml_node, "dtls_v1_2_supported"))
			vpninfo->dtls12 = xmlnode_bool_or_int_value(vpninfo, xml_node);
		else if (xmlnode_is_named(xml_node, "UseDefaultGateway0")) {
			default_route = xmlnode_bool_or_int_value(vpninfo, xml_node);
			if (default_route)
				vpn_progress(vpninfo, PRG_INFO, _("Got default routes\n"));
		} else if (xmlnode_is_named(xml_node, "AllowLocalSubnetAccess0")) {
			if (xmlnode_bool_or_int_value(vpninfo, xml_node) == 0) {
				/* XX: Server wants us to make the local network unreachable except through the VPN.
				 * (See https://techdocs.f5.com/content/kb/en-us/products/big-ip_apm/manuals/product/apm-network-access-13-0-0/2.html
				 * for reference information, and https://techdocs.f5.com/kb/en-us/products/big-ip_apm/manuals/related/apm-f5-access-macos-2-0-0/2.html
				 * for how poorly this seems to work in practice.)
				 *
				 * "Select this option to enable local subnet access and local access to any host or
				 * subnet in routes that you have specified in the client routing table."
				 */
				new_ip_info.unreachable_ipv4 = new_ip_info.unreachable_ipv6 = 0;
				vpn_progress(vpninfo, PRG_INFO, _("Server asked us to disable all IP traffic except through VPN.\n"));
			}
		} else if (   xmlnode_is_named(xml_node, "AllowLocalDNSServersAccess0")
			   || xmlnode_is_named(xml_node, "AllowLocalDHCPAccess0")) {
			/* XX: Implementing these requires "integrated IP filtering" (deep packet inspection). No. Just no. */
		} else if (xmlnode_is_named(xml_node, "SplitTunneling0")) {
			int st = xmlnode_bool_or_int_value(vpninfo, xml_node);
			vpn_progress(vpninfo, PRG_INFO, _("Got SplitTunneling0 value of %d\n"), st);
			/* XX: Should we ignore split-{in,ex}cludes if this is zero? */
                }
		/* XX: This is an objectively stupid way to use XML, a hierarchical data format. */
		else if (   (!strncmp((char *)xml_node->name, "DNS", 3) && isdigit(xml_node->name[3]))
			 || (!strncmp((char *)xml_node->name, "DNS6_", 5) && isdigit(xml_node->name[5])) ) {
			free(s);
			s = (char *)xmlNodeGetContent(xml_node);
			if (s && *s) {
				vpn_progress(vpninfo, PRG_INFO, _("Got DNS server %s\n"), s);
				if (n_dns < 3) new_ip_info.dns[n_dns++] = add_option_steal(&new_opts, "DNS", &s);
			}
		} else if (!strncmp((char *)xml_node->name, "WINS", 4) && isdigit(xml_node->name[4])) {
			free(s);
			s = (char *)xmlNodeGetContent(xml_node);
			if (s && *s) {
				vpn_progress(vpninfo, PRG_INFO, _("Got WINS/NBNS server %s\n"), s);
				if (n_nbns < 3) new_ip_info.dns[n_nbns++] = add_option_steal(&new_opts, "WINS", &s);
			}
		} else if (!strncmp((char *)xml_node->name, "DNSSuffix", 9) && isdigit(xml_node->name[9])) {
			free(s);
			s = (char *)xmlNodeGetContent(xml_node);
			if (s && *s) {
				vpn_progress(vpninfo, PRG_INFO, _("Got search domain %s\n"), s);
				buf_append(domains, "%s ", s);
			}
		}
		/* XX: Like the above, but even stupider because one tag can contain multiple space-separated values. */
		else if (   (!strncmp((char *)xml_node->name, "LAN", 3) && isdigit((char)xml_node->name[3]))
			 || (!strncmp((char *)xml_node->name, "LAN6_", 5) && isdigit((char)xml_node->name[5]))
			 || (!strncmp((char *)xml_node->name, "ExcludeSubnets", 14) && isdigit((char)xml_node->name[14]))
			 || (!strncmp((char *)xml_node->name, "ExcludeSubnets6_", 16) && isdigit((char)xml_node->name[16]))) {
			free(s);
			s = (char *)xmlNodeGetContent(xml_node);
			if (s && *s) {
				char *word, *next;
				int is_exclude = (xml_node->name[0] == 'E');
				const char *option = is_exclude ? "split-exclude" : "split-include";

				for (word = s; *word; word = next) {
					for (next = word; *next && !isspace(*next); next++);
					if (*next)
						*next++ = 0;
					if (next == word + 1)
						continue;

					struct oc_split_include *inc = malloc(sizeof(*inc));
					if (!inc)
						continue;
					inc->route = add_option_dup(&new_opts, option, word, -1);
					if (is_exclude) {
						inc->next = new_ip_info.split_excludes;
						new_ip_info.split_excludes = inc;
						vpn_progress(vpninfo, PRG_INFO, _("Got split exclude route %s\n"), word);
					} else {
						inc->next = new_ip_info.split_includes;
						new_ip_info.split_includes = inc;
						vpn_progress(vpninfo, PRG_INFO, _("Got split include route %s\n"), word);
					}
				}
			}
		}
	}

	if (dtls && dtls_port && vpninfo->dtls_state == DTLS_NOSECRET) {
		vpn_progress(vpninfo, PRG_INFO, _("DTLS is enabled on port %d\n"), dtls_port);
		if (!*hdlc) {
			udp_sockaddr(vpninfo, dtls_port);
			vpninfo->dtls_state = DTLS_SECRET;
		} else {
			/* XX: HDLC-like framing (RFC1662) means that tunneled packets may double in size as part of
			 * their on-the-wire encapsulation, while efficient datagram transport requires calculation
			 * of a predictable maximum transfer unit.
			 *
			 * We hope no servers expect us to combine them. If they do, we should reject DTLS.
			 */
			vpn_progress(vpninfo, PRG_ERR,
				     _("WARNING: Server enables DTLS, but also requires HDLC. Disabling DTLS,\n"
				       "    because HDLC prevents determination of efficient and consistent MTU.\n"));
		}
	}
	if (default_route && *ipv4)
		new_ip_info.netmask = add_option_dup(&new_opts, "netmask", "0.0.0.0", -1);
	if (default_route && *ipv6)
		new_ip_info.netmask6 = add_option_dup(&new_opts, "netmask6", "::/0", -1);
	if (buf_error(domains) == 0 && domains->pos > 0) {
		domains->data[domains->pos-1] = '\0';
		new_ip_info.domain = add_option_steal(&new_opts, "search", &domains->data);
	}
	buf_free(domains);

	ret = install_vpn_opts(vpninfo, new_opts, &new_ip_info);

	if (ret || (*ipv4 < 1 && *ipv6 < 1) || !*ur_z || !*session_id) {
		free_optlist(new_opts);
                free_split_routes(&new_ip_info);
	err:
		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to find VPN options\n"));
		vpn_progress(vpninfo, PRG_DEBUG,
			     _("Response was:%s\n"), buf);
		ret = -EINVAL;
	}
 	xmlFreeDoc(xml_doc);
	free(s);
	return ret;
}

static int get_ip_address(struct openconnect_info *vpninfo, char *header, char *val)
{
	struct oc_ppp *ppp = vpninfo->ppp;

	if (!ppp || ppp->ppp_state != PPPS_DEAD)
		return 0;

	/* If the addresses were already negotiated once in PPP and this
	 * is a reconnect, they'll be in vpninfo->ip_info.addr*. In that
	 * case don't overwrite them, and let it correctly abort if the
	 * server rejects the same addresses this time round. */
	if (!strcasecmp(header, "X-VPN-client-IP")) {
		vpn_progress(vpninfo, PRG_INFO,
			     _("Got Legacy IP address %s\n"), val);
		if (!vpninfo->ip_info.addr)
			ppp->out_ipv4_addr.s_addr = inet_addr(val);
	} else if (!strcasecmp(header, "X-VPN-client-IPv6")) {
		vpn_progress(vpninfo, PRG_INFO,
			     _("Got IPv6 address %s\n"), val);
		if (!vpninfo->ip_info.addr6 && !vpninfo->ip_info.netmask6)
			inet_pton(AF_INET6, val, &ppp->out_ipv6_addr);
	}
        /* XX: The server's IP address(es) X-VPN-server-{IP,IPv6} are also
         * sent, but the utility of these is unclear. As remarked in oncp.c,
	 * "this is a tunnel; having a gateway is meaningless." */
	return 0;
}

static int f5_configure(struct openconnect_info *vpninfo)
{
	int ret;
	struct oc_text_buf *reqbuf = NULL;
	struct oc_vpn_option *cookie;
	char *profile_params = NULL;
	char *sid = NULL, *ur_z = NULL;
	int ipv4 = -1, ipv6 = -1, hdlc = 0;
	char *res_buf = NULL;

	if (!vpninfo->cookies) {
		/* XX: This will happen if authentication was separate/external */
		ret = internal_split_cookies(vpninfo, 1, "MRHSession");
		if (ret)
			return ret;
	}

	/* XX: parse "session timeout" cookie to get auth expiration */
	for (cookie = vpninfo->cookies; cookie; cookie = cookie->next) {
		if (!strcmp(cookie->option, "F5_ST")) {
			int junk, start, dur;
			char c = 0;
			if (sscanf(cookie->value, "%dz%dz%dz%dz%d%c", &junk, &junk, &junk, &start, &dur, &c) >= 5
			    && (c == 0 || c == 'z'))
				vpninfo->auth_expiration = start + dur;
			break;
		}
	}

	free(vpninfo->urlpath);
	vpninfo->urlpath = strdup("vdesk/vpn/index.php3?outform=xml&client_version=2.0");
	ret = do_https_request(vpninfo, "GET", NULL, NULL, &res_buf, 0);
	if (ret < 0)
		goto out;

	ret = parse_profile(vpninfo, res_buf, ret, &profile_params);
	if (ret)
		goto out;

	vpn_progress(vpninfo, PRG_DEBUG,
		     _("Got profile parameters '%s'\n"), profile_params);

	free(res_buf);
	res_buf = NULL;

	free(vpninfo->urlpath);
	if (asprintf(&vpninfo->urlpath, "vdesk/vpn/connect.php3?%s&outform=xml&client_version=2.0",
		     profile_params) == -1) {
		ret = -ENOMEM;
		goto out;
	}
	ret = do_https_request(vpninfo, "GET", NULL, NULL, &res_buf, 0);
	if (ret < 0)
		goto out;

	ret = parse_options(vpninfo, res_buf, ret, &sid, &ur_z, &ipv4, &ipv6, &hdlc);
	if (ret)
		goto out;

	vpn_progress(vpninfo, PRG_DEBUG,
		     _("Got ipv4 %d ipv6 %d hdlc %d ur_Z '%s'\n"), ipv4, ipv6, hdlc, ur_z);

	if (ipv4 == -1)
		ipv4 = 0;
	if (ipv6 == -1)
		ipv6 = 0;

	/* XX: This buffer is used to initiate the connection over either TLS or DTLS.
	 * Cookies are not needed for it to succeed, and can potentially grow without bound,
	 * which would make it too big to fit in a single DTLS packet (ick, HTTP over DTLS).
	 *
	 * Don't blame me. I didn't design this.
	 */
	reqbuf = vpninfo->ppp_dtls_connect_req;
	if (!reqbuf)
		reqbuf = buf_alloc();
	buf_truncate(reqbuf);
	buf_append(reqbuf, "GET /myvpn?sess=%s&hdlc_framing=%s&ipv4=%s&ipv6=%s&Z=%s&hostname=",
		   sid, hdlc?"yes":"no", ipv4?"yes":"no", ipv6?"yes":"no", ur_z);
	buf_append_base64(reqbuf, vpninfo->localname, strlen(vpninfo->localname), 0);
	buf_append(reqbuf, " HTTP/1.1\r\n");
	struct oc_vpn_option *saved_cookies = vpninfo->cookies;
	vpninfo->cookies = NULL; /* hide cookies */
	http_common_headers(vpninfo, reqbuf);
	vpninfo->cookies = saved_cookies; /* restore cookies */
	buf_append(reqbuf, "\r\n");

	if (buf_error(reqbuf)) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Error establishing F5 connection\n"));
		ret = buf_error(reqbuf);
		goto out;
	}
	vpninfo->ppp_dtls_connect_req = reqbuf;
	reqbuf = NULL;

	ret = openconnect_ppp_new(vpninfo, hdlc ? PPP_ENCAP_F5_HDLC : PPP_ENCAP_F5, ipv4, ipv6);

 out:
	free(res_buf);
	free(profile_params);
	free(sid);
	free(ur_z);
	buf_free(reqbuf);

	return ret;
}

int f5_connect(struct openconnect_info *vpninfo)
{
	int ret = 0;

	if (!vpninfo->ppp) {
		/* Initial connection */
		ret = f5_configure(vpninfo);
	} else if (vpninfo->ppp->ppp_state != PPPS_DEAD) {
		/* TLS/DTLS reconnection with already-established PPP session
		 * (PPP session will persist past reconnect.)
		 */
		ret = ppp_reset(vpninfo);
	}
	if (ret) {
	err:
		openconnect_close_https(vpninfo, 0);
		return ret;
	}

	ret = ppp_tcp_should_connect(vpninfo);
	if (ret <= 0)
		goto err;

	ret = openconnect_open_https(vpninfo);
	if (ret)
		goto err;

	if (vpninfo->dump_http_traffic)
		dump_buf(vpninfo, '>', vpninfo->ppp_dtls_connect_req->data);

	ret = vpninfo->ssl_write(vpninfo, vpninfo->ppp_dtls_connect_req->data,
				 vpninfo->ppp_dtls_connect_req->pos);
	if (ret < 0)
		goto err;

	struct oc_text_buf *resp_buf = buf_alloc();
	if (buf_error(resp_buf)) {
		ret = buf_free(resp_buf);
		goto err;
	}

	ret = process_http_response(vpninfo, 1, get_ip_address, resp_buf);
	buf_free(resp_buf);
	if (ret < 0)
		goto err;

	if (ret != 201 && ret != 200) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Unexpected %d result from server\n"),
			     ret);
		ret = (ret == 504) ? -EPERM : -EINVAL;
		goto err;
	}

	/* Trigger the first PPP negotiations and ensure the PPP state
	 * is PPPS_ESTABLISH so that our mainloop knows we've started. */
	ppp_start_tcp_mainloop(vpninfo);

	monitor_fd_new(vpninfo, ssl);
	monitor_read_fd(vpninfo, ssl);
	monitor_except_fd(vpninfo, ssl);

	return 0;
}

int f5_bye(struct openconnect_info *vpninfo, const char *reason)
{
	char *orig_path;
	char *res_buf = NULL;
	int ret;

	/* We need to close and reopen the HTTPS connection (to kill
	 * the f5 tunnel) and submit a new HTTPS request to logout.
	 */
	openconnect_close_https(vpninfo, 0);

	orig_path = vpninfo->urlpath;
	vpninfo->urlpath = strdup("vdesk/hangup.php3?hangup_error=1"); /* redirect segfaults without strdup */
	ret = do_https_request(vpninfo, "GET", NULL, NULL, &res_buf, 0);
	free(vpninfo->urlpath);
	vpninfo->urlpath = orig_path;

	if (ret < 0)
		vpn_progress(vpninfo, PRG_ERR, _("Logout failed.\n"));
	else
		vpn_progress(vpninfo, PRG_INFO, _("Logout successful.\n"));

	free(res_buf);
	return ret;
}


int f5_dtls_catch_probe(struct openconnect_info *vpninfo, struct pkt *pkt)
{
	char *line = (void *)pkt->data, *cr, *colon;
	int first = 1, status = -1;

	pkt->data[pkt->len] = 0;
	while (line && *line) {
		if (*line == '\n') {
			line++;
			continue;
		}
		cr = strchr(line, '\r');

		if (!cr)
			break;

		*cr = 0;

		if (first) {
			char junk, c = 0;

			if (sscanf(line, "HTTP/%c.%c %d%c", &junk, &junk, &status, &c) >= 3
			    && (c == 0 || isspace(c))
			    && status == 200) {
				first = 0;
			} else
				return (status >= 400 && status <= 499) ? -EPERM : -EINVAL;
		} else {
			colon = strchr(line, ':');
			if (colon) {
				*colon = 0;
				colon++;

				while (isspace(*colon))
					colon++;

				get_ip_address(vpninfo, line, colon);
			}
		}
		line = cr + 1;
	}
	return 1;
}
