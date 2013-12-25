/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "box/box.h"
#include <arpa/inet.h>

extern "C" {
#include <cfg/warning.h>
#include <cfg/tarantool_box_cfg.h>
} /* extern "C" */

#include <errcode.h>
#include <recovery.h>
#include <log_io.h>
#include <pickle.h>
#include <say.h>
#include <stat.h>
#include <tarantool.h>
#include "tuple.h"
#include "lua/call.h"
#include "schema.h"
#include "space.h"
#include "port.h"
#include "request.h"
#include "txn.h"
#include "fiber.h"

static void process_replica(struct port *port, struct request *request);
static void process_ro(struct port *port, struct request *request);
static void process_rw(struct port *port, struct request *request);
box_process_func box_process = process_ro;
box_process_func box_process_ro = process_ro;

static int stat_base;
int snapshot_pid = 0; /* snapshot processes pid */


/** The snapshot row metadata repeats the structure of REPLACE request. */
struct box_snap_row {
	uint16_t op;
	uint32_t space;
	uint32_t flags;
	char data[];
} __attribute__((packed));

void
port_send_tuple(struct port *port, struct txn *txn, uint32_t flags)
{
	struct tuple *tuple;
	if ((tuple = txn->new_tuple) || (tuple = txn->old_tuple))
		port_add_tuple(port, tuple, flags);
}

static void
process_rw(struct port *port, struct request *request)
{
	struct txn *txn = txn_begin();

	try {
		stat_collect(stat_base, request->req.request_type, 1);
		request->execute(request, txn, port);
		txn_commit(txn);
		port_send_tuple(port, txn, request->flags);
		port_eof(port);
		txn_finish(txn);
	} catch (const Exception& e) {
		txn_rollback(txn);
		throw;
	}
}

static void
process_replica(struct port *port, struct request *request)
{
	if (!request_is_select(request->req.request_type)) {
		tnt_raise(ClientError, ER_NONMASTER,
			  cfg.replication_source);
	}
	return process_rw(port, request);
}

static void
process_ro(struct port *port, struct request *request)
{
	if (!request_is_select(request->req.request_type))
		tnt_raise(LoggedError, ER_SECONDARY);
	return process_rw(port, request);
}

static int
recover_row(void *param __attribute__((unused)), const char *row, uint32_t rowlen)
{
	/* drop wal header */
	if (rowlen < sizeof(struct row_header)) {
		say_error("incorrect row header: expected %zd, got %zd bytes",
			  sizeof(struct row_header), (size_t) rowlen);
		return -1;
	}

	try {
		const char *end = row + rowlen;
		row += sizeof(struct row_header);
		(void) pick_u16(&row, end); /* drop tag - unused. */
		(void) pick_u64(&row, end); /* drop cookie */
		uint16_t op = pick_u16(&row, end);
		struct request request;
		request_create(&request, op, row, end - row);
		process_rw(&null_port, &request);
	} catch (const Exception& e) {
		e.log();
		return -1;
	}

	return 0;
}

static void
box_enter_master_or_replica_mode(struct tarantool_cfg *conf)
{
	if (conf->replication_source != NULL) {
		box_process = process_replica;

		recovery_wait_lsn(recovery_state, recovery_state->lsn);
		if (strcmp(conf->replication_protocol, "1.5") == 0) {
			recovery_follow_remote_1_5(recovery_state,
						   conf->replication_source);
		} else {
			recovery_follow_remote(recovery_state,
					       conf->replication_source);
		}

	} else {
		box_process = process_rw;
		title("primary", NULL);
		say_info("I am primary");
	}
}

void
box_leave_local_standby_mode(void *data __attribute__((unused)))
{
	recovery_finalize(recovery_state);

	recovery_update_mode(recovery_state, cfg.wal_mode,
			     cfg.wal_fsync_delay);

	box_enter_master_or_replica_mode(&cfg);
}

