/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Manipulation functions for IPVS & IPFW wrappers.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2012 Alexandre Cassen, <acassen@gmail.com>
 */

#include "ipwrapper.h"
#include "ipvswrapper.h"
#include "logger.h"
#include "memory.h"
#include "utils.h"
#include "notify.h"
#include "main.h"
#ifdef _WITH_SNMP_
  #include "check_snmp.h"
#endif

/* Returns the sum of all RS weight in a virtual server. */
long unsigned
weigh_live_realservers(virtual_server * vs)
{
	element e;
	real_server *svr;
	long unsigned count = 0;

	for (e = LIST_HEAD(vs->rs); e; ELEMENT_NEXT(e)) {
		svr = ELEMENT_DATA(e);
		if (ISALIVE(svr))
			count += svr->weight;
	}
	return count;
}

/* Remove a realserver IPVS rule */
static int
clear_service_rs(list vs_group, virtual_server * vs, list l)
{
	element e;
	real_server *rs;
	char rsip[INET6_ADDRSTRLEN];

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		rs = ELEMENT_DATA(e);
		if (ISALIVE(rs)) {
			if (!ipvs_cmd(LVS_CMD_DEL_DEST, vs_group, vs, rs))
				return 0;
			UNSET_ALIVE(rs);
			if (!vs->omega)
				continue;

			/* In Omega mode we call VS and RS down notifiers
			 * all the way down the exit, as necessary.
			 */
			if (rs->notify_down) {
				log_message(LOG_INFO, "Executing [%s] for service [%s]:%d in VS [%s]:%d"
						    , rs->notify_down
						    , inet_sockaddrtos2(&rs->addr, rsip)
						    , ntohs(inet_sockaddrport(&rs->addr))
						    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
						    , ntohs(inet_sockaddrport(&vs->addr)));
				notify_exec(rs->notify_down);
			}
#ifdef _WITH_SNMP_
			check_snmp_rs_trap(rs, vs);
#endif

			/* Sooner or later VS will lose the quorum (if any). However,
			 * we don't push in a sorry server then, hence the regression
			 * is intended.
			 */
			if (vs->quorum_state == UP &&
			    weigh_live_realservers(vs) < vs->quorum - vs->hysteresis) {
				vs->quorum_state = DOWN;
				if (vs->quorum_down) {
					log_message(LOG_INFO, "Executing [%s] for VS [%s]:%d"
							    , vs->quorum_down
							    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
							    , ntohs(inet_sockaddrport(&vs->addr)));
					notify_exec(vs->quorum_down);
				}
#ifdef _WITH_SNMP_
				check_snmp_quorum_trap(vs);
#endif
			}
		}
	}

	return 1;
}

/* Remove a virtualserver IPVS rule */
static int
clear_service_vs(list vs_group, virtual_server * vs)
{
	/* Processing real server queue */
	if (!LIST_ISEMPTY(vs->rs)) {
		if (vs->s_svr) {
			if (ISALIVE(vs->s_svr))
				if (!ipvs_cmd(LVS_CMD_DEL_DEST, vs_group, vs, vs->s_svr))
					return 0;
		} else if (!clear_service_rs(vs_group, vs, vs->rs))
			return 0;
		/* The above will handle Omega case for VS as well. */
	}

	if (!ipvs_cmd(LVS_CMD_DEL, vs_group, vs, NULL))
		return 0;

	UNSET_ALIVE(vs);
	return 1;
}

/* IPVS cleaner processing */
int
clear_services(void)
{
	element e;
	list l = check_data->vs;
	virtual_server *vs;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vs = ELEMENT_DATA(e);
		if (!clear_service_vs(check_data->vs_group, vs))
			return 0;
	}
	return 1;
}

/* Set a realserver IPVS rules */
static int
init_service_rs(virtual_server * vs)
{
	element e;
	real_server *rs;

	for (e = LIST_HEAD(vs->rs); e; ELEMENT_NEXT(e)) {
		rs = ELEMENT_DATA(e);
		/* In alpha mode, be pessimistic (or realistic?) and don't
		 * add real servers into the VS pool. They will get there
		 * later upon healthchecks recovery (if ever).
		 */
		if (vs->alpha) {
			UNSET_ALIVE(rs);
			continue;
		}
		if (!ISALIVE(rs)) {
			if (!ipvs_cmd(LVS_CMD_ADD_DEST, check_data->vs_group, vs, rs))
				return 0;
			else
				SET_ALIVE(rs);
		} else if (vs->vsgname) {
			UNSET_ALIVE(rs);
			if (!ipvs_cmd(LVS_CMD_ADD_DEST, check_data->vs_group, vs, rs))
				return 0;
			SET_ALIVE(rs);
		}
	}

	return 1;
}

