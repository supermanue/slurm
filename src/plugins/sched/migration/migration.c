/*****************************************************************************\
 *  migration.c - simple migration scheduler plugin.
 *
 *  If a partition does not have root only access and nodes are not shared
 *  then raise the priority of pending jobs if doing so does not adversely
 *  effect the expected initiation of any higher priority job. We do not alter
 *  a job's required or excluded node list, so this is a conservative
 *  algorithm.
 *
 *  For example, consider a cluster "lx[01-08]" with one job executing on
 *  nodes "lx[01-04]". The highest priority pending job requires five nodes
 *  including "lx05". The next highest priority pending job requires any
 *  three nodes. Without explicitly forcing the second job to use nodes
 *  "lx[06-08]", we can't start it without possibly delaying the higher
 *  priority job.
 *****************************************************************************
 *  Copyright (C) 2003-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#if HAVE_SYS_PRCTL_H
#  include <sys/prctl.h>
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/assoc_mgr.h"
#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/node_select.h"
#include "src/common/parse_time.h"
#include "src/common/power.h"
#include "src/common/read_config.h"
#include "src/common/slurm_accounting_storage.h"
#include "src/common/slurm_mcs.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/slurmctld/acct_policy.h"
#include "src/slurmctld/burst_buffer.h"
#include "src/slurmctld/front_end.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/node_scheduler.h"
#include "src/slurmctld/preempt.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/srun_comm.h"
#include "src/plugins/slurmctld/job_migration/job_migration.h"
#include "migration.h"


#define MIGRATION_INTERVAL	30
#define MIGRATION_RESOLUTION	60
#define MIGRATION_WINDOW		(24 * 60 * 60)
#define BF_MAX_USERS		1000
#define BF_MAX_JOB_ARRAY_RESV	20

#define SLURMCTLD_THREAD_LIMIT	5
#define SCHED_TIMEOUT		2000000	/* time in micro-seconds */

typedef struct node_space_map {
	time_t begin_time;
	time_t end_time;
	bitstr_t *avail_bitmap;
	int next;	/* next record, by time, zero termination */
} node_space_map_t;

/* Diag statistics */
extern diag_stats_t slurmctld_diag_stats;
uint32_t bf_sleep_usec = 0;

/*********************** local variables *********************/
static bool stop_migration = false;
static pthread_mutex_t thread_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t term_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  term_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;
static bool config_flag = false;
static uint64_t debug_flags = 0;
static int migration_interval = MIGRATION_INTERVAL;
static int migration_resolution = MIGRATION_RESOLUTION;
static int migration_window = MIGRATION_WINDOW;
static int bf_max_job_array_resv = BF_MAX_JOB_ARRAY_RESV;
static int bf_min_age_reserve = 0;
static uint32_t bf_min_prio_reserve = 0;
static int max_migration_job_cnt = 100;
static int max_migration_job_per_part = 0;
static int max_migration_job_per_user = 0;
static int max_migration_jobs_start = 0;
static bool migration_continue = false;
static int defer_rpc_cnt = 0;
static int sched_timeout = SCHED_TIMEOUT;

/*********************** local functions *********************/
static void _add_reservation(uint32_t start_time, uint32_t end_reserve,
			     bitstr_t *res_bitmap,
			     node_space_map_t *node_space,
			     int *node_space_recs);
static void *_attempt_migration(void *dummyArg);
extern List _build_running_job_queue();
static bool _can_be_allocated(struct job_record *job_ptr);
static bool _should_be_migrated(struct job_record *job_ptr);
static int  _delta_tv(struct timeval *tv);
static void _do_diag_stats(struct timeval *tv1, struct timeval *tv2);
static void _load_config(void);
static bool _many_pending_rpcs(void);
static bool _more_work(time_t last_migration_time);
static uint32_t _my_sleep(int usec);
static int  _num_feature_count(struct job_record *job_ptr, bool *has_xor);
static int  _try_sched(struct job_record *job_ptr, bitstr_t **avail_bitmap,
		       uint32_t min_nodes, uint32_t max_nodes,
		       uint32_t req_nodes, bitstr_t *exc_core_bitmap);
void _printBitStr(void *data, int size);

static bool _many_pending_rpcs(void)
{
	//info("thread_count = %u", slurmctld_config.server_thread_count);
	if ((defer_rpc_cnt > 0) &&
	    (slurmctld_config.server_thread_count >= defer_rpc_cnt))
		return true;
	return false;
}

