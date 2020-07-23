/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/conf.h"
#include "spdk/sock.h"
#include "spdk/scsi.h"

#include "spdk_internal/log.h"

#include "iscsi/iscsi.h"
#include "iscsi/conn.h"
#include "iscsi/tgt_node.h"
#include "iscsi/portal_grp.h"
#include "iscsi/init_grp.h"
#include "iscsi/task.h"

#define MAX_TMPBUF 4096
#define MAX_MASKBUF 128

static bool
iscsi_ipv6_netmask_allow_addr(const char *netmask, const char *addr)
{
	struct in6_addr in6_mask;
	struct in6_addr in6_addr;
	char mask[MAX_MASKBUF];
	const char *p;
	size_t n;
	int bits, bmask;
	int i;

	if (netmask[0] != '[') {
		return false;
	}
	p = strchr(netmask, ']');
	if (p == NULL) {
		return false;
	}
	n = p - (netmask + 1);
	if (n + 1 > sizeof mask) {
		return false;
	}

	memcpy(mask, netmask + 1, n);
	mask[n] = '\0';
	p++;

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits <= 0 || bits > 128) {
			return false;
		}
	} else {
		bits = 128;
	}

#if 0
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "input %s\n", addr);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "mask  %s / %d\n", mask, bits);
#endif

	/* presentation to network order binary */
	if (inet_pton(AF_INET6, mask, &in6_mask) <= 0
	    || inet_pton(AF_INET6, addr, &in6_addr) <= 0) {
		return false;
	}

	/* check 128bits */
	for (i = 0; i < (bits / 8); i++) {
		if (in6_mask.s6_addr[i] != in6_addr.s6_addr[i]) {
			return false;
		}
	}
	if (bits % 8) {
		bmask = (0xffU << (8 - (bits % 8))) & 0xffU;
		if ((in6_mask.s6_addr[i] & bmask) != (in6_addr.s6_addr[i] & bmask)) {
			return false;
		}
	}

	/* match */
	return true;
}

static bool
iscsi_ipv4_netmask_allow_addr(const char *netmask, const char *addr)
{
	struct in_addr in4_mask;
	struct in_addr in4_addr;
	char mask[MAX_MASKBUF];
	const char *p;
	uint32_t bmask;
	size_t n;
	int bits;

	p = strchr(netmask, '/');
	if (p == NULL) {
		p = netmask + strlen(netmask);
	}
	n = p - netmask;
	if (n + 1 > sizeof mask) {
		return false;
	}

	memcpy(mask, netmask, n);
	mask[n] = '\0';

	if (p[0] == '/') {
		bits = (int) strtol(p + 1, NULL, 10);
		if (bits <= 0 || bits > 32) {
			return false;
		}
	} else {
		bits = 32;
	}

	/* presentation to network order binary */
	if (inet_pton(AF_INET, mask, &in4_mask) <= 0
	    || inet_pton(AF_INET, addr, &in4_addr) <= 0) {
		return false;
	}

	/* check 32bits */
	bmask = (0xffffffffU << (32 - bits)) & 0xffffffffU;
	if ((ntohl(in4_mask.s_addr) & bmask) != (ntohl(in4_addr.s_addr) & bmask)) {
		return false;
	}

	/* match */
	return true;
}

static bool
iscsi_netmask_allow_addr(const char *netmask, const char *addr)
{
	if (netmask == NULL || addr == NULL) {
		return false;
	}
	if (strcasecmp(netmask, "ANY") == 0) {
		return true;
	}
	if (netmask[0] == '[') {
		/* IPv6 */
		if (iscsi_ipv6_netmask_allow_addr(netmask, addr)) {
			return true;
		}
	} else {
		/* IPv4 */
		if (iscsi_ipv4_netmask_allow_addr(netmask, addr)) {
			return true;
		}
	}
	return false;
}

static bool
iscsi_init_grp_allow_addr(struct spdk_iscsi_init_grp *igp,
			  const char *addr)
{
	struct spdk_iscsi_initiator_netmask *imask;

	TAILQ_FOREACH(imask, &igp->netmask_head, tailq) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "netmask=%s, addr=%s\n",
			      imask->mask, addr);
		if (iscsi_netmask_allow_addr(imask->mask, addr)) {
			return true;
		}
	}
	return false;
}

static int
iscsi_init_grp_allow_iscsi_name(struct spdk_iscsi_init_grp *igp,
				const char *iqn, bool *result)
{
	struct spdk_iscsi_initiator_name *iname;

	TAILQ_FOREACH(iname, &igp->initiator_head, tailq) {
		/* denied if iqn is matched */
		if ((iname->name[0] == '!')
		    && (strcasecmp(&iname->name[1], "ANY") == 0
			|| strcasecmp(&iname->name[1], iqn) == 0)) {
			*result = false;
			return 0;
		}
		/* allowed if iqn is matched */
		if (strcasecmp(iname->name, "ANY") == 0
		    || strcasecmp(iname->name, iqn) == 0) {
			*result = true;
			return 0;
		}
	}
	return -1;
}

static struct spdk_iscsi_pg_map *
iscsi_tgt_node_find_pg_map(struct spdk_iscsi_tgt_node *target,
			   struct spdk_iscsi_portal_grp *pg);

bool
iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
		      struct spdk_iscsi_tgt_node *target, const char *iqn, const char *addr)
{
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int rc;
	bool allowed = false;

	if (conn == NULL || target == NULL || iqn == NULL || addr == NULL) {
		return false;
	}
	pg = conn->portal->group;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "pg=%d, iqn=%s, addr=%s\n",
		      pg->tag, iqn, addr);
	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		return false;
	}
	TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
		rc = iscsi_init_grp_allow_iscsi_name(ig_map->ig, iqn, &allowed);
		if (rc == 0) {
			if (allowed == false) {
				goto denied;
			} else {
				if (iscsi_init_grp_allow_addr(ig_map->ig, addr)) {
					return true;
				}
			}
		} else {
			/* netmask is denied in this initiator group */
		}
	}