/* Set a virtualserver IPVS rules */
static int
init_service_vs(virtual_server * vs)
{
	/* Init the VS root */
	if (!ISALIVE(vs) || vs->vsgname) {
		if (!ipvs_cmd(LVS_CMD_ADD, check_data->vs_group, vs, NULL))
			return 0;
		else
			SET_ALIVE(vs);
	}

	/* Processing real server queue */
	if (!LIST_ISEMPTY(vs->rs)) {
		if (vs->alpha)
			vs->quorum_state = DOWN;
		if (!init_service_rs(vs))
			return 0;
	}
	return 1;
}

/* Set IPVS rules */
int
init_services(void)
{
	element e;
	list l = check_data->vs;
	virtual_server *vs;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vs = ELEMENT_DATA(e);
		if (!init_service_vs(vs))
			return 0;
	}
	return 1;
}

/* add or remove _alive_ real servers from a virtual server */
void
perform_quorum_state(virtual_server *vs, int add)
{
	element e;
	real_server *rs;

	if (LIST_ISEMPTY(vs->rs))
		return;

	log_message(LOG_INFO, "%s the pool for VS [%s]:%d"
			    , add?"Adding alive servers to":"Removing alive servers from"
			    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
			    , ntohs(inet_sockaddrport(&vs->addr)));
	for (e = LIST_HEAD(vs->rs); e; ELEMENT_NEXT(e)) {
		rs = ELEMENT_DATA(e);
		if (!ISALIVE(rs)) /* We only handle alive servers */
			continue;
		if (add)
			rs->alive = 0;
		ipvs_cmd(add?LVS_CMD_ADD_DEST:LVS_CMD_DEL_DEST, check_data->vs_group, vs, rs);
		rs->alive = 1;
	}
}

/* set quorum state depending on current weight of real servers */
void
update_quorum_state(virtual_server * vs)
{
	char rsip[INET6_ADDRSTRLEN];

	/* If we have just gained quorum, it's time to consider notify_up. */
	if (vs->quorum_state == DOWN &&
	    weigh_live_realservers(vs) >= vs->quorum + vs->hysteresis) {
		vs->quorum_state = UP;
		log_message(LOG_INFO, "Gained quorum %lu+%lu=%lu <= %u for VS [%s]:%d"
				    , vs->quorum
				    , vs->hysteresis
				    , vs->quorum + vs->hysteresis
				    , weigh_live_realservers(vs)
				    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
				    , ntohs(inet_sockaddrport(&vs->addr)));
		if (vs->s_svr && ISALIVE(vs->s_svr)) {
			log_message(LOG_INFO, "Removing sorry server [%s]:%d from VS [%s]:%d"
					    , inet_sockaddrtos2(&vs->s_svr->addr, rsip)
					    , ntohs(inet_sockaddrport(&vs->s_svr->addr))
					    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
					    , ntohs(inet_sockaddrport(&vs->addr)));

			ipvs_cmd(LVS_CMD_DEL_DEST, check_data->vs_group, vs, vs->s_svr);
			vs->s_svr->alive = 0;

			/* Adding back alive real servers */
			perform_quorum_state(vs, 1);
		}
		if (vs->quorum_up) {
			log_message(LOG_INFO, "Executing [%s] for VS [%s]:%d"
					    , vs->quorum_up
					    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
					    , ntohs(inet_sockaddrport(&vs->addr)));
			notify_exec(vs->quorum_up);
		}
#ifdef _WITH_SNMP_
               check_snmp_quorum_trap(vs);
#endif
		return;
	}

	/* If we have just lost quorum for the VS, we need to consider
	 * VS notify_down and sorry_server cases
	 */
	if (vs->quorum_state == UP &&
	    weigh_live_realservers(vs) < vs->quorum - vs->hysteresis) {
		vs->quorum_state = DOWN;
		log_message(LOG_INFO, "Lost quorum %lu-%lu=%lu > %u for VS [%s]:%d"
				    , vs->quorum
				    , vs->hysteresis
				    , vs->quorum - vs->hysteresis
				    , weigh_live_realservers(vs)
				    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
				    , ntohs(inet_sockaddrport(&vs->addr)));
		if (vs->quorum_down) {
			log_message(LOG_INFO, "Executing [%s] for VS [%s]:%d"
					    , vs->quorum_down
					    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
					    , ntohs(inet_sockaddrport(&vs->addr)));
			notify_exec(vs->quorum_down);
		}
		if (vs->s_svr) {
			log_message(LOG_INFO, "Adding sorry server [%s]:%d to VS [%s]:%d"
					    , inet_sockaddrtos2(&vs->s_svr->addr, rsip)
					    , ntohs(inet_sockaddrport(&vs->s_svr->addr))
					    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
					    , ntohs(inet_sockaddrport(&vs->addr)));

			/* the sorry server is now up in the pool, we flag it alive */
			ipvs_cmd(LVS_CMD_ADD_DEST, check_data->vs_group, vs, vs->s_svr);
			vs->s_svr->alive = 1;

			/* Remove remaining alive real servers */
			perform_quorum_state(vs, 0);
		}
#ifdef _WITH_SNMP_
		check_snmp_quorum_trap(vs);
#endif
		return;
	}
}