/* test if job has feature count specification */
static int _num_feature_count(struct job_record *job_ptr, bool *has_xor)
{
	struct job_details *detail_ptr = job_ptr->details;
	int rc = 0;
	ListIterator feat_iter;
	job_feature_t *feat_ptr;

	if (detail_ptr->feature_list == NULL)	/* no constraints */
		return rc;

	feat_iter = list_iterator_create(detail_ptr->feature_list);
	while ((feat_ptr = (job_feature_t *) list_next(feat_iter))) {
		if (feat_ptr->count)
			rc++;
		if (feat_ptr->op_code == FEATURE_OP_XOR)
			*has_xor = true;
	}
	list_iterator_destroy(feat_iter);

	return rc;
}

/* Attempt to schedule a specific job on specific available nodes
 * IN job_ptr - job to schedule
 * IN/OUT avail_bitmap - nodes available/selected to use
 * IN exc_core_bitmap - cores which can not be used
 * RET SLURM_SUCCESS on success, otherwise an error code
 */


//MANUEL in backfill, this is called in "attempt_backfill". Im this simple algorithm we are not using it
//i removed the comments, they can be seen in backfill.
/*
static int  _try_sched(struct job_record *job_ptr, bitstr_t **avail_bitmap,
		       uint32_t min_nodes, uint32_t max_nodes,
		       uint32_t req_nodes, bitstr_t *exc_core_bitmap)
{
	bitstr_t *low_bitmap = NULL, *tmp_bitmap = NULL;
	int rc = SLURM_SUCCESS;
	bool has_xor = false;
	int feat_cnt = _num_feature_count(job_ptr, &has_xor);
	struct job_details *detail_ptr = job_ptr->details;
	List preemptee_candidates = NULL;
	List preemptee_job_list = NULL;
	ListIterator feat_iter;
	job_feature_t *feat_ptr;

	if (feat_cnt) {

		int i = 0, list_size;
		uint16_t *feat_cnt_orig = NULL, high_cnt = 0;
		list_size = list_count(detail_ptr->feature_list);
		feat_cnt_orig = xmalloc(sizeof(uint16_t) * list_size);
		feat_iter = list_iterator_create(detail_ptr->feature_list);
		while ((feat_ptr = (job_feature_t *) list_next(feat_iter))) {
			high_cnt = MAX(high_cnt, feat_ptr->count);
			feat_cnt_orig[i++] = feat_ptr->count;
			feat_ptr->count = 0;
		}
		list_iterator_destroy(feat_iter);

		if ((job_req_node_filter(job_ptr, *avail_bitmap, true) !=
		     SLURM_SUCCESS) ||
		    (bit_set_count(*avail_bitmap) < high_cnt)) {
			rc = ESLURM_NODES_BUSY;
		} else {
			preemptee_candidates =
				slurm_find_preemptable_jobs(job_ptr);
			rc = select_g_job_test(job_ptr, *avail_bitmap,
					       high_cnt, max_nodes, req_nodes,
					       SELECT_MODE_WILL_RUN,
					       preemptee_candidates,
					       &preemptee_job_list,
					       exc_core_bitmap);
			FREE_NULL_LIST(preemptee_job_list);
		}
		i = 0;
		feat_iter = list_iterator_create(detail_ptr->feature_list);
		while ((feat_ptr = (job_feature_t *) list_next(feat_iter))) {
			feat_ptr->count = feat_cnt_orig[i++];
		}
		list_iterator_destroy(feat_iter);
		xfree(feat_cnt_orig);
	} else if (has_xor) {
		job_feature_t feature_base;
		List feature_cache = detail_ptr->feature_list;
		time_t low_start = 0;

		detail_ptr->feature_list = list_create(NULL);
		feature_base.count = 0;
		feature_base.op_code = FEATURE_OP_END;
		list_append(detail_ptr->feature_list, &feature_base);

		tmp_bitmap = bit_copy(*avail_bitmap);
		feat_iter = list_iterator_create(feature_cache);
		while ((feat_ptr = (job_feature_t *) list_next(feat_iter))) {
			feature_base.name = feat_ptr->name;
			if ((job_req_node_filter(job_ptr, *avail_bitmap, true)
			     == SLURM_SUCCESS) &&
			    (bit_set_count(*avail_bitmap) >= min_nodes)) {
				preemptee_candidates =
					slurm_find_preemptable_jobs(job_ptr);
				rc = select_g_job_test(job_ptr, *avail_bitmap,
						       min_nodes, max_nodes,
						       req_nodes,
						       SELECT_MODE_WILL_RUN,
						       preemptee_candidates,
						       &preemptee_job_list,
						       exc_core_bitmap);
				FREE_NULL_LIST(preemptee_job_list);
				if ((rc == SLURM_SUCCESS) &&
				    ((low_start == 0) ||
				     (low_start > job_ptr->start_time))) {
					low_start = job_ptr->start_time;
					low_bitmap = *avail_bitmap;
					*avail_bitmap = NULL;
				}
			}
			FREE_NULL_BITMAP(*avail_bitmap);
			*avail_bitmap = bit_copy(tmp_bitmap);
		}
		list_iterator_destroy(feat_iter);
		FREE_NULL_BITMAP(tmp_bitmap);
		if (low_start) {
			job_ptr->start_time = low_start;
			rc = SLURM_SUCCESS;
			*avail_bitmap = low_bitmap;
		} else {
			rc = ESLURM_NODES_BUSY;
			FREE_NULL_BITMAP(low_bitmap);
		}
		list_destroy(detail_ptr->feature_list);
		detail_ptr->feature_list = feature_cache;
	} else if (detail_ptr->feature_list) {
		if ((job_req_node_filter(job_ptr, *avail_bitmap, true) !=
		     SLURM_SUCCESS) ||
		    (bit_set_count(*avail_bitmap) < min_nodes)) {
			rc = ESLURM_NODES_BUSY;
		} else {
			preemptee_candidates =
					slurm_find_preemptable_jobs(job_ptr);
			rc = select_g_job_test(job_ptr, *avail_bitmap,
					       min_nodes, max_nodes, req_nodes,
					       SELECT_MODE_WILL_RUN,
					       preemptee_candidates,
					       &preemptee_job_list,
					       exc_core_bitmap);
			FREE_NULL_LIST(preemptee_job_list);
		}
	} else {

		uint16_t orig_shared;
		time_t now = time(NULL);
		char str[100];

		preemptee_candidates = slurm_find_preemptable_jobs(job_ptr);
		orig_shared = job_ptr->details->share_res;
		job_ptr->details->share_res = 0;
		tmp_bitmap = bit_copy(*avail_bitmap);

		if (exc_core_bitmap) {
			bit_fmt(str, (sizeof(str) - 1), exc_core_bitmap);
			debug2("%s exclude core bitmap: %s", __func__, str);
		}

		rc = select_g_job_test(job_ptr, *avail_bitmap, min_nodes,
				       max_nodes, req_nodes,
				       SELECT_MODE_WILL_RUN,
				       preemptee_candidates,
				       &preemptee_job_list,
				       exc_core_bitmap);
		FREE_NULL_LIST(preemptee_job_list);

		job_ptr->details->share_res = orig_shared;

		if (((rc != SLURM_SUCCESS) || (job_ptr->start_time > now)) &&
		    (orig_shared != 0)) {
			FREE_NULL_BITMAP(*avail_bitmap);
			*avail_bitmap = tmp_bitmap;
			rc = select_g_job_test(job_ptr, *avail_bitmap,
					       min_nodes, max_nodes, req_nodes,
					       SELECT_MODE_WILL_RUN,
					       preemptee_candidates,
					       &preemptee_job_list,
					       exc_core_bitmap);
			FREE_NULL_LIST(preemptee_job_list);
		} else
			FREE_NULL_BITMAP(tmp_bitmap);
	}

	FREE_NULL_LIST(preemptee_candidates);
	return rc;
}

*/