denied:
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "access denied from %s (%s) to %s (%s:%s,%d)\n",
		      iqn, addr, target->name, conn->portal_host,
		      conn->portal_port, conn->pg_tag);
	return false;
}

static bool
iscsi_tgt_node_allow_iscsi_name(struct spdk_iscsi_tgt_node *target, const char *iqn)
{
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int rc;
	bool result = false;

	if (target == NULL || iqn == NULL) {
		return false;
	}

	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
			rc = iscsi_init_grp_allow_iscsi_name(ig_map->ig, iqn, &result);
			if (rc == 0) {
				return result;
			}
		}
	}

	return false;
}

static int
iscsi_send_tgt_portals(struct spdk_iscsi_conn *conn,
		       struct spdk_iscsi_tgt_node *target,
		       uint8_t *data, int alloc_len, int total)
{
	char buf[MAX_TMPBUF];
	struct spdk_iscsi_portal_grp *pg;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_portal *p;
	char *host;
	int len;

	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		pg = pg_map->pg;
		TAILQ_FOREACH(p, &pg->head, per_pg_tailq) {
			if (alloc_len - total < 1) {
				/* TODO: long text responses support */
				SPDK_ERRLOG("SPDK doesn't support long text responses now, "
					    "you can use larger MaxRecvDataSegmentLength"
					    "value in initiator\n");
				return alloc_len;
			}
			host = p->host;
			/* wildcard? */
			if (!strcasecmp(host, "[::]") || !strcasecmp(host, "0.0.0.0")) {
				if (spdk_sock_is_ipv6(conn->sock)) {
					snprintf(buf, sizeof buf, "[%s]", conn->target_addr);
					host = buf;
				} else if (spdk_sock_is_ipv4(conn->sock)) {
					snprintf(buf, sizeof buf, "%s", conn->target_addr);
					host = buf;
				} else {
					/* skip portal for the family */
					continue;
				}
			}
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "TargetAddress=%s:%s,%d\n",
				      host, p->port, pg->tag);
			len = snprintf((char *)data + total, alloc_len - total,
				       "TargetAddress=%s:%s,%d", host, p->port, pg->tag);
			total += len + 1;
		}
	}

	return total;
}

int
iscsi_send_tgts(struct spdk_iscsi_conn *conn, const char *iiqn,
		const char *tiqn, uint8_t *data, int alloc_len, int data_len)
{
	struct spdk_iscsi_tgt_node *target;
	int total;
	int len;
	int rc;

	if (conn == NULL) {
		return 0;
	}

	total = data_len;
	if (alloc_len < 1) {
		return 0;
	}
	if (total >= alloc_len) {
		total = alloc_len;
		data[total - 1] = '\0';
		return total;
	}

	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		if (strcasecmp(tiqn, "ALL") != 0
		    && strcasecmp(tiqn, target->name) != 0) {
			continue;
		}
		rc = iscsi_tgt_node_allow_iscsi_name(target, iiqn);
		if (rc == 0) {
			continue;
		}

		len = snprintf((char *)data + total, alloc_len - total, "TargetName=%s",
			       target->name);
		total += len + 1;

		total = iscsi_send_tgt_portals(conn, target, data, alloc_len, total);
		if (alloc_len - total < 1) {
			break;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);

	return total;
}

struct spdk_iscsi_tgt_node *
iscsi_find_tgt_node(const char *target_name)
{
	struct spdk_iscsi_tgt_node *target;

	if (target_name == NULL) {
		return NULL;
	}
	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		if (strcasecmp(target_name, target->name) == 0) {
			return target;
		}
	}
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "can't find target %s\n", target_name);
	return NULL;
}

static int
iscsi_tgt_node_register(struct spdk_iscsi_tgt_node *target)
{
	pthread_mutex_lock(&g_iscsi.mutex);

	if (iscsi_find_tgt_node(target->name) != NULL) {
		pthread_mutex_unlock(&g_iscsi.mutex);
		return -EEXIST;
	}

	TAILQ_INSERT_TAIL(&g_iscsi.target_head, target, tailq);

	pthread_mutex_unlock(&g_iscsi.mutex);
	return 0;
}

static int
iscsi_tgt_node_unregister(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_tgt_node *t;

	TAILQ_FOREACH(t, &g_iscsi.target_head, tailq) {
		if (t == target) {
			TAILQ_REMOVE(&g_iscsi.target_head, t, tailq);
			return 0;
		}
	}

	return -1;
}

static struct spdk_iscsi_ig_map *
iscsi_pg_map_find_ig_map(struct spdk_iscsi_pg_map *pg_map,
			 struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_ig_map *ig_map;

	TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
		if (ig_map->ig == ig) {
			return ig_map;
		}
	}

	return NULL;
}

static struct spdk_iscsi_ig_map *
iscsi_pg_map_add_ig_map(struct spdk_iscsi_pg_map *pg_map,
			struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_ig_map *ig_map;

	if (iscsi_pg_map_find_ig_map(pg_map, ig) != NULL) {
		return NULL;
	}

	ig_map = malloc(sizeof(*ig_map));
	if (ig_map == NULL) {
		return NULL;
	}

	ig_map->ig = ig;
	ig->ref++;
	pg_map->num_ig_maps++;
	TAILQ_INSERT_TAIL(&pg_map->ig_map_head, ig_map, tailq);

	return ig_map;
}