/* manipulate add/remove rs according to alive state */
void
perform_svr_state(int alive, virtual_server * vs, real_server * rs)
{
	char rsip[INET6_ADDRSTRLEN];

	/*
	 * | ISALIVE(rs) | alive | context
	 * | 0           | 0     | first check failed under alpha mode, unreachable here
	 * | 0           | 1     | RS went up, add it to the pool
	 * | 1           | 0     | RS went down, remove it from the pool
	 * | 1           | 1     | first check succeeded w/o alpha mode, unreachable here
	 */
	if (!ISALIVE(rs) && alive) {
		log_message(LOG_INFO, "%s service [%s]:%d to VS [%s]:%d"
				    , (rs->inhibit) ? "Enabling" : "Adding"
				    , inet_sockaddrtos2(&rs->addr, rsip)
				    , ntohs(inet_sockaddrport(&rs->addr))
				    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
				    , ntohs(inet_sockaddrport(&vs->addr)));
		/* Add only if we have quorum or no sorry server */
		if (vs->quorum_state == UP || !vs->s_svr || !ISALIVE(vs->s_svr)) {
			ipvs_cmd(LVS_CMD_ADD_DEST, check_data->vs_group, vs, rs);
		}
		rs->alive = alive;
		if (rs->notify_up) {
			log_message(LOG_INFO, "Executing [%s] for service [%s]:%d in VS [%s]:%d"
					    , rs->notify_up
					    , inet_sockaddrtos2(&rs->addr, rsip)
					    , ntohs(inet_sockaddrport(&rs->addr))
					    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
					    , ntohs(inet_sockaddrport(&vs->addr)));
			notify_exec(rs->notify_up);
		}
#ifdef _WITH_SNMP_
		check_snmp_rs_trap(rs, vs);
#endif

		/* We may have gained quorum */
		update_quorum_state(vs);
	}

	if (ISALIVE(rs) && !alive) {
		log_message(LOG_INFO, "%s service [%s]:%d from VS [%s]:%d"
				    , (rs->inhibit) ? "Disabling" : "Removing"
				    , inet_sockaddrtos2(&rs->addr, rsip)
				    , ntohs(inet_sockaddrport(&rs->addr))
				    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
				    , ntohs(inet_sockaddrport(&vs->addr)));

		/* server is down, it is removed from the LVS realserver pool
		 * Remove only if we have quorum or no sorry server
		 */
		if (vs->quorum_state == UP || !vs->s_svr || !ISALIVE(vs->s_svr)) {
			ipvs_cmd(LVS_CMD_DEL_DEST, check_data->vs_group, vs, rs);
		}
		rs->alive = alive;
		if (rs->notify_down) {
			log_message(LOG_INFO, "Executing [%s] for service [%s]:%d in VS [%s]:%d"
					    , rs->notify_down
					    , inet_sockaddrtos2(&rs->addr, rsip)
					    , ntohs(inet_sockaddrport(&rs->addr))
					    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
					    , ntohs(inet_sockaddrport(&vs->addr)));
			notify_exec(rs->notify_down);
		}
#ifdef _WITH_SNMP_
		check_snmp_rs_trap(rs, vs);
#endif

		/* We may have lost quorum */
		update_quorum_state(vs);
	}
}