/* Terminate migration_agent */
extern void stop_migration_agent(void)
{
	slurm_mutex_lock(&term_lock);
	stop_migration = true;
	slurm_cond_signal(&term_cond);
	slurm_mutex_unlock(&term_lock);
}

/* Return the number of micro-seconds between now and argument "tv" */
static int _delta_tv(struct timeval *tv)
{
	struct timeval now = {0, 0};
	int delta_t;

	if (gettimeofday(&now, NULL))
		return 1;		/* Some error */

	delta_t  = (now.tv_sec - tv->tv_sec) * 1000000;
	delta_t += (now.tv_usec - tv->tv_usec);
	return delta_t;
}

/* Sleep for at least specified time, returns actual sleep time in usec */
static uint32_t _my_sleep(int usec)
{
	int64_t nsec;
	uint32_t sleep_time = 0;
	struct timespec ts = {0, 0};
	struct timeval  tv1 = {0, 0}, tv2 = {0, 0};

	if (gettimeofday(&tv1, NULL)) {		/* Some error */
		sleep(1);
		return 1000000;
	}

	nsec  = tv1.tv_usec + usec;
	nsec *= 1000;
	ts.tv_sec  = tv1.tv_sec + (nsec / 1000000000);
	ts.tv_nsec = nsec % 1000000000;
	slurm_mutex_lock(&term_lock);
	if (!stop_migration)
		slurm_cond_timedwait(&term_cond, &term_lock, &ts);
	slurm_mutex_unlock(&term_lock);
	if (gettimeofday(&tv2, NULL))
		return usec;
	sleep_time = (tv2.tv_sec - tv1.tv_sec) * 1000000;
	sleep_time += tv2.tv_usec;
	sleep_time -= tv1.tv_usec;
	return sleep_time;
}