int
box_check_config(struct tarantool_cfg *conf)
{
	if (strindex(wal_mode_STRS, conf->wal_mode,
		     WAL_MODE_MAX) == WAL_MODE_MAX) {
		out_warning(CNF_OK, "wal_mode %s is not recognized", conf->wal_mode);
		return -1;
	}
	/* replication & hot standby modes can not work together */
	if (conf->replication_source != NULL && conf->local_hot_standby > 0) {
		out_warning(CNF_OK, "replication and local hot standby modes "
			       "can't be enabled simultaneously");
		return -1;
	}

	if (strcmp(conf->replication_protocol, "1.5") != 0 &&
	    strcmp(conf->replication_protocol, "1.6") != 0) {
		out_warning(CNF_OK, "unknown replication protocol %s",
			    conf->replication_protocol);
		return -1;
	}


	/* check replication mode */
	if (conf->replication_source != NULL) {
		/* check replication port */
		char ip_addr[32];
		int port;

		if (sscanf(conf->replication_source, "%31[^:]:%i",
			   ip_addr, &port) != 2) {
			out_warning(CNF_OK, "replication source IP address is not recognized");
			return -1;
		}
		if (port <= 0 || port >= USHRT_MAX) {
			out_warning(CNF_OK, "invalid replication source port value: %i", port);
			return -1;
		}
	}

	/* check primary port */
	if (conf->primary_port != 0 &&
	    (conf->primary_port <= 0 || conf->primary_port >= USHRT_MAX)) {
		out_warning(CNF_OK, "invalid primary port value: %i", conf->primary_port);
		return -1;
	}

	/* check secondary port */
	if (conf->secondary_port != 0 &&
	    (conf->secondary_port <= 0 || conf->secondary_port >= USHRT_MAX)) {
		out_warning(CNF_OK, "invalid secondary port value: %i", conf->primary_port);
		return -1;
	}

	/* check rows_per_wal configuration */
	if (conf->rows_per_wal <= 1) {
		out_warning(CNF_OK, "rows_per_wal must be greater than one");
		return -1;
	}

	return 0;
}

int
box_reload_config(struct tarantool_cfg *old_conf, struct tarantool_cfg *new_conf)
{
	if (strcasecmp(old_conf->wal_mode, new_conf->wal_mode) != 0 ||
	    old_conf->wal_fsync_delay != new_conf->wal_fsync_delay) {

		double new_delay = new_conf->wal_fsync_delay;

		/* Mode has changed: */
		if (strcasecmp(old_conf->wal_mode, new_conf->wal_mode)) {
			if (strcasecmp(old_conf->wal_mode, "fsync") == 0 ||
			    strcasecmp(new_conf->wal_mode, "fsync") == 0) {
				out_warning(CNF_OK, "wal_mode cannot switch to/from fsync");
				return -1;
			}
		}

		/*
		 * Unless wal_mode=fsync_delay, wal_fsync_delay is
		 * irrelevant and must be 0.
		 */
		if (strcasecmp(new_conf->wal_mode, "fsync_delay") != 0)
			new_delay = 0.0;


		recovery_update_mode(recovery_state, new_conf->wal_mode, new_delay);
	}

	if (old_conf->snap_io_rate_limit != new_conf->snap_io_rate_limit)
		recovery_update_io_rate_limit(recovery_state, new_conf->snap_io_rate_limit);
	bool old_is_replica = old_conf->replication_source != NULL;
	bool new_is_replica = new_conf->replication_source != NULL;

	if (old_is_replica != new_is_replica ||
	    (old_is_replica &&
	     (strcmp(old_conf->replication_source, new_conf->replication_source) != 0))) {

		if (recovery_state->finalize != true) {
			out_warning(CNF_OK, "Could not propagate %s before local recovery finished",
				    old_is_replica == true ? "slave to master" :
				    "master to slave");

			return -1;
		}
		if (recovery_state->remote) {
			if (strcmp(new_conf->replication_protocol,
				   "1.5") == 0) {
				recovery_stop_remote_1_5(recovery_state);
			} else {
				recovery_stop_remote(recovery_state);
			}
		}

		box_enter_master_or_replica_mode(new_conf);
	}

	return 0;
}