/* Store new weight in real_server struct and then update kernel. */
void
update_svr_wgt(int weight, virtual_server * vs, real_server * rs)
{
	char rsip[INET6_ADDRSTRLEN];

	if (weight != rs->weight) {
		log_message(LOG_INFO, "Changing weight from %d to %d for %s service [%s]:%d of VS [%s]:%d"
				    , rs->weight
				    , weight
				    , ISALIVE(rs) ? "active" : "inactive"
				    , inet_sockaddrtos2(&rs->addr, rsip)
				    , ntohs(inet_sockaddrport(&rs->addr))
				    , (vs->vsgname) ? vs->vsgname : inet_sockaddrtos(&vs->addr)
				    , ntohs(inet_sockaddrport(&vs->addr)));
		rs->weight = weight;
		/*
		 * Have weight change take effect now only if rs is in
		 * the pool and alive and the quorum is met (or if
		 * there is no sorry server). If not, it will take
		 * effect later when it becomes alive.
		 */
		if (rs->set && ISALIVE(rs) &&
		    (vs->quorum_state == UP || !vs->s_svr || !ISALIVE(vs->s_svr)))
			ipvs_cmd(LVS_CMD_EDIT_DEST, check_data->vs_group, vs, rs);
		update_quorum_state(vs);
	}
}

/* Test if realserver is marked UP for a specific checker */
int
svr_checker_up(checker_id_t cid, real_server *rs)
{
	element e;
	list l = rs->failed_checkers;
	checker_id_t *id;

	/*
	 * We assume there is not too much checker per
	 * real server, so we consider this lookup as
	 * o(1).
	 */
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		id = ELEMENT_DATA(e);
		if (*id == cid)
			return 0;
	}

	return 1;
}

/* Update checker's state */
void
update_svr_checker_state(int alive, checker_id_t cid, virtual_server *vs, real_server *rs)
{
	element e;
	list l = rs->failed_checkers;
	checker_id_t *id;

	/* Handle alive state. Depopulate failed_checkers and call
	 * perform_svr_state() independently, letting the latter sort
	 * things out itself.
	 */
	if (alive) {
		/* Remove the succeeded check from failed_checkers list. */
		for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
			id = ELEMENT_DATA(e);
			if (*id == cid) {
				free_list_element(l, e);
				/* If we don't break, the next iteration will trigger
				 * a SIGSEGV.
				 */
				break;
			}
		}
		if (LIST_SIZE(l) == 0)
			perform_svr_state(alive, vs, rs);
	}
	/* Handle not alive state */
	else {
		id = (checker_id_t *) MALLOC(sizeof(checker_id_t));
		*id = cid;
		list_add(l, id);
		if (LIST_SIZE(l) == 1)
			perform_svr_state(alive, vs, rs);
	}
}

/* Check if a vsg entry is in new data */
static int
vsge_exist(virtual_server_group_entry *vsg_entry, list l)
{
	element e;
	virtual_server_group_entry *vsge;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsge = ELEMENT_DATA(e);
		if (VSGE_ISEQ(vsg_entry, vsge)) {
			/*
			 * If vsge exist this entry
			 * is alive since only rs entries
			 * are changing from alive state.
			 */
			SET_ALIVE(vsge);
			return 1;
		}
	}

	return 0;
}

/* Clear the diff vsge of old group */
static int
clear_diff_vsge(list old, list new, virtual_server * old_vs)
{
	virtual_server_group_entry *vsge;
	element e;

	for (e = LIST_HEAD(old); e; ELEMENT_NEXT(e)) {
		vsge = ELEMENT_DATA(e);
		if (!vsge_exist(vsge, new)) {
			log_message(LOG_INFO, "VS [[%s]:%d:%d:%d] in group %s no longer exist" 
					    , inet_sockaddrtos(&vsge->addr)
					    , ntohs(inet_sockaddrport(&vsge->addr))
					    , vsge->range
					    , vsge->vfwmark
					    , old_vs->vsgname);

			if (!ipvs_group_remove_entry(old_vs, vsge))
				return 0;
		}
	}

	return 1;
}

/* Clear the diff vsg of the old vs */
static int
clear_diff_vsg(virtual_server * old_vs)
{
	virtual_server_group *old;
	virtual_server_group *new;

	/* Fetch group */
	old = ipvs_get_group_by_name(old_vs->vsgname, old_check_data->vs_group);
	new = ipvs_get_group_by_name(old_vs->vsgname, check_data->vs_group);

	/* Diff the group entries */
	if (!clear_diff_vsge(old->addr_ip, new->addr_ip, old_vs))
		return 0;
	if (!clear_diff_vsge(old->range, new->range, old_vs))
		return 0;
	if (!clear_diff_vsge(old->vfwmark, new->vfwmark, old_vs))
		return 0;

	return 1;
}