static void _load_config(void)
{
	char *sched_params, *tmp_ptr;

	sched_params = slurm_get_sched_params();
	debug_flags  = slurm_get_debug_flags();

	if (sched_params && (tmp_ptr = strstr(sched_params, "bf_interval="))) {
		migration_interval = atoi(tmp_ptr + 12);
		if (migration_interval < 1) {
			error("Invalid SchedulerParameters bf_interval: %d",
			      migration_interval);
			migration_interval = MIGRATION_INTERVAL;
		}
	} else {
		migration_interval = MIGRATION_INTERVAL;
	}

	if (sched_params && (tmp_ptr = strstr(sched_params, "bf_window="))) {
		migration_window = atoi(tmp_ptr + 10) * 60;  /* mins to secs */
		if (migration_window < 1) {
			error("Invalid SchedulerParameters bf_window: %d",
			      migration_window);
			migration_window = MIGRATION_WINDOW;
		}
	} else {
		migration_window = MIGRATION_WINDOW;
	}

	/* "max_job_bf" replaced by "bf_max_job_test" in version 14.03 and
	 * can be removed later. Only "bf_max_job_test" is documented. */
	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_max_job_test=")))
		max_migration_job_cnt = atoi(tmp_ptr + 16);
	else if (sched_params && (tmp_ptr=strstr(sched_params, "max_job_bf=")))
		max_migration_job_cnt = atoi(tmp_ptr + 11);
	else
		max_migration_job_cnt = 100;
	if (max_migration_job_cnt < 1) {
		error("Invalid SchedulerParameters bf_max_job_test: %d",
		      max_migration_job_cnt);
		max_migration_job_cnt = 100;
	}

	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_resolution="))) {
		migration_resolution = atoi(tmp_ptr + 14);
		if (migration_resolution < 1) {
			error("Invalid SchedulerParameters bf_resolution: %d",
			      migration_resolution);
			migration_resolution = MIGRATION_RESOLUTION;
		}
	} else {
		migration_resolution = MIGRATION_RESOLUTION;
	}

	if (sched_params &&
	    (tmp_ptr = strstr(sched_params, "bf_max_job_array_resv="))) {
		bf_max_job_array_resv = atoi(tmp_ptr + 22);
		if (bf_max_job_array_resv < 0) {
			error("Invalid SchedulerParameters bf_max_job_array_resv: %d",
			      bf_max_job_array_resv);
			bf_max_job_array_resv = BF_MAX_JOB_ARRAY_RESV;
		}
	} else {
		bf_max_job_array_resv = BF_MAX_JOB_ARRAY_RESV;
	}

	if (sched_params &&
	    (tmp_ptr = strstr(sched_params, "bf_max_job_part="))) {
		max_migration_job_per_part = atoi(tmp_ptr + 16);
		if (max_migration_job_per_part < 0) {
			error("Invalid SchedulerParameters bf_max_job_part: %d",
			      max_migration_job_per_part);
			max_migration_job_per_part = 0;
		}
	} else {
		max_migration_job_per_part = 0;
	}
	if ((max_migration_job_per_part != 0) &&
	    (max_migration_job_per_part >= max_migration_job_cnt)) {
		error("bf_max_job_part >= bf_max_job_test (%u >= %u)",
		      max_migration_job_per_part, max_migration_job_cnt);
	}

	if (sched_params &&
	    (tmp_ptr = strstr(sched_params, "bf_max_job_start="))) {
		max_migration_jobs_start = atoi(tmp_ptr + 17);
		if (max_migration_jobs_start < 0) {
			error("Invalid SchedulerParameters bf_max_job_start: %d",
			      max_migration_jobs_start);
			max_migration_jobs_start = 0;
		}
	} else {
		max_migration_jobs_start = 0;
	}

	if (sched_params &&
	    (tmp_ptr = strstr(sched_params, "bf_max_job_user="))) {
		max_migration_job_per_user = atoi(tmp_ptr + 16);
		if (max_migration_job_per_user < 0) {
			error("Invalid SchedulerParameters bf_max_job_user: %d",
			      max_migration_job_per_user);
			max_migration_job_per_user = 0;
		}
	} else {
		max_migration_job_per_user = 0;
	}
	if ((max_migration_job_per_user != 0) &&
	    (max_migration_job_per_user >= max_migration_job_cnt)) {
		error("bf_max_job_user >= bf_max_job_test (%u >= %u)",
		      max_migration_job_per_user, max_migration_job_cnt);
	}

	bf_min_age_reserve = 0;
	if (sched_params &&
	    (tmp_ptr = strstr(sched_params, "bf_min_age_reserve="))) {
		int min_age = atoi(tmp_ptr + 19);
		if (min_age < 0) {
			error("Invalid SchedulerParameters bf_min_age_reserve: %d",
			      min_age);
		} else {
			bf_min_age_reserve = min_age;
		}
	}

	bf_min_prio_reserve = 0;
	if (sched_params &&
	    (tmp_ptr = strstr(sched_params, "bf_min_prio_reserve="))) {
		int64_t min_prio = (int64_t) atoll(tmp_ptr + 20);
		if (min_prio < 0) {
			error("Invalid SchedulerParameters bf_min_prio_reserve: %"PRIi64,
			      min_prio);
		} else {
			bf_min_prio_reserve = (uint32_t) min_prio;
		}
	}

	/* bf_continue makes migration continue where it was if interrupted */
	if (sched_params && (strstr(sched_params, "bf_continue"))) {
		migration_continue = true;
	} else {
		migration_continue = false;
	}

	if (sched_params &&
	    (tmp_ptr = strstr(sched_params, "bf_yield_interval="))) {
		sched_timeout = atoi(tmp_ptr + 18);
		if (sched_timeout <= 0) {
			error("Invalid migration scheduler bf_yield_interval: %d",
			      sched_timeout);
			sched_timeout = SCHED_TIMEOUT;
		}
	} else {
		sched_timeout = SCHED_TIMEOUT;
	}

	if (sched_params && (tmp_ptr = strstr(sched_params, "max_rpc_cnt=")))
		defer_rpc_cnt = atoi(tmp_ptr + 12);
	else if (sched_params &&
		 (tmp_ptr = strstr(sched_params, "max_rpc_count=")))
		defer_rpc_cnt = atoi(tmp_ptr + 14);
	else
		defer_rpc_cnt = 0;
	if (defer_rpc_cnt < 0) {
		error("Invalid SchedulerParameters max_rpc_cnt: %d",
		      defer_rpc_cnt);
		defer_rpc_cnt = 0;
	}

	xfree(sched_params);
}