void
box_free(void)
{
	schema_free();
	tuple_free();
	recovery_free();
}

void
box_init()
{
	title("loading", NULL);

	tuple_init(cfg.slab_alloc_arena, cfg.slab_alloc_minimal,
		   cfg.slab_alloc_factor);
	schema_init();

	/* recovery initialization */
	recovery_init(cfg.snap_dir, cfg.wal_dir,
		      recover_row, NULL, cfg.rows_per_wal);
	recovery_update_io_rate_limit(recovery_state, cfg.snap_io_rate_limit);
	recovery_setup_panic(recovery_state, cfg.panic_on_snap_error, cfg.panic_on_wal_error);

	stat_base = stat_register(requests_strs, requests_MAX);

	recover_snap(recovery_state, cfg.replication_source);
	space_end_recover_snapshot();
	recover_existing_wals(recovery_state);
	space_end_recover();

	stat_cleanup(stat_base, requests_MAX);
	title("orphan", NULL);
	if (cfg.local_hot_standby) {
		say_info("starting local hot standby");
		recovery_follow_local(recovery_state, cfg.wal_dir_rescan_delay);
		title("hot_standby", NULL);
	}
}

static void
snapshot_write_tuple(struct log_io *l, struct fio_batch *batch,
		     uint32_t n, struct tuple *tuple)
{
	struct box_snap_row header;
	header.op = REPLACE;
	header.space = n;
	header.flags = BOX_ADD;
	snapshot_write_row(l, batch, (const char *) &header, sizeof(header),
			   tuple->data, tuple->bsize);
}


struct snapshot_space_param {
	struct log_io *l;
	struct fio_batch *batch;
};

static void
snapshot_space(struct space *sp, void *udata)
{
	if (space_is_temporary(sp))
		return;
	struct tuple *tuple;
	struct snapshot_space_param *ud = (struct snapshot_space_param *) udata;
	Index *pk = space_index(sp, 0);
	if (pk == NULL)
		return;
	struct iterator *it = pk->position();
	pk->initIterator(it, ITER_ALL, NULL, 0);

	while ((tuple = it->next(it)))
		snapshot_write_tuple(ud->l, ud->batch, space_id(sp), tuple);
}

void
box_snapshot_cb(struct log_io *l, struct fio_batch *batch)
{
	struct snapshot_space_param ud = { l, batch };

	space_foreach(snapshot_space, &ud);
}

int
box_snapshot(void)
{
	if (snapshot_pid)
		return EINPROGRESS;

	pid_t p = fork();
	if (p < 0) {
		say_syserror("fork");
		return -1;
	}
	if (p > 0) {
		snapshot_pid = p;
		/* increment snapshot version */
		tuple_begin_snapshot();
		int status = wait_for_child(p);
		tuple_end_snapshot();
		snapshot_pid = 0;
		return (WIFSIGNALED(status) ? EINTR : WEXITSTATUS(status));
	}

	slab_arena_mprotect(&tuple_arena);

	title("dumper", "%" PRIu32, getppid());
	fiber_set_name(fiber, status);
	/*
	 * Safety: make sure we don't double-write
	 * parent stdio buffers at exit().
	 */
	close_all_xcpt(1, sayfd);
	snapshot_save(recovery_state, box_snapshot_cb);

	exit(EXIT_SUCCESS);
	return 0;
}

void
box_init_storage(const char *dirname)
{
	struct log_dir dir = snap_dir;
	dir.dirname = (char *) dirname;
	init_storage(&dir, NULL);
}

void
box_info(struct tbuf *out)
{
	tbuf_printf(out, "  status: %s" CRLF, status);
}

const char *
box_status(void)
{
    return status;
}
