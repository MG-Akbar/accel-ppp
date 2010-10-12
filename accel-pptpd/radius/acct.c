#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/if_ppp.h>
#include <openssl/md5.h>

#include "log.h"
#include "radius_p.h"

#include "memdebug.h"

#define STAT_UPDATE_INTERVAL (10 * 60 * 1000)

static int req_set_RA(struct rad_req_t *req, const char *secret)
{
	MD5_CTX ctx;
	
	if (rad_packet_build(req->pack, req->RA))
		return -1;

	MD5_Init(&ctx);
	MD5_Update(&ctx, req->pack->buf, req->pack->len);
	MD5_Update(&ctx, secret, strlen(secret));
	MD5_Final(req->pack->buf + 4, &ctx);

	return 0;
}

static void req_set_stat(struct rad_req_t *req, struct ppp_t *ppp)
{
	struct ifpppstatsreq ifreq;

	memset(&ifreq, 0, sizeof(ifreq));
	ifreq.stats_ptr = (void *)&ifreq.stats;
	strcpy(ifreq.ifr__name, ppp->ifname);

	if (ioctl(sock_fd, SIOCGPPPSTATS, &ifreq)) {
		log_ppp_error("radius: failed to get ppp statistics: %s\n", strerror(errno));
		return;
	}

	if (ifreq.stats.p.ppp_ibytes < req->rpd->acct_input_octets)
		req->rpd->acct_input_gigawords++;
	req->rpd->acct_input_octets = ifreq.stats.p.ppp_ibytes;

	if (ifreq.stats.p.ppp_obytes < req->rpd->acct_output_octets)
		req->rpd->acct_output_gigawords++;
	req->rpd->acct_output_octets = ifreq.stats.p.ppp_obytes;

	rad_packet_change_int(req->pack, "Acct-Input-Octets", ifreq.stats.p.ppp_ibytes);
	rad_packet_change_int(req->pack, "Acct-Output-Octets", ifreq.stats.p.ppp_obytes);
	rad_packet_change_int(req->pack, "Acct-Input-Packets", ifreq.stats.p.ppp_ipackets);
	rad_packet_change_int(req->pack, "Acct-Output-Packets", ifreq.stats.p.ppp_opackets);
	rad_packet_change_int(req->pack, "Acct-Input-Gigawords", req->rpd->acct_input_gigawords);
	rad_packet_change_int(req->pack, "Acct-Output-Gigawords", req->rpd->acct_output_gigawords);
	rad_packet_change_int(req->pack, "Acct-Session-Time", time(NULL) - ppp->start_time);
}

static int rad_acct_read(struct triton_md_handler_t *h)
{
	struct rad_req_t *req = container_of(h, typeof(*req), hnd);

	if (req->reply)
		rad_packet_free(req->reply);

	req->reply = rad_packet_recv(h->fd, NULL);
	if (!req->reply)
		return 0;

	if (conf_verbose) {
		log_ppp_info("recv ");
		rad_packet_print(req->reply, log_ppp_info);
	}

	if (req->reply->code != CODE_ACCOUNTING_RESPONSE || req->reply->id != req->pack->id) {
		rad_packet_free(req->reply);
		req->reply = NULL;
	} else {
		req->pack->id++;
		if (req->timeout.tpd)
			triton_timer_del(&req->timeout);
	}

	return 0;
}

static void rad_acct_timeout(struct triton_timer_t *t)
{
	struct rad_req_t *req = container_of(t, typeof(*req), timeout);

	rad_req_send(req);
}

static void rad_acct_interim_update(struct triton_timer_t *t)
{
	struct radius_pd_t *rpd = container_of(t, typeof(*rpd), acct_interim_timer);

	if (rpd->acct_req->timeout.tpd)
		return;

	req_set_stat(rpd->acct_req, rpd->ppp);
	if (!rpd->acct_interim_interval)
		return;

	rad_packet_change_val(rpd->acct_req->pack, "Acct-Status-Type", "Interim-Update");
	req_set_RA(rpd->acct_req, conf_acct_secret);
	rad_req_send(rpd->acct_req);
	rpd->acct_req->timeout.period = conf_timeout * 1000;
	triton_timer_add(rpd->ppp->ctrl->ctx, &rpd->acct_req->timeout, 0);
}