/* Note that slurm.conf has changed */
extern void migration_reconfig(void)
{
	slurm_mutex_lock(&config_lock);
	config_flag = true;
	slurm_mutex_unlock(&config_lock);
}

/* Update migration scheduling statistics
 * IN tv1 - start time
 * IN tv2 - end (current) time
 */
static void _do_diag_stats(struct timeval *tv1, struct timeval *tv2)
{
	uint32_t delta_t, real_time;

	delta_t  = (tv2->tv_sec - tv1->tv_sec) * 1000000;
	delta_t +=  tv2->tv_usec;
	delta_t -=  tv1->tv_usec;
	real_time = delta_t - bf_sleep_usec;

	slurmctld_diag_stats.bf_cycle_counter++;
	slurmctld_diag_stats.bf_cycle_sum += real_time;
	slurmctld_diag_stats.bf_cycle_last = real_time;

	slurmctld_diag_stats.bf_depth_sum += slurmctld_diag_stats.bf_last_depth;
	slurmctld_diag_stats.bf_depth_try_sum +=
		slurmctld_diag_stats.bf_last_depth_try;
	if (slurmctld_diag_stats.bf_cycle_last >
	    slurmctld_diag_stats.bf_cycle_max) {
		slurmctld_diag_stats.bf_cycle_max = slurmctld_diag_stats.
						    bf_cycle_last;
	}

	slurmctld_diag_stats.mg_active = 0;
}


/* migration_agent - detached thread periodically attempts to migration jobs */
//MANUEL this wakes up every minute or so, and then calls _attempt_migration
extern void *migration_agent(void *args)
{
	time_t now;
	double wait_time;
	static time_t last_migration_time = 0;
	/* Read config and partitions; Write jobs and nodes */
	slurmctld_lock_t all_locks = {
		READ_LOCK, WRITE_LOCK, WRITE_LOCK, READ_LOCK, NO_LOCK };
	bool load_config;
	bool short_sleep = false;

	#if HAVE_SYS_PRCTL_H
		if (prctl(PR_SET_NAME, "mgtn", NULL, NULL, NULL) < 0) {
			error("%s: cannot set my name to %s %m", __func__, "migration");
		}
	#endif
	_load_config();
	last_migration_time = time(NULL);
	while (!stop_migration) {
		if (short_sleep)
			_my_sleep(1000000);
		else
			_my_sleep(migration_interval * 1000000);
		if (stop_migration)
			break;
		slurm_mutex_lock(&config_lock);
		if (config_flag) {
			config_flag = false;
			load_config = true;
		} else {
			load_config = false;
		}
		slurm_mutex_unlock(&config_lock);
		if (load_config)
			_load_config();
		now = time(NULL);
		wait_time = difftime(now, last_migration_time);
		if ((wait_time < migration_interval) ||
		    job_is_completing(NULL) || _many_pending_rpcs() ||
		    !avail_front_end(NULL) || !_more_work(last_migration_time)) {
			short_sleep = true;
			continue;
		}
		lock_slurmctld(all_locks);

		pthread_t inc_x_thread;
		if(pthread_create(&inc_x_thread, NULL, _attempt_migration, NULL)) {
			debug ("MANUEL I broke it LOL");
			return NULL;
		}

		last_migration_time = time(NULL);
		(void) bb_g_job_try_stage_in();
		unlock_slurmctld(all_locks);
		short_sleep = false;
	}
	return NULL;
}