static void
_iscsi_pg_map_delete_ig_map(struct spdk_iscsi_pg_map *pg_map,
			    struct spdk_iscsi_ig_map *ig_map)
{
	TAILQ_REMOVE(&pg_map->ig_map_head, ig_map, tailq);
	pg_map->num_ig_maps--;
	ig_map->ig->ref--;
	free(ig_map);
}

static int
iscsi_pg_map_delete_ig_map(struct spdk_iscsi_pg_map *pg_map,
			   struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_ig_map *ig_map;

	ig_map = iscsi_pg_map_find_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		return -ENOENT;
	}

	_iscsi_pg_map_delete_ig_map(pg_map, ig_map);
	return 0;
}

static void
iscsi_pg_map_delete_all_ig_maps(struct spdk_iscsi_pg_map *pg_map)
{
	struct spdk_iscsi_ig_map *ig_map, *tmp;

	TAILQ_FOREACH_SAFE(ig_map, &pg_map->ig_map_head, tailq, tmp) {
		_iscsi_pg_map_delete_ig_map(pg_map, ig_map);
	}
}

static struct spdk_iscsi_pg_map *
iscsi_tgt_node_find_pg_map(struct spdk_iscsi_tgt_node *target,
			   struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_pg_map *pg_map;

	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		if (pg_map->pg == pg) {
			return pg_map;
		}
	}

	return NULL;
}

static struct spdk_iscsi_pg_map *
iscsi_tgt_node_add_pg_map(struct spdk_iscsi_tgt_node *target,
			  struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_pg_map *pg_map;
	char port_name[MAX_TMPBUF];
	int rc;

	if (iscsi_tgt_node_find_pg_map(target, pg) != NULL) {
		return NULL;
	}

	if (target->num_pg_maps >= SPDK_SCSI_DEV_MAX_PORTS) {
		SPDK_ERRLOG("Number of PG maps is more than allowed (max=%d)\n",
			    SPDK_SCSI_DEV_MAX_PORTS);
		return NULL;
	}

	pg_map = malloc(sizeof(*pg_map));
	if (pg_map == NULL) {
		return NULL;
	}

	snprintf(port_name, sizeof(port_name), "%s,t,0x%4.4x",
		 spdk_scsi_dev_get_name(target->dev), pg->tag);
	rc = spdk_scsi_dev_add_port(target->dev, pg->tag, port_name);
	if (rc != 0) {
		free(pg_map);
		return NULL;
	}

	TAILQ_INIT(&pg_map->ig_map_head);
	pg_map->num_ig_maps = 0;
	pg->ref++;
	pg_map->pg = pg;
	target->num_pg_maps++;
	TAILQ_INSERT_TAIL(&target->pg_map_head, pg_map, tailq);

	return pg_map;
}

static void
_iscsi_tgt_node_delete_pg_map(struct spdk_iscsi_tgt_node *target,
			      struct spdk_iscsi_pg_map *pg_map)
{
	TAILQ_REMOVE(&target->pg_map_head, pg_map, tailq);
	target->num_pg_maps--;
	pg_map->pg->ref--;

	spdk_scsi_dev_delete_port(target->dev, pg_map->pg->tag);

	free(pg_map);
}

static int
iscsi_tgt_node_delete_pg_map(struct spdk_iscsi_tgt_node *target,
			     struct spdk_iscsi_portal_grp *pg)
{
	struct spdk_iscsi_pg_map *pg_map;

	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		return -ENOENT;
	}

	if (pg_map->num_ig_maps > 0) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "delete %d ig_maps forcefully\n",
			      pg_map->num_ig_maps);
	}

	iscsi_pg_map_delete_all_ig_maps(pg_map);
	_iscsi_tgt_node_delete_pg_map(target, pg_map);
	return 0;
}

static void
iscsi_tgt_node_delete_ig_maps(struct spdk_iscsi_tgt_node *target,
			      struct spdk_iscsi_init_grp *ig)
{
	struct spdk_iscsi_pg_map *pg_map, *tmp;

	TAILQ_FOREACH_SAFE(pg_map, &target->pg_map_head, tailq, tmp) {
		iscsi_pg_map_delete_ig_map(pg_map, ig);
		if (pg_map->num_ig_maps == 0) {
			_iscsi_tgt_node_delete_pg_map(target, pg_map);
		}
	}
}

static void
iscsi_tgt_node_delete_all_pg_maps(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_pg_map *pg_map, *tmp;

	TAILQ_FOREACH_SAFE(pg_map, &target->pg_map_head, tailq, tmp) {
		iscsi_pg_map_delete_all_ig_maps(pg_map);
		_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}
}

static void
_iscsi_tgt_node_destruct(void *cb_arg, int rc)
{
	struct spdk_iscsi_tgt_node *target = cb_arg;
	iscsi_tgt_node_destruct_cb destruct_cb_fn = target->destruct_cb_fn;
	void *destruct_cb_arg = target->destruct_cb_arg;

	if (rc != 0) {
		if (destruct_cb_fn) {
			destruct_cb_fn(destruct_cb_arg, rc);
		}
		return;
	}

	pthread_mutex_lock(&g_iscsi.mutex);
	iscsi_tgt_node_delete_all_pg_maps(target);
	pthread_mutex_unlock(&g_iscsi.mutex);

	pthread_mutex_destroy(&target->mutex);
	free(target);

	if (destruct_cb_fn) {
		destruct_cb_fn(destruct_cb_arg, 0);
	}
}

static int
iscsi_tgt_node_check_active_conns(void *arg)
{
	struct spdk_iscsi_tgt_node *target = arg;

	if (iscsi_get_active_conns(target) != 0) {
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&target->destruct_poller);

	spdk_scsi_dev_destruct(target->dev, _iscsi_tgt_node_destruct, target);

	return SPDK_POLLER_BUSY;
}