int rad_acct_start(struct radius_pd_t *rpd)
{
	rpd->acct_req = rad_req_alloc(rpd, CODE_ACCOUNTING_REQUEST, rpd->ppp->username);
	if (!rpd->acct_req) {
		log_emerg("radius: out of memory\n");
		return -1;
	}

	if (rad_req_acct_fill(rpd->acct_req)) {
		log_ppp_error("radius:acct: failed to fill accounting attributes\n");
		goto out_err;
	}

	//if (rad_req_add_val(rpd->acct_req, "Acct-Status-Type", "Start", 4))
	//	goto out_err;
	//if (rad_req_add_str(rpd->acct_req, "Acct-Session-Id", rpd->ppp->sessionid, PPP_SESSIONID_LEN, 1))
	//	goto out_err;

	if (req_set_RA(rpd->acct_req, conf_acct_secret))
		goto out_err;

	if (rad_req_send(rpd->acct_req))
		goto out_err;
	
	rpd->acct_req->hnd.read = rad_acct_read;

	triton_md_register_handler(rpd->ppp->ctrl->ctx, &rpd->acct_req->hnd);
	if (triton_md_enable_handler(&rpd->acct_req->hnd, MD_MODE_READ))
		goto out_err;
	
	rpd->acct_req->timeout.expire = rad_acct_timeout;
	rpd->acct_req->timeout.period = conf_timeout * 1000;
	if (triton_timer_add(rpd->ppp->ctrl->ctx, &rpd->acct_req->timeout, 0)) {
		triton_md_unregister_handler(&rpd->acct_req->hnd);
		goto out_err;
	}
	
	rpd->acct_interim_timer.expire = rad_acct_interim_update;
	rpd->acct_interim_timer.period = rpd->acct_interim_interval ? rpd->acct_interim_interval * 1000 : STAT_UPDATE_INTERVAL;
	if (rpd->acct_interim_interval && triton_timer_add(rpd->ppp->ctrl->ctx, &rpd->acct_interim_timer, 0)) {
		triton_md_unregister_handler(&rpd->acct_req->hnd);
		triton_timer_del(&rpd->acct_req->timeout);
		goto out_err;
	}
	return 0;

out_err:
	rad_req_free(rpd->acct_req);
	rpd->acct_req = NULL;
	return -1;
}

void rad_acct_stop(struct radius_pd_t *rpd)
{
	int i;

	if (rpd->acct_interim_timer.tpd)
		triton_timer_del(&rpd->acct_interim_timer);

	if (rpd->acct_req) {
		triton_md_unregister_handler(&rpd->acct_req->hnd);
		if (rpd->acct_req->timeout.tpd)
			triton_timer_del(&rpd->acct_req->timeout);

		rad_packet_change_val(rpd->acct_req->pack, "Acct-Status-Type", "Stop");
		req_set_stat(rpd->acct_req, rpd->ppp);
		req_set_RA(rpd->acct_req, conf_acct_secret);
		/// !!! rad_req_add_val(rpd->acct_req, "Acct-Terminate-Cause", "");
		
		if (rpd->acct_req->reply) {
			rad_packet_free(rpd->acct_req->reply);
			rpd->acct_req->reply = NULL;
		}

		for(i = 0; i < conf_max_try; i++) {
			if (rad_req_send(rpd->acct_req))
				break;
			rad_req_wait(rpd->acct_req, conf_timeout);
			if (!rpd->acct_req->reply)
				continue;
			if (rpd->acct_req->reply->id != rpd->acct_req->pack->id || rpd->acct_req->reply->code != CODE_ACCOUNTING_RESPONSE) {
				rad_packet_free(rpd->acct_req->reply);
				rpd->acct_req->reply = NULL;
			} else
				break;
		}
		if (!rpd->acct_req->reply)
			log_ppp_warn("radius:acct_stop: no response\n");

		rad_req_free(rpd->acct_req);
		rpd->acct_req = NULL;
	}
}