//here we decide if a given job can be migrated or not
static bool _should_be_migrated(struct job_record *job_ptr){
	printf ("Deciding what to do with job %d\n", job_ptr->job_id);


	time_t start_time;
	//TODO esto tendría que ser algo como job_ptr->step_id (slurmctld.h)
	uint32_t stepid = -1;
	if (slurm_checkpoint_able(job_ptr->job_id, NO_VAL,&start_time) != 0) {
		debug ("Job %u is not checkpointable, not migrating this job",job_ptr->job_id );
		return false;
	}


	///job_ptr is declared in /slurm/src/slurmctld/slurmctld.h
	if (job_ptr->details->req_nodes != NULL ) {
		debug ("User has specified required nodes for job %u, not migrating this job", job_ptr->job_id);
		return false;
	}

	//it makes no sense to migrate a job employing a whole node
	//note that this is called "exclusive" in some other places
	if (job_ptr->details->whole_node == 1 ) {
		debug ("User has specified whole node for job %u, not migrating this job", job_ptr->job_id);
		return false;
	}


	/*
	The following examples serve as proof of concept and to show how to
	access the different resources and fields available to the developer
	*/

	//Migrate serial jobs from nodes 1X to 3X
	if (job_ptr->total_cpus == 1){

		printf ("penultima cifra: %c\n", job_ptr->nodes[strlen(job_ptr->nodes)-2]);
	 	int one = '1';
		if (job_ptr->nodes[strlen(job_ptr->nodes)-2] !=  one)
			return false;
}
	// We are migrating parallel jobs NOT using the minnimum posible number of nodes
if (job_ptr->total_cpus > 1){
	printf ("JOB %d IS A PARALLEL JOB WITH %d CPUs\n", job_ptr->job_id, job_ptr->total_cpus);
	int avg_node_size = cluster_cpus / bit_size(avail_node_bitmap); //problem: clusters heterogéneos con CPUs de distintos tamaños

	//home-made CEIL implementation for positive integers
	int number_of_nodes = bit_set_count(job_ptr->job_resrcs->node_bitmap);
	int minimal_number_of_nodes = (int)(job_ptr->cpu_cnt / avg_node_size);
	if (minimal_number_of_nodes * avg_node_size < job_ptr->cpu_cnt)
			minimal_number_of_nodes += 1;

	if (number_of_nodes <= minimal_number_of_nodes)
		return false;
	else
		debug ("job %u is NOT running in the minnimum posible number of nodes, we should migrate", job_ptr->job_id);

}

	debug ("We are migratig job with id %u and node %s", job_ptr->job_id, job_ptr->nodes);
		return true;
}




