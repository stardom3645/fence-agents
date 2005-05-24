/******************************************************************************
*******************************************************************************
**
**  Copyright (C) 2005 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include "lock_dlm.h"

#define LOCK_DLM_SYSFS_DIR	("/sys/kernel/lock_dlm")

extern char *clustername;
extern int our_nodeid;
extern group_handle_t gh;

struct list_head mounts;


int set_sysfs(struct mountgroup *mg, char *field, int val)
{
	char fname[512];
	char out[16];
	int rv, fd;

	snprintf(fname, 512, "%s/%s/%s", LOCK_DLM_SYSFS_DIR, mg->name, field);

	log_group(mg, "set %s to %d", fname, val);

	fd = open(fname, O_RDWR);
	if (fd < 0) {
		log_error("open error %s %d %d", fname, fd, errno);
		return -1;
	}

	memset(out, 0, 16);
	sprintf(out, "%d", val);
	rv = write(fd, out, strlen(out));

	if (rv != strlen(out))
		log_error("write error %s %d %d", fname, fd, errno);

	close(fd);
	return 0;
}

int get_sysfs(struct mountgroup *mg, char *field, char *buf, int len)
{
	char fname[512], *p;
	int fd, rv;

	snprintf(fname, 512, "%s/%s/%s", LOCK_DLM_SYSFS_DIR, mg->name, field);

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		log_error("open error %s %d %d\n", fname, fd, errno);
		return -1;
	}

	rv = read(fd, buf, len);
	if (rv < 0)
		log_error("read error %s %d %d", fname, rv, errno);
	else {
		rv = 0;
		p = strchr(buf, '\n');
		if (p)
			*p = '\0';
	}

	close(fd);
	return rv;
}

int claim_journal(struct mountgroup *mg)
{
	mg->our_jid = our_nodeid - 1;
	return 0;
}

int discover_journals(struct mountgroup *mg)
{
	struct mg_member *memb;

	list_for_each_entry(memb, &mg->members, list)
		memb->jid = memb->nodeid - 1;
	return 0;
}

int recover_journals(struct mountgroup *mg)
{
	struct mg_member *memb;
	int rv, found = 0;

	list_for_each_entry(memb, &mg->members, list) {
		if (memb->recover_journal) {
			rv = set_sysfs(mg, "recover", memb->jid);
			if (rv < 0)
				break;
			memb->recover_journal = 0;
			memb->wait_recover_done = 1;
			found = 1;
			break;
		}
	}
	return found;
}

static void add_ordered_member(struct mountgroup *mg, struct mg_member *new)
{
	struct mg_member *memb = NULL;
	struct list_head *tmp;
	struct list_head *newlist = &new->list;
	struct list_head *head = &mg->members;

	list_for_each(tmp, head) {
		memb = list_entry(tmp, struct mg_member, list);
		if (new->nodeid < memb->nodeid)
			break;
	}

	if (!memb)
		list_add_tail(newlist, head);
	else {
		/* FIXME: can use list macro here */
		newlist->prev = tmp->prev;
		newlist->next = tmp;
		tmp->prev->next = newlist;
		tmp->prev = newlist;
	}
}

int add_member(struct mountgroup *mg, int nodeid)
{
	struct mg_member *memb;
	char buf[MAXLINE];
	int rv;

	memb = malloc(sizeof(struct mg_member));
	if (!memb)
		return -ENOMEM;

	memset(memb, 0, sizeof(*memb));
	memset(buf, 0, sizeof(buf));

	rv = group_join_info(GFS_GROUP_LEVEL, mg->name, nodeid, buf);
	if (!rv && strstr(buf, "spectator"))
		memb->spectator = 1;

	memb->nodeid = nodeid;
	add_ordered_member(mg, memb);
	mg->num_memb++;
	return 0;
}

void remove_member(struct mountgroup *mg, struct mg_member *memb)
{
	list_move(&memb->list, &mg->members_gone);
	memb->gone_event = mg->start_event_nr;
	mg->num_memb--;
}

int is_member(struct mountgroup *mg, int nodeid)
{
	struct mg_member *memb;

	list_for_each_entry(memb, &mg->members, list) {
		if (memb->nodeid == nodeid)
			return TRUE;
	}
	return FALSE;
}

int is_removed(struct mountgroup *mg, int nodeid)
{
	struct mg_member *memb;

	list_for_each_entry(memb, &mg->members_gone, list) {
		if (memb->nodeid == nodeid)
			return TRUE;
	}
	return FALSE;
}