/* Check if a vs exist in new data */
static int
vs_exist(virtual_server * old_vs)
{
	element e;
	list l = check_data->vs;
	virtual_server *vs;
	virtual_server_group *vsg;

	if (LIST_ISEMPTY(l))
		return 0;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vs = ELEMENT_DATA(e);
		if (VS_ISEQ(old_vs, vs)) {
			/* Check if group exist */
			if (vs->vsgname) {
				vsg = ipvs_get_group_by_name(old_vs->vsgname,
							    check_data->vs_group);
				if (!vsg)
					return 0;
				else
					if (!clear_diff_vsg(old_vs))
						return 0;	
			}

			/*
			 * Exist so set alive.
			 */
			SET_ALIVE(vs);
			return 1;
		}
	}

	return 0;
}

/* Check if rs is in new vs data */
static int
rs_exist(real_server * old_rs, list l)
{
	element e;
	real_server *rs;

	if (LIST_ISEMPTY(l))
		return 0;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		rs = ELEMENT_DATA(e);
		if (RS_ISEQ(rs, old_rs)) {
			/*
			 * We reflect the previous alive
			 * flag value to not try to set
			 * already set IPVS rule.
			 */
			rs->alive = old_rs->alive;
			rs->set = old_rs->set;
			rs->weight = old_rs->weight;
			return 1;
		}
	}

	return 0;
}

/* get rs list for a specific vs */
static list
get_rs_list(virtual_server * vs)
{
	element e;
	list l = check_data->vs;
	virtual_server *vsvr;

	if (LIST_ISEMPTY(l))
		return NULL;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsvr = ELEMENT_DATA(e);
		if (VS_ISEQ(vs, vsvr))
			return vsvr->rs;
	}

	/* most of the time never reached */
	return NULL;
}

/* Clear the diff rs of the old vs */
static int
clear_diff_rs(virtual_server * old_vs)
{
	element e;
	list l = old_vs->rs;
	list new = get_rs_list(old_vs);
	real_server *rs;
	char rsip[INET6_ADDRSTRLEN];

	/* If old vs didn't own rs then nothing return */
	if (LIST_ISEMPTY(l))
		return 1;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		rs = ELEMENT_DATA(e);
		if (!rs_exist(rs, new)) {
			/* Reset inhibit flag to delete inhibit entries */
			log_message(LOG_INFO, "service [%s]:%d no longer exist"
					    , inet_sockaddrtos(&rs->addr)
					    , ntohs(inet_sockaddrport(&rs->addr)));
			log_message(LOG_INFO, "Removing service [%s]:%d from VS [%s]:%d"
					    , inet_sockaddrtos2(&rs->addr, rsip)
					    , ntohs(inet_sockaddrport(&rs->addr))
					    , (old_vs->vsgname) ? old_vs->vsgname : inet_sockaddrtos(&old_vs->addr)
					    , ntohs(inet_sockaddrport(&old_vs->addr)));
			rs->inhibit = 0;
			if (!ipvs_cmd(LVS_CMD_DEL_DEST, check_data->vs_group, old_vs, rs))
				return 0;
		}
	}

	return 1;
}

/* When reloading configuration, remove negative diff entries */
int
clear_diff_services(void)
{
	element e;
	list l = old_check_data->vs;
	virtual_server *vs;

	/* If old config didn't own vs then nothing return */
	if (LIST_ISEMPTY(l))
		return 1;

	/* Remove diff entries from previous IPVS rules */
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vs = ELEMENT_DATA(e);

		/*
		 * Try to find this vs into the new conf data
		 * reloaded.
		 */
		if (!vs_exist(vs)) {
			if (vs->vsgname)
				log_message(LOG_INFO, "Removing Virtual Server Group [%s]"
						    , vs->vsgname);
			else
				log_message(LOG_INFO, "Removing Virtual Server [%s]:%d"
						    , inet_sockaddrtos(&vs->addr)
						    , ntohs(inet_sockaddrport(&vs->addr)));

			/* Clear VS entry */
			if (!clear_service_vs(old_check_data->vs_group, vs))
				return 0;
		} else {
			/* If vs exist, perform rs pool diff */
			if (!clear_diff_rs(vs))
				return 0;
			if (vs->s_svr)
				if (ISALIVE(vs->s_svr))
					if (!ipvs_cmd(LVS_CMD_DEL_DEST
						      , check_data->vs_group
						      , vs
						      , vs->s_svr))
						return 0;
		}
	}

	return 1;
}