static void
iscsi_tgt_node_destruct(struct spdk_iscsi_tgt_node *target,
			iscsi_tgt_node_destruct_cb cb_fn, void *cb_arg)
{
	if (target == NULL) {
		if (cb_fn) {
			cb_fn(cb_arg, -ENOENT);
		}
		return;
	}

	if (target->destructed) {
		SPDK_ERRLOG("Destructing %s is already started\n", target->name);
		if (cb_fn) {
			cb_fn(cb_arg, -EBUSY);
		}
		return;
	}

	target->destructed = true;
	target->destruct_cb_fn = cb_fn;
	target->destruct_cb_arg = cb_arg;

	iscsi_conns_request_logout(target, -1);

	if (iscsi_get_active_conns(target) != 0) {
		target->destruct_poller = SPDK_POLLER_REGISTER(iscsi_tgt_node_check_active_conns,
					  target, 10);
	} else {
		spdk_scsi_dev_destruct(target->dev, _iscsi_tgt_node_destruct, target);
	}

}

static int
iscsi_tgt_node_delete_pg_ig_map(struct spdk_iscsi_tgt_node *target,
				int pg_tag, int ig_tag)
{
	struct spdk_iscsi_portal_grp	*pg;
	struct spdk_iscsi_init_grp	*ig;
	struct spdk_iscsi_pg_map	*pg_map;
	struct spdk_iscsi_ig_map	*ig_map;

	pg = iscsi_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d not found\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig = iscsi_init_grp_find_by_tag(ig_tag);
	if (ig == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not found\n", target->name, ig_tag);
		return -ENOENT;
	}

	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d is not mapped\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig_map = iscsi_pg_map_find_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d is not mapped\n", target->name, pg_tag);
		return -ENOENT;
	}

	_iscsi_pg_map_delete_ig_map(pg_map, ig_map);
	if (pg_map->num_ig_maps == 0) {
		_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}

	return 0;
}

static int
iscsi_tgt_node_add_pg_ig_map(struct spdk_iscsi_tgt_node *target,
			     int pg_tag, int ig_tag)
{
	struct spdk_iscsi_portal_grp	*pg;
	struct spdk_iscsi_pg_map	*pg_map;
	struct spdk_iscsi_init_grp	*ig;
	struct spdk_iscsi_ig_map	*ig_map;
	bool				new_pg_map = false;

	pg = iscsi_portal_grp_find_by_tag(pg_tag);
	if (pg == NULL) {
		SPDK_ERRLOG("%s: PortalGroup%d not found\n", target->name, pg_tag);
		return -ENOENT;
	}
	ig = iscsi_init_grp_find_by_tag(ig_tag);
	if (ig == NULL) {
		SPDK_ERRLOG("%s: InitiatorGroup%d not found\n", target->name, ig_tag);
		return -ENOENT;
	}

	/* get existing pg_map or create new pg_map and add it to target */
	pg_map = iscsi_tgt_node_find_pg_map(target, pg);
	if (pg_map == NULL) {
		pg_map = iscsi_tgt_node_add_pg_map(target, pg);
		if (pg_map == NULL) {
			goto failed;
		}
		new_pg_map = true;
	}

	/* create new ig_map and add it to pg_map */
	ig_map = iscsi_pg_map_add_ig_map(pg_map, ig);
	if (ig_map == NULL) {
		goto failed;
	}

	return 0;

failed:
	if (new_pg_map) {
		_iscsi_tgt_node_delete_pg_map(target, pg_map);
	}

	return -1;
}