//MANUEL this is called every minute. Here we decide whether to migrate each task or not.
//it is called by  *migration_agent(void *args)
//this uses to be "int"
static void *_attempt_migration(void *dummyArg)
{
	DEF_TIMERS;
	List job_queue;
	job_queue_rec_t *job_queue_rec;
	struct job_record *job_ptr;
	bitstr_t  *avail_bitmap = NULL; //*active_bitmap = NULL,
	bitstr_t *exc_core_bitmap = NULL, *resv_bitmap = NULL;
	time_t now, sched_start; // later_start, start_res, resv_end, window_end;
	time_t orig_sched_start = (time_t) 0; //, orig_start_time
	struct timeval bf_time1, bf_time2;
	int rc = 0;
	int job_test_count = 0, test_time_count = 0; //, pend_time;
	uint32_t *uid = NULL,  *bf_part_jobs = NULL; //nuser = 0, bf_parts = 0,
	uint16_t *njobs = NULL;
	time_t config_update = slurmctld_conf.last_update;
	time_t part_update = last_part_update;
	struct timeval start_tv;

	bf_sleep_usec = 0;
	#ifdef HAVE_ALPS_CRAY

	START_TIMER;
		if (select_g_update_block(NULL)) {
			debug4("migration: not scheduling due to ALPS");
			return SLURM_SUCCESS;
		}
		END_TIMER;
		if (debug_flags & DEBUG_FLAG_MIGRATION)
			info("migration: ALPS inventory completed, %s", TIME_STR);
	#endif

	(void) bb_g_load_state(false);

	START_TIMER;
	if (debug_flags & DEBUG_FLAG_MIGRATION)
		info("\n\n\n\n\nmigration: beginning");
	else
		debug("\n\n\n\n\nmigration: beginning");
	sched_start = orig_sched_start = now = time(NULL);
	gettimeofday(&start_tv, NULL);

	//MANUEL
	job_queue = _build_running_job_queue();
	job_test_count = list_count(job_queue);
	if (job_test_count == 0) {
		if (debug_flags & DEBUG_FLAG_MIGRATION)
			info("MANUEL migration: no running jobs");
		else
			debug("MANUEL migration: no running jobs");
		FREE_NULL_LIST(job_queue);
		return 0;
	}

	if (slurmctld_diag_stats.mg_active ==1){
		debug ("Migration is already being executed, exiting.");
		return 0;
	}
	slurmctld_diag_stats.mg_active = 1;
	sort_job_queue(job_queue);

	while (1) {
		job_queue_rec = (job_queue_rec_t *) list_pop(job_queue);
		if (!job_queue_rec) {
			if (debug_flags & DEBUG_FLAG_MIGRATION)
			info("migration: reached end of job queue");
			break;
		}
		if (slurmctld_config.shutdown_time ||
			(difftime(time(NULL),orig_sched_start)>=migration_interval)){
				xfree(job_queue_rec);
				break;
			}

		//Esto son cosas de la configuración que intuyo que es mejor no tocar
		if (((defer_rpc_cnt > 0) &&
		(slurmctld_config.server_thread_count >= defer_rpc_cnt)) ||
		(_delta_tv(&start_tv) >= sched_timeout)) {
			if (debug_flags & DEBUG_FLAG_MIGRATION) {
				END_TIMER;
				info("migration: yielding locks after testing "
				"%u(%d) jobs, %s",
				slurmctld_diag_stats.bf_last_depth,
				job_test_count, TIME_STR);
			}
		if ((!migration_continue) ||
			(slurmctld_conf.last_update != config_update) ||
			(last_part_update != part_update)) {
				if (debug_flags & DEBUG_FLAG_MIGRATION) {
					info("migration: system state changed, "
					"breaking out after testing "
					"%u(%d) jobs",
					slurmctld_diag_stats.bf_last_depth,
					job_test_count);
				}
				rc = 1;
				xfree(job_queue_rec);
				break;
			}
			/* Reset migration scheduling timers, resume testing */
			sched_start = time(NULL);
			gettimeofday(&start_tv, NULL);
			job_test_count = 0;
			test_time_count = 0;
			START_TIMER;
		}
		//ESTE ES EL TRABAJO EN EJECUCION QUE PODEMOS MIGRAR
		//vamos a ver que este todo bien

		job_ptr  = job_queue_rec->job_ptr;
		if (!job_ptr)	/* All task array elements started */
			continue;
		if (!IS_JOB_RUNNING(job_ptr))	/* Something happened while getting here */
			continue;


/*
a different approach could be joining those two methods in something like
"get_destination_node", which returns the node(s) where the job should be
migrated to have a faster execution.

This however depends on the final objective of this algorithm and is not relevant
right here
*/


		if (!_should_be_migrated(job_ptr))
			continue;

	//debug ("migration for sched algorithm disabled");
	//continue;

		//debug ("MANUEL 8");
	//slurm_checkpoint_migrate (uint32_t job_id, uint32_t step_id, char *destination_nodes, char *excluded_nodes, char *drain_node,  int shared, int spread, bool test_only)
	if (slurm_checkpoint_migrate (job_ptr->job_id, NO_VAL, "", "acme12", "", NO_VAL, NO_VAL, true) != 0){
		printf("Job %d cannot be migrated, continuing", job_ptr->job_id);
		continue;
	}

	if (slurm_checkpoint_migrate (job_ptr->job_id, NO_VAL, "", "acme12", "", NO_VAL, NO_VAL, false) != 0){
		printf ("Errror when migrating job %d. What should I do?", job_ptr->job_id);
		continue;
	}
	debug ("End of migration for job %u", job_ptr->job_id);
	debug ("We only migrate a job per call. Exiting...");
	break;
	} //while recorrer todos los trabajos running

	//DESDE AQUI, LIMPIEZA
	xfree(bf_part_jobs);
	xfree(uid);
	xfree(njobs);
	FREE_NULL_BITMAP(avail_bitmap);
	FREE_NULL_BITMAP(exc_core_bitmap);
	FREE_NULL_BITMAP(resv_bitmap);

	//TODO MANUEL esto lo mismo hay que liberarlo! Lo he quitado de momento
	/*
	for (i=0; ; ) {
		FREE_NULL_BITMAP(node_space[i].avail_bitmap);
		if ((i = node_space[i].next) == 0)
		break;
	}
	xfree(node_space);
	*/
	FREE_NULL_LIST(job_queue);
	gettimeofday(&bf_time2, NULL);
	_do_diag_stats(&bf_time1, &bf_time2);

	if (debug_flags & DEBUG_FLAG_MIGRATION) {
		END_TIMER;
		info("migration: completed testing %u(%d) jobs, %s",
		slurmctld_diag_stats.bf_last_depth,
		job_test_count, TIME_STR);
	}
	if (slurmctld_config.server_thread_count >= 150) {
		info("migration: %d pending RPCs at cycle end, consider "
		"configuring max_rpc_cnt",
		slurmctld_config.server_thread_count);
	}
//	return rc;
	return NULL;
}