static void clear_memb_list(struct list_head *head)
{
	struct mg_member *memb;

	while (!list_empty(head)) {
		memb = list_entry(head->next, struct mg_member, list);
		list_del(&memb->list);
		free(memb);
	}
}

void clear_members(struct mountgroup *mg)
{
	clear_memb_list(&mg->members);
	mg->num_memb = 0;
}

void clear_members_gone(struct mountgroup *mg)
{
	clear_memb_list(&mg->members_gone);
}

void clear_members_finish(struct mountgroup *mg, int finish_event)
{
	struct mg_member *memb, *safe;

	list_for_each_entry_safe(memb, safe, &mg->members_gone, list) {
		if (memb->gone_event <= finish_event) {
			list_del(&memb->list);
			free(memb);
		}
	}
}

void mount_finished(struct mountgroup *mg)
{
	struct mg_member *memb;

	list_for_each_entry(memb, &mg->members, list)
		memb->mount_finished = 1;
}

void recover_members(struct mountgroup *mg, int num_nodes,
 		     int *nodeids, int *pos_out, int *neg_out)
{
	struct mg_member *memb, *safe;
	int i, found, id, pos = 0, neg = 0, low = -1;

	/* move departed nodes from members list to members_gone */

	list_for_each_entry_safe(memb, safe, &mg->members, list) {
		found = FALSE;
		for (i = 0; i < num_nodes; i++) {
			if (memb->nodeid == nodeids[i]) {
				found = TRUE;
				break;
			}
		}

		if (!found) {
			neg++;
			remove_member(mg, memb);

			/* - spectators don't do journal callbacks
			   - journal cb only for failed (not leaving) nodes
			   - journal cb only if failed node finished joining
			   - no journal cb if failed node was spectator
			   - no journal cb if we've already done a journl cb */

			if (!mg->spectator &&
			    mg->start_type == GROUP_NODE_FAILED &&
			    memb->mount_finished &&
			    !memb->spectator &&
			    !memb->wait_recover_done)
				memb->recover_journal = 1;

			log_group(mg, "remove member %d recover_journal %d",
				  memb->nodeid, memb->recover_journal);
		}
	}	

	/* add new nodes to members list */

	for (i = 0; i < num_nodes; i++) {
		id = nodeids[i];
		if (is_member(mg, id))
			continue;
		add_member(mg, id);
		pos++;
		log_group(mg, "add member %d", id);
	}

	list_for_each_entry(memb, &mg->members, list) {
		if (low == -1 || memb->nodeid < low)
			low = memb->nodeid;
	}
	mg->low_nodeid = low;

	*pos_out = pos;
	*neg_out = neg;

	log_group(mg, "total members %d", mg->num_memb);
}

struct mountgroup *create_mg(char *name)
{
	struct mountgroup *mg;

	mg = malloc(sizeof(*mg));
	memset(mg, 0, sizeof(*mg));

	INIT_LIST_HEAD(&mg->members);
	INIT_LIST_HEAD(&mg->members_gone);
	mg->first_start = 1;

	strcpy(mg->name, name);
	mg->namelen = strlen(name);

	return mg;
}

struct mountgroup *find_mg(char *name)
{
	struct mountgroup *mg;

	list_for_each_entry(mg, &mounts, list) {
		if ((mg->namelen == strlen(name)) &&
		    !strncmp(mg->name, name, mg->namelen))
			return mg;
	}
	return NULL;
}

int do_mount(char *name)
{
	struct mountgroup *mg;
	char buf[MAXLINE], *info = NULL;
	group_data_t data;
	int rv;

	mg = find_mg(name);
	if (mg) {
		rv = -EEXIST;
		goto fail;
	}

	mg = create_mg(name);
	if (!mg) {
		rv = -ENOMEM;
		goto fail;
	}

	memset(buf, 0, sizeof(buf));

	rv = get_sysfs(mg, "cluster", buf, sizeof(buf));
	if (rv < 0)
		goto fail;

	if (strlen(buf) != strlen(clustername) ||
	    strlen(buf) == 0 || strcmp(buf, clustername)) {
		rv = -1;
		log_error("do_mount: different cluster names: fs=%s cman=%s",
			  buf, clustername);
		goto fail;
	} else
		log_debug("cluster name matches: %s", clustername);

	memset(buf, 0, sizeof(buf));

	rv = get_sysfs(mg, "options", buf, sizeof(buf));

	if (strstr(buf, "spectator")) {
		log_debug("spectator mount");
		mg->spectator = 1;
		info = "spectator";
	} else {
		/* check that we're in fence domain */
		memset(&data, 0, sizeof(data));
		rv = group_get_group(0, "default", &data);
		if (rv || strcmp(data.client_name, "fence") || !data.member) {
			log_error("do_mount: not in default fence domain");
			goto fail;
		}
	}

	list_add(&mg->list, &mounts);

	group_join(gh, name, info);
	return 0;

 fail:
	/* terminate the mount */
	set_sysfs(mg, "mounted", -1);
	return rv;
}