int
iscsi_target_node_add_pg_ig_maps(struct spdk_iscsi_tgt_node *target,
				 int *pg_tag_list, int *ig_tag_list, uint16_t num_maps)
{
	uint16_t i;
	int rc;

	pthread_mutex_lock(&g_iscsi.mutex);
	for (i = 0; i < num_maps; i++) {
		rc = iscsi_tgt_node_add_pg_ig_map(target, pg_tag_list[i],
						  ig_tag_list[i]);
		if (rc != 0) {
			SPDK_ERRLOG("could not add map to target\n");
			goto invalid;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return 0;

invalid:
	for (; i > 0; --i) {
		iscsi_tgt_node_delete_pg_ig_map(target, pg_tag_list[i - 1],
						ig_tag_list[i - 1]);
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return -1;
}

int
iscsi_target_node_remove_pg_ig_maps(struct spdk_iscsi_tgt_node *target,
				    int *pg_tag_list, int *ig_tag_list, uint16_t num_maps)
{
	uint16_t i;
	int rc;

	pthread_mutex_lock(&g_iscsi.mutex);
	for (i = 0; i < num_maps; i++) {
		rc = iscsi_tgt_node_delete_pg_ig_map(target, pg_tag_list[i],
						     ig_tag_list[i]);
		if (rc != 0) {
			SPDK_ERRLOG("could not delete map from target\n");
			goto invalid;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return 0;

invalid:
	for (; i > 0; --i) {
		rc = iscsi_tgt_node_add_pg_ig_map(target, pg_tag_list[i - 1],
						  ig_tag_list[i - 1]);
		if (rc != 0) {
			iscsi_tgt_node_delete_all_pg_maps(target);
			break;
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
	return -1;
}

static int
check_iscsi_name(const char *name)
{
	const unsigned char *up = (const unsigned char *) name;
	size_t n;

	/* valid iSCSI name no larger than 223 bytes */
	if (strlen(name) > MAX_TARGET_NAME) {
		return -1;
	}

	/* valid iSCSI name? */
	for (n = 0; up[n] != 0; n++) {
		if (up[n] > 0x00U && up[n] <= 0x2cU) {
			return -1;
		}
		if (up[n] == 0x2fU) {
			return -1;
		}
		if (up[n] >= 0x3bU && up[n] <= 0x40U) {
			return -1;
		}
		if (up[n] >= 0x5bU && up[n] <= 0x60U) {
			return -1;
		}
		if (up[n] >= 0x7bU && up[n] <= 0x7fU) {
			return -1;
		}
		if (isspace(up[n])) {
			return -1;
		}
	}
	/* valid format? */
	if (strncasecmp(name, "iqn.", 4) == 0) {
		/* iqn.YYYY-MM.reversed.domain.name */
		if (!isdigit(up[4]) || !isdigit(up[5]) || !isdigit(up[6])
		    || !isdigit(up[7]) || up[8] != '-' || !isdigit(up[9])
		    || !isdigit(up[10]) || up[11] != '.') {
			SPDK_ERRLOG("invalid iqn format. "
				    "expect \"iqn.YYYY-MM.reversed.domain.name\"\n");
			return -1;
		}
	} else if (strncasecmp(name, "eui.", 4) == 0) {
		/* EUI-64 -> 16bytes */
		/* XXX */
	} else if (strncasecmp(name, "naa.", 4) == 0) {
		/* 64bit -> 16bytes, 128bit -> 32bytes */
		/* XXX */
	}
	/* OK */
	return 0;
}

bool
iscsi_check_chap_params(bool disable, bool require, bool mutual, int group)
{
	if (group < 0) {
		SPDK_ERRLOG("Invalid auth group ID (%d)\n", group);
		return false;
	}
	if ((!disable && !require && !mutual) ||	/* Auto */
	    (disable && !require && !mutual) ||	/* None */
	    (!disable && require && !mutual) ||	/* CHAP */
	    (!disable && require && mutual)) {	/* CHAP Mutual */
		return true;
	}
	SPDK_ERRLOG("Invalid combination of CHAP params (d=%d,r=%d,m=%d)\n",
		    disable, require, mutual);
	return false;
}

struct spdk_iscsi_tgt_node *iscsi_tgt_node_construct(int target_index,
		const char *name, const char *alias,
		int *pg_tag_list, int *ig_tag_list, uint16_t num_maps,
		const char *bdev_name_list[], int *lun_id_list, int num_luns,
		int queue_depth,
		bool disable_chap, bool require_chap, bool mutual_chap, int chap_group,
		bool header_digest, bool data_digest)
{
	char				fullname[MAX_TMPBUF];
	struct spdk_iscsi_tgt_node	*target;
	int				rc;

	if (!iscsi_check_chap_params(disable_chap, require_chap,
				     mutual_chap, chap_group)) {
		return NULL;
	}

	if (num_maps == 0) {
		SPDK_ERRLOG("num_maps = 0\n");
		return NULL;
	}

	if (name == NULL) {
		SPDK_ERRLOG("TargetName not found\n");
		return NULL;
	}

	if (strncasecmp(name, "iqn.", 4) != 0
	    && strncasecmp(name, "eui.", 4) != 0
	    && strncasecmp(name, "naa.", 4) != 0) {
		snprintf(fullname, sizeof(fullname), "%s:%s", g_iscsi.nodebase, name);
	} else {
		snprintf(fullname, sizeof(fullname), "%s", name);
	}

	if (check_iscsi_name(fullname) != 0) {
		SPDK_ERRLOG("TargetName %s contains an invalid character or format.\n",
			    name);
		return NULL;
	}

	target = calloc(1, sizeof(*target));
	if (!target) {
		SPDK_ERRLOG("could not allocate target\n");
		return NULL;
	}

	rc = pthread_mutex_init(&target->mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("tgt_node%d: mutex_init() failed\n", target->num);
		iscsi_tgt_node_destruct(target, NULL, NULL);
		return NULL;
	}

	target->num = target_index;

	memcpy(target->name, fullname, strlen(fullname));

	if (alias != NULL) {
		if (strlen(alias) > MAX_TARGET_NAME) {
			iscsi_tgt_node_destruct(target, NULL, NULL);
			return NULL;
		}
		memcpy(target->alias, alias, strlen(alias));
	}

	target->dev = spdk_scsi_dev_construct(fullname, bdev_name_list, lun_id_list, num_luns,
					      SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI, NULL, NULL);
	if (!target->dev) {
		SPDK_ERRLOG("Could not construct SCSI device\n");
		iscsi_tgt_node_destruct(target, NULL, NULL);
		return NULL;
	}

	TAILQ_INIT(&target->pg_map_head);
	rc = iscsi_target_node_add_pg_ig_maps(target, pg_tag_list,
					      ig_tag_list, num_maps);
	if (rc != 0) {
		SPDK_ERRLOG("could not add map to target\n");
		iscsi_tgt_node_destruct(target, NULL, NULL);
		return NULL;
	}

	target->disable_chap = disable_chap;
	target->require_chap = require_chap;
	target->mutual_chap = mutual_chap;
	target->chap_group = chap_group;
	target->header_digest = header_digest;
	target->data_digest = data_digest;

	if (queue_depth > 0 && ((uint32_t)queue_depth <= g_iscsi.MaxQueueDepth)) {
		target->queue_depth = queue_depth;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "QueueDepth %d is invalid and %d is used instead.\n",
			      queue_depth, g_iscsi.MaxQueueDepth);
		target->queue_depth = g_iscsi.MaxQueueDepth;
	}

	rc = iscsi_tgt_node_register(target);
	if (rc != 0) {
		SPDK_ERRLOG("register target is failed\n");
		iscsi_tgt_node_destruct(target, NULL, NULL);
		return NULL;
	}

	return target;
}

static int
iscsi_parse_tgt_node(struct spdk_conf_section *sp)
{
	char buf[MAX_TMPBUF];
	struct spdk_iscsi_tgt_node *target;
	int pg_tag_list[MAX_TARGET_MAP], ig_tag_list[MAX_TARGET_MAP];
	int num_target_maps;
	const char *alias, *pg_tag, *ig_tag;
	const char *ag_tag;
	const char *val, *name;
	int target_num, chap_group, pg_tag_i, ig_tag_i;
	bool header_digest, data_digest;
	bool disable_chap, require_chap, mutual_chap;
	int i;
	int lun_id_list[SPDK_SCSI_DEV_MAX_LUN];
	const char *bdev_name_list[SPDK_SCSI_DEV_MAX_LUN];
	int num_luns, queue_depth;

	target_num = spdk_conf_section_get_num(sp);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "add unit %d\n", target_num);

	data_digest = false;
	header_digest = false;

	name = spdk_conf_section_get_val(sp, "TargetName");

	if (name == NULL) {
		SPDK_ERRLOG("tgt_node%d: TargetName not found\n", target_num);
		return -1;
	}

	alias = spdk_conf_section_get_val(sp, "TargetAlias");

	/* Setup initiator and portal group mapping */
	val = spdk_conf_section_get_val(sp, "Mapping");
	if (val == NULL) {
		/* no map */
		SPDK_ERRLOG("tgt_node%d: no Mapping\n", target_num);
		return -1;
	}

	for (i = 0; i < MAX_TARGET_MAP; i++) {
		val = spdk_conf_section_get_nmval(sp, "Mapping", i, 0);
		if (val == NULL) {
			break;
		}
		pg_tag = spdk_conf_section_get_nmval(sp, "Mapping", i, 0);
		ig_tag = spdk_conf_section_get_nmval(sp, "Mapping", i, 1);
		if (pg_tag == NULL || ig_tag == NULL) {
			SPDK_ERRLOG("tgt_node%d: mapping error\n", target_num);
			return -1;
		}
		if (strncasecmp(pg_tag, "PortalGroup",
				strlen("PortalGroup")) != 0
		    || sscanf(pg_tag, "%*[^0-9]%d", &pg_tag_i) != 1) {
			SPDK_ERRLOG("tgt_node%d: mapping portal error\n", target_num);
			return -1;
		}
		if (strncasecmp(ig_tag, "InitiatorGroup",
				strlen("InitiatorGroup")) != 0
		    || sscanf(ig_tag, "%*[^0-9]%d", &ig_tag_i) != 1) {
			SPDK_ERRLOG("tgt_node%d: mapping initiator error\n", target_num);
			return -1;
		}
		if (pg_tag_i < 1 || ig_tag_i < 1) {
			SPDK_ERRLOG("tgt_node%d: invalid group tag\n", target_num);
			return -1;
		}
		pg_tag_list[i] = pg_tag_i;
		ig_tag_list[i] = ig_tag_i;
	}

	num_target_maps = i;

	/* Setup AuthMethod */
	val = spdk_conf_section_get_val(sp, "AuthMethod");
	disable_chap = false;
	require_chap = false;
	mutual_chap = false;
	if (val != NULL) {
		for (i = 0; ; i++) {
			val = spdk_conf_section_get_nmval(sp, "AuthMethod", 0, i);
			if (val == NULL) {
				break;
			}
			if (strcasecmp(val, "CHAP") == 0) {
				require_chap = true;
			} else if (strcasecmp(val, "Mutual") == 0) {
				mutual_chap = true;
			} else if (strcasecmp(val, "Auto") == 0) {
				disable_chap = false;
				require_chap = false;
				mutual_chap = false;
			} else if (strcasecmp(val, "None") == 0) {
				disable_chap = true;
				require_chap = false;
				mutual_chap = false;
			} else {
				SPDK_ERRLOG("tgt_node%d: unknown auth\n", target_num);
				return -1;
			}
		}
		if (mutual_chap && !require_chap) {
			SPDK_ERRLOG("tgt_node%d: Mutual but not CHAP\n", target_num);
			return -1;
		}
	}
	if (disable_chap) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AuthMethod None\n");
	} else if (!require_chap) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AuthMethod Auto\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AuthMethod CHAP %s\n",
			      mutual_chap ? "Mutual" : "");
	}

	val = spdk_conf_section_get_val(sp, "AuthGroup");
	if (val == NULL) {
		chap_group = 0;
	} else {
		ag_tag = val;
		if (strcasecmp(ag_tag, "None") == 0) {
			chap_group = 0;
		} else {
			if (strncasecmp(ag_tag, "AuthGroup",
					strlen("AuthGroup")) != 0
			    || sscanf(ag_tag, "%*[^0-9]%d", &chap_group) != 1) {
				SPDK_ERRLOG("tgt_node%d: auth group error\n", target_num);
				return -1;
			}
			if (chap_group == 0) {
				SPDK_ERRLOG("tgt_node%d: invalid auth group 0\n", target_num);
				return -1;
			}
		}
	}
	if (chap_group == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AuthGroup None\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "AuthGroup AuthGroup%d\n", chap_group);
	}

	val = spdk_conf_section_get_val(sp, "UseDigest");
	if (val != NULL) {
		for (i = 0; ; i++) {
			val = spdk_conf_section_get_nmval(sp, "UseDigest", 0, i);
			if (val == NULL) {
				break;
			}
			if (strcasecmp(val, "Header") == 0) {
				header_digest = true;
			} else if (strcasecmp(val, "Data") == 0) {
				data_digest = true;
			} else if (strcasecmp(val, "Auto") == 0) {
				header_digest = false;
				data_digest = false;
			} else {
				SPDK_ERRLOG("tgt_node%d: unknown digest\n", target_num);
				return -1;
			}
		}
	}
	if (!header_digest && !data_digest) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "UseDigest Auto\n");
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "UseDigest %s %s\n",
			      header_digest ? "Header" : "",
			      data_digest ? "Data" : "");
	}

	val = spdk_conf_section_get_val(sp, "QueueDepth");
	if (val == NULL) {
		queue_depth = g_iscsi.MaxQueueDepth;
	} else {
		queue_depth = (int) strtol(val, NULL, 10);
	}

	num_luns = 0;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		snprintf(buf, sizeof(buf), "LUN%d", i);
		val = spdk_conf_section_get_val(sp, buf);
		if (val == NULL) {
			continue;
		}

		bdev_name_list[num_luns] = val;
		lun_id_list[num_luns] = i;
		num_luns++;
	}

	if (num_luns == 0) {
		SPDK_ERRLOG("tgt_node%d: No LUN specified for target %s.\n", target_num, name);
		return -1;
	}

	target = iscsi_tgt_node_construct(target_num, name, alias,
					  pg_tag_list, ig_tag_list, num_target_maps,
					  bdev_name_list, lun_id_list, num_luns, queue_depth,
					  disable_chap, require_chap, mutual_chap, chap_group,
					  header_digest, data_digest);

	if (target == NULL) {
		SPDK_ERRLOG("tgt_node%d: add_iscsi_target_node error\n", target_num);
		return -1;
	}

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(target->dev, i);

		if (lun) {
			SPDK_INFOLOG(SPDK_LOG_ISCSI, "device %d: LUN%d %s\n",
				     spdk_scsi_dev_get_id(target->dev),
				     spdk_scsi_lun_get_id(lun),
				     spdk_scsi_lun_get_bdev_name(lun));
		}
	}

	return 0;
}