/* Report if any changes occurred to job, node or partition information */
static bool _more_work (time_t last_migration_time)
{
	bool rc = false;

	slurm_mutex_lock( &thread_flag_mutex );
	if ( (last_job_update  >= last_migration_time ) ||
	     (last_node_update >= last_migration_time ) ||
	     (last_part_update >= last_migration_time ) ) {
		rc = true;
	}
	slurm_mutex_unlock( &thread_flag_mutex );
	return rc;
}

/* Create a reservation for a job in the future */
//MANUEL: ESTO PUEDE SER MUY NECESARIO PARA LA MIGRACION!!!!
static void _add_reservation(uint32_t start_time, uint32_t end_reserve,
			     bitstr_t *res_bitmap,
			     node_space_map_t *node_space,
			     int *node_space_recs)
	{
	bool placed = false;
	int i, j;

	start_time = MAX(start_time, node_space[0].begin_time);
	for (j = 0; ; ) {
		if (node_space[j].end_time > start_time) {
			/* insert start entry record */
			i = *node_space_recs;
			node_space[i].begin_time = start_time;
			node_space[i].end_time = node_space[j].end_time;
			node_space[j].end_time = start_time;
			node_space[i].avail_bitmap =
				bit_copy(node_space[j].avail_bitmap);
			node_space[i].next = node_space[j].next;
			node_space[j].next = i;
			(*node_space_recs)++;
			placed = true;
		}
		if (node_space[j].end_time == start_time) {
			/* no need to insert new start entry record */
			placed = true;
		}
		if (placed == true) {
			while ((j = node_space[j].next)) {
				if (end_reserve < node_space[j].end_time) {
					/* insert end entry record */
					i = *node_space_recs;
					node_space[i].begin_time = end_reserve;
					node_space[i].end_time = node_space[j].
								 end_time;
					node_space[j].end_time = end_reserve;
					node_space[i].avail_bitmap =
						bit_copy(node_space[j].
							 avail_bitmap);
					node_space[i].next = node_space[j].next;
					node_space[j].next = i;
					(*node_space_recs)++;
					break;
				}
				if (end_reserve == node_space[j].end_time) {
					break;
				}
			}
			break;
		}
		if ((j = node_space[j].next) == 0)
			break;
	}

	for (j = 0; ; ) {
		if ((node_space[j].begin_time >= start_time) &&
		    (node_space[j].end_time <= end_reserve))
			bit_and(node_space[j].avail_bitmap, res_bitmap);
		if ((node_space[j].begin_time >= end_reserve) ||
		    ((j = node_space[j].next) == 0))
			break;
	}

	/* Drop records with identical bitmaps (up to one record).
	 * This can significantly improve performance of the migration tests. */
	for (i = 0; ; ) {
		if ((j = node_space[i].next) == 0)
			break;
		if (!bit_equal(node_space[i].avail_bitmap,
			       node_space[j].avail_bitmap)) {
			i = j;
			continue;
		}
		node_space[i].end_time = node_space[j].end_time;
		node_space[i].next = node_space[j].next;
		FREE_NULL_BITMAP(node_space[j].avail_bitmap);
		break;
	}
}



static void _job_queue_rec_del(void *x)
{
	xfree(x);
}
/*
 * _build_running_job_queue - build (non-priority ordered) list of running jobs
 * RET the job queue
 * NOTE: the caller must call FREE_NULL_LIST() on RET value to free memory
 */

extern List _build_running_job_queue()
{
	List job_queue;
	ListIterator job_iterator;
	struct job_record *job_ptr = NULL;
	struct timeval start_tv = {0, 0};

	(void) _delta_tv(&start_tv);
	job_queue = list_create(_job_queue_rec_del);

	/* Create individual job records for job arrays that need burst buffer
	 * staging */
	job_iterator = list_iterator_create(job_list);
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		if (IS_JOB_RUNNING(job_ptr)) {
			job_queue_rec_t *job_queue_rec;
			job_queue_rec = xmalloc(sizeof(job_queue_rec_t));
			job_queue_rec->array_task_id = job_ptr->array_task_id;
			job_queue_rec->job_id   = job_ptr->job_id;
			job_queue_rec->job_ptr  = job_ptr;
			job_queue_rec->priority = job_ptr->priority;
			list_append(job_queue, job_queue_rec);
		}
	}
	list_iterator_destroy(job_iterator);
	return job_queue;
}