int do_recovery_done(char *name)
{
	struct mountgroup *mg;
	struct mg_member *memb;
	char buf[MAXLINE];
	int rv, jid_done, wait, found = 0;

	mg = find_mg(name);
	if (!mg) {
		log_error("recovery_done: unknown mount group %s", name);
		return -1;
	}

	memset(buf, 0, sizeof(buf));

	rv = get_sysfs(mg, "recover_done", buf, sizeof(buf));
	if (rv < 0)
		return rv;
	jid_done = atoi(buf);

	list_for_each_entry(memb, &mg->members, list) {
		if (memb->jid == jid_done) {
			if (memb->wait_recover_done) {
				memb->wait_recover_done = 0;
				found = 1;
			}
			break;
		}
	}

	if (!found)
		log_debug("jid_recovery_done %d: not waiting", jid_done);

	wait = recover_journals(mg);
	if (!wait)
		group_done(gh, name, mg->start_event_nr);

	return 0;
}

int do_unmount(char *name)
{
	struct mountgroup *mg;

	mg = find_mg(name);
	if (!mg) {
		log_error("do_unmount: unknown mount group %s", name);
		return -1;
	}

	group_leave(gh, name, NULL);

	return 0;
}

int do_stop(char *name)
{
	struct mountgroup *mg;
	int rv;

	mg = find_mg(name);
	if (!mg) {
		log_error("stop: unknown mount group %s", name);
		return -1;
	}

	rv = set_sysfs(mg, "block", 1);

	return rv;
}

int do_finish(char *name, int event_nr)
{
	struct mountgroup *mg;
	int rv;

	mg = find_mg(name);
	if (!mg) {
		log_error("finish: unknown mount group %s", name);
		return -1;
	}

	mg->finish_event_nr = event_nr;

	mount_finished(mg);
	clear_members_finish(mg, mg->finish_event_nr);

	set_sysfs(mg, "block", 0);

	/* only needed if joining */
	rv = set_sysfs(mg, "mounted", 1);

	return rv;
}

/* first mounter is the first non-spectator to join the group */

int first_participant(struct mountgroup *mg, int member_count)
{
	struct mg_member *memb;

	if (member_count == 1)
		return 1;

	list_for_each_entry(memb, &mg->members, list) {
		if (memb->nodeid == our_nodeid)
			continue;
		if (memb->spectator == 0)
			return 0;
	}

	log_debug("first participant of %d members", member_count);
	return 1;
}

int do_start(char *name, int event_nr, int type, int member_count, int *nodeids)
{
	struct mountgroup *mg;
	int wait = 0, pos = 0, neg = 0;

	mg = find_mg(name);
	if (!mg) {
		log_error("start: unknown mount group %s", name);
		return -1;
	}

	mg->start_event_nr = event_nr;
	mg->start_type = type;

	recover_members(mg, member_count, nodeids, &pos, &neg);

	/* NB "first_start" doesn't mean the first group member */
	if (mg->first_start) {
		mg->first_start = 0;

		if (!mg->spectator) {
			claim_journal(mg);
			set_sysfs(mg, "jid", mg->our_jid);

			if (first_participant(mg, member_count))
				set_sysfs(mg, "first", 1);
		}
	}

	if (pos)
		discover_journals(mg);

	if (neg)
		wait = recover_journals(mg);

	if (!wait)
		group_done(gh, name, event_nr);

	return 0;
}

int do_terminate(char *name)
{
	struct mountgroup *mg;

	mg = find_mg(name);
	if (!mg) {
		log_error("terminate: unknown mount group %s", name);
		return -1;
	}

	return set_sysfs(mg, "mounted", -1);
}