int iscsi_parse_tgt_nodes(void)
{
	struct spdk_conf_section *sp;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "iscsi_parse_tgt_nodes\n");

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "TargetNode")) {
			int tag = spdk_conf_section_get_num(sp);

			if (tag > SPDK_TN_TAG_MAX) {
				SPDK_ERRLOG("tag %d is invalid\n", tag);
				return -1;
			}
			rc = iscsi_parse_tgt_node(sp);
			if (rc < 0) {
				SPDK_ERRLOG("spdk_iscsi_parse_tgt_node() failed\n");
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

void
iscsi_shutdown_tgt_nodes(void)
{
	struct spdk_iscsi_tgt_node *target;

	pthread_mutex_lock(&g_iscsi.mutex);
	while (!TAILQ_EMPTY(&g_iscsi.target_head)) {
		target = TAILQ_FIRST(&g_iscsi.target_head);
		TAILQ_REMOVE(&g_iscsi.target_head, target, tailq);

		pthread_mutex_unlock(&g_iscsi.mutex);

		iscsi_tgt_node_destruct(target, NULL, NULL);

		pthread_mutex_lock(&g_iscsi.mutex);
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
}

void
iscsi_shutdown_tgt_node_by_name(const char *target_name,
				iscsi_tgt_node_destruct_cb cb_fn, void *cb_arg)
{
	struct spdk_iscsi_tgt_node *target;

	pthread_mutex_lock(&g_iscsi.mutex);
	target = iscsi_find_tgt_node(target_name);
	if (target != NULL) {
		iscsi_tgt_node_unregister(target);
		pthread_mutex_unlock(&g_iscsi.mutex);

		iscsi_tgt_node_destruct(target, cb_fn, cb_arg);

		return;
	}
	pthread_mutex_unlock(&g_iscsi.mutex);

	if (cb_fn) {
		cb_fn(cb_arg, -ENOENT);
	}
}

bool
iscsi_tgt_node_is_destructed(struct spdk_iscsi_tgt_node *target)
{
	return target->destructed;
}

int
iscsi_tgt_node_cleanup_luns(struct spdk_iscsi_conn *conn,
			    struct spdk_iscsi_tgt_node *target)
{
	int i;
	struct spdk_iscsi_task *task;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(target->dev, i);

		if (!lun) {
			continue;
		}

		/* we create a fake management task per LUN to cleanup */
		task = iscsi_task_get(conn, NULL, iscsi_task_mgmt_cpl);
		if (!task) {
			SPDK_ERRLOG("Unable to acquire task\n");
			return -1;
		}

		task->scsi.target_port = conn->target_port;
		task->scsi.initiator_port = conn->initiator_port;
		task->scsi.lun = lun;

		iscsi_op_abort_task_set(task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
	}

	return 0;
}

void iscsi_tgt_node_delete_map(struct spdk_iscsi_portal_grp *portal_group,
			       struct spdk_iscsi_init_grp *initiator_group)
{
	struct spdk_iscsi_tgt_node *target;

	pthread_mutex_lock(&g_iscsi.mutex);
	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		if (portal_group) {
			iscsi_tgt_node_delete_pg_map(target, portal_group);
		}
		if (initiator_group) {
			iscsi_tgt_node_delete_ig_maps(target, initiator_group);
		}
	}
	pthread_mutex_unlock(&g_iscsi.mutex);
}

int
iscsi_tgt_node_add_lun(struct spdk_iscsi_tgt_node *target,
		       const char *bdev_name, int lun_id)
{
	struct spdk_scsi_dev *dev;
	int rc;

	if (target->num_active_conns > 0) {
		SPDK_ERRLOG("Target has active connections (count=%d)\n",
			    target->num_active_conns);
		return -1;
	}

	if (lun_id < -1 || lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
		SPDK_ERRLOG("Specified LUN ID (%d) is invalid\n", lun_id);
		return -1;
	}

	dev = target->dev;
	if (dev == NULL) {
		SPDK_ERRLOG("SCSI device is not found\n");
		return -1;
	}

	rc = spdk_scsi_dev_add_lun(dev, bdev_name, lun_id, NULL, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_scsi_dev_add_lun failed\n");
		return -1;
	}

	return 0;
}

int
iscsi_tgt_node_set_chap_params(struct spdk_iscsi_tgt_node *target,
			       bool disable_chap, bool require_chap,
			       bool mutual_chap, int32_t chap_group)
{
	if (!iscsi_check_chap_params(disable_chap, require_chap,
				     mutual_chap, chap_group)) {
		return -EINVAL;
	}

	pthread_mutex_lock(&target->mutex);
	target->disable_chap = disable_chap;
	target->require_chap = require_chap;
	target->mutual_chap = mutual_chap;
	target->chap_group = chap_group;
	pthread_mutex_unlock(&target->mutex);

	return 0;
}

static const char *target_nodes_section = \
		"\n"
		"# Users should change the TargetNode section(s) below to match the\n"
		"#  desired iSCSI target node configuration.\n"
		"# TargetName, Mapping, LUN0 are minimum required\n";

#define TARGET_NODE_TMPL \
"[TargetNode%d]\n" \
"  Comment \"Target%d\"\n" \
"  TargetName %s\n" \
"  TargetAlias \"%s\"\n"

#define TARGET_NODE_PGIG_MAPPING_TMPL \
"  Mapping PortalGroup%d InitiatorGroup%d\n"

#define TARGET_NODE_AUTH_TMPL \
"  AuthMethod %s\n" \
"  AuthGroup %s\n" \
"  UseDigest %s\n"

#define TARGET_NODE_QD_TMPL \
"  QueueDepth %d\n\n"

#define TARGET_NODE_LUN_TMPL \
"  LUN%d %s\n"

void
iscsi_tgt_nodes_config_text(FILE *fp)
{
	int l = 0;
	struct spdk_scsi_dev *dev = NULL;
	struct spdk_iscsi_tgt_node *target = NULL;
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;

	/* Create target nodes section */
	fprintf(fp, "%s", target_nodes_section);

	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		int idx;
		const char *authmethod = "None";
		char authgroup[32] = "None";
		const char *usedigest = "Auto";

		dev = target->dev;
		if (NULL == dev) { continue; }

		idx = target->num;
		fprintf(fp, TARGET_NODE_TMPL, idx, idx, target->name, spdk_scsi_dev_get_name(dev));

		TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
			TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
				fprintf(fp, TARGET_NODE_PGIG_MAPPING_TMPL,
					pg_map->pg->tag,
					ig_map->ig->tag);
			}
		}

		if (target->disable_chap) {
			authmethod = "None";
		} else if (!target->require_chap) {
			authmethod = "Auto";
		} else if (target->mutual_chap) {
			authmethod = "CHAP Mutual";
		} else {
			authmethod = "CHAP";
		}

		if (target->chap_group > 0) {
			snprintf(authgroup, sizeof(authgroup), "AuthGroup%d", target->chap_group);
		}

		if (target->header_digest) {
			usedigest = "Header";
		} else if (target->data_digest) {
			usedigest = "Data";
		}

		fprintf(fp, TARGET_NODE_AUTH_TMPL,
			authmethod, authgroup, usedigest);

		for (l = 0; l < SPDK_SCSI_DEV_MAX_LUN; l++) {
			struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(dev, l);

			if (!lun) {
				continue;
			}

			fprintf(fp, TARGET_NODE_LUN_TMPL,
				spdk_scsi_lun_get_id(lun),
				spdk_scsi_lun_get_bdev_name(lun));
		}

		fprintf(fp, TARGET_NODE_QD_TMPL,
			target->queue_depth);
	}
}

static void
iscsi_tgt_node_info_json(struct spdk_iscsi_tgt_node *target,
			 struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_pg_map *pg_map;
	struct spdk_iscsi_ig_map *ig_map;
	int i;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", target->name);

	if (target->alias[0] != '\0') {
		spdk_json_write_named_string(w, "alias_name", target->alias);
	}

	spdk_json_write_named_array_begin(w, "pg_ig_maps");
	TAILQ_FOREACH(pg_map, &target->pg_map_head, tailq) {
		TAILQ_FOREACH(ig_map, &pg_map->ig_map_head, tailq) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_int32(w, "pg_tag", pg_map->pg->tag);
			spdk_json_write_named_int32(w, "ig_tag", ig_map->ig->tag);
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_array_begin(w, "luns");
	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(target->dev, i);

		if (lun) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));
			spdk_json_write_named_int32(w, "lun_id", spdk_scsi_lun_get_id(lun));
			spdk_json_write_object_end(w);
		}
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_int32(w, "queue_depth", target->queue_depth);

	spdk_json_write_named_bool(w, "disable_chap", target->disable_chap);
	spdk_json_write_named_bool(w, "require_chap", target->require_chap);
	spdk_json_write_named_bool(w, "mutual_chap", target->mutual_chap);
	spdk_json_write_named_int32(w, "chap_group", target->chap_group);

	spdk_json_write_named_bool(w, "header_digest", target->header_digest);
	spdk_json_write_named_bool(w, "data_digest", target->data_digest);

	spdk_json_write_object_end(w);
}

static void
iscsi_tgt_node_config_json(struct spdk_iscsi_tgt_node *target,
			   struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "iscsi_create_target_node");

	spdk_json_write_name(w, "params");
	iscsi_tgt_node_info_json(target, w);

	spdk_json_write_object_end(w);
}

void
iscsi_tgt_nodes_info_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_tgt_node *target;

	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		iscsi_tgt_node_info_json(target, w);
	}
}

void
iscsi_tgt_nodes_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_iscsi_tgt_node *target;

	TAILQ_FOREACH(target, &g_iscsi.target_head, tailq) {
		iscsi_tgt_node_config_json(target, w);
	}
}
