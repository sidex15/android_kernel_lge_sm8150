/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/*
 * NOTE: This file has been modified by Sony Mobile Communications Inc.
 * Modifications are Copyright (c) 2018 Sony Mobile Communications Inc,
 * and licensed under the license of the file.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched/signal.h>
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/profile.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/swap.h>
#include <linux/fs.h>
#include <linux/cpuset.h>
#include <linux/vmpressure.h>
#include <linux/freezer.h>
#include <linux/memory.h>

#define CREATE_TRACE_POINTS
#include <trace/events/almk.h>
#include <linux/show_mem_notifier.h>

#ifdef CONFIG_HSWAP
#include <linux/delay.h>
#include <linux/kthread.h>
#include "../../block/zram/zram_drv.h"
#endif

#ifdef LMK_TNG_ENABLE_TRACE
#include <trace/events/lmk.h>
#endif

#ifdef CONFIG_HIGHMEM
#define _ZONE ZONE_HIGHMEM
#else
#define _ZONE ZONE_NORMAL
#endif

#define CREATE_TRACE_POINTS
#include "trace/lowmemorykiller.h"

/* to enable lowmemorykiller */
static int enable_lmk = 1;
module_param_named(enable_lmk, enable_lmk, int, 0644);

#include "lowmemorykiller_stats.h"
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_TNG
#include "lowmemorykiller_tng.h"
#endif

u32 lowmem_debug_level = 1;
short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};

static int lowmem_adj_size = 4;
int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};

static int lowmem_minfree_size = 4;
static int lmk_fast_run = 0;

static int lmk_kill_cnt = 0;
#ifdef CONFIG_HSWAP
static int lmk_reclaim_cnt = 0;

enum alloc_pressure {
	PRESSURE_NORMAL,
	PRESSURE_HIGH
};

enum {
	KILL_LMK,
	KILL_MEMORY_PRESSURE,
	KILL_NO_RECLAIMABLE,
	KILL_RECLAIMING,
	KILL_SWAP_FULL,
	REASON_COUNT
};

static char* kill_reason_str[REASON_COUNT] = {
	"by lmk",
	"by mem pressure",
	"by no reclaimable",
	"by reclaming",
	"by swap full"
};
#endif

static unsigned long lowmem_deathpending_timeout;

int lowmem_min_param_size(void)
{
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	return array_size;
}

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)

static unsigned long lowmem_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	if (!enable_lmk)
		return 0;
	lmk_inc_stats(LMK_COUNT);
	return global_node_page_state(NR_ACTIVE_ANON) +
		global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_ANON) +
		global_node_page_state(NR_INACTIVE_FILE);
}

static atomic_t shift_adj = ATOMIC_INIT(0);
static short adj_max_shift = 353;
module_param_named(adj_max_shift, adj_max_shift, short, 0644);

enum {
	ADAPTIVE_LMK_DISABLED = 0,
	ADAPTIVE_LMK_ENABLED,
	ADAPTIVE_LMK_WAS_ENABLED,
};

/* User knob to enable/disable adaptive lmk feature */
static int enable_adaptive_lmk = ADAPTIVE_LMK_DISABLED;
module_param_named(enable_adaptive_lmk, enable_adaptive_lmk, int, 0644);

/*
 * This parameter controls the behaviour of LMK when vmpressure is in
 * the range of 90-94. Adaptive lmk triggers based on number of file
 * pages wrt vmpressure_file_min, when vmpressure is in the range of
 * 90-94. Usually this is a pseudo minfree value, higher than the
 * highest configured value in minfree array.
 */
static int vmpressure_file_min;
module_param_named(vmpressure_file_min, vmpressure_file_min, int, 0644);

/* User knob to enable/disable oom reaping feature */
int oom_reaper = 1;
module_param_named(oom_reaper, oom_reaper, int, 0644);

enum {
	VMPRESSURE_NO_ADJUST = 0,
	VMPRESSURE_ADJUST_ENCROACH,
	VMPRESSURE_ADJUST_NORMAL,
};

int adjust_minadj(short *min_score_adj)
{
	int ret = VMPRESSURE_NO_ADJUST;

	if (enable_adaptive_lmk != ADAPTIVE_LMK_ENABLED)
		return 0;

	if (atomic_read(&shift_adj) &&
	    (*min_score_adj > adj_max_shift)) {
		if (*min_score_adj == OOM_SCORE_ADJ_MAX + 1)
			ret = VMPRESSURE_ADJUST_ENCROACH;
		else
			ret = VMPRESSURE_ADJUST_NORMAL;
		*min_score_adj = adj_max_shift;
	}
	atomic_set(&shift_adj, 0);

	return ret;
}

static int lmk_vmpressure_notifier(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	int other_free, other_file;
	unsigned long pressure = action;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (enable_adaptive_lmk != ADAPTIVE_LMK_ENABLED) {
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_TNG
		balance_cache(pressure);
#endif
		return 0;
	}

	if (pressure >= 95) {
		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			total_swapcache_pages();
		other_free = global_zone_page_state(NR_FREE_PAGES);

		atomic_set(&shift_adj, 1);
		trace_almk_vmpressure(pressure, other_free, other_file);
	} else if (pressure >= 90) {
		if (lowmem_adj_size < array_size)
			array_size = lowmem_adj_size;
		if (lowmem_minfree_size < array_size)
			array_size = lowmem_minfree_size;

		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			total_swapcache_pages();

		other_free = global_zone_page_state(NR_FREE_PAGES);

		if (other_free < lowmem_minfree[array_size - 1] &&
		    other_file < vmpressure_file_min) {
			atomic_set(&shift_adj, 1);
			trace_almk_vmpressure(pressure, other_free, other_file);
		}
	} else if (atomic_read(&shift_adj)) {
		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			total_swapcache_pages();

		other_free = global_zone_page_state(NR_FREE_PAGES);
		/*
		 * shift_adj would have been set by a previous invocation
		 * of notifier, which is not followed by a lowmem_shrink yet.
		 * Since vmpressure has improved, reset shift_adj to avoid
		 * false adaptive LMK trigger.
		 */
		trace_almk_vmpressure(pressure, other_free, other_file);
		atomic_set(&shift_adj, 0);
	}
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_TNG
	balance_cache(pressure);
#endif
	return 0;
}

static struct notifier_block lmk_vmpr_nb = {
	.notifier_call = lmk_vmpressure_notifier,
};

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static int test_task_state(struct task_struct *p, int state)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (t->state & state) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static int test_task_lmk_waiting(struct task_struct *p)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (task_lmk_waiting(t)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static DEFINE_MUTEX(scan_mutex);

static int can_use_cma_pages(gfp_t gfp_mask)
{
	if (gfpflags_to_migratetype(gfp_mask) == MIGRATE_MOVABLE &&
	    (gfp_mask & __GFP_CMA))
		return 1;

	return 0;
}

void tune_lmk_zone_param(struct zonelist *zonelist, int classzone_idx,
					int *other_free, int *other_file,
					int use_cma_pages)
{
	struct zone *zone;
	struct zoneref *zoneref;
	int zone_idx;

	for_each_zone_zonelist(zone, zoneref, zonelist, MAX_NR_ZONES) {
		zone_idx = zonelist_zone_idx(zoneref);

		if (zone_idx > classzone_idx) {
			if (other_free != NULL)
				*other_free -= zone_page_state(zone,
							       NR_FREE_PAGES);
			if (other_file != NULL)
				*other_file -= zone_page_state(zone,
					NR_ZONE_INACTIVE_FILE) +
					zone_page_state(zone,
					NR_ZONE_ACTIVE_FILE);
		} else if (zone_idx < classzone_idx) {
			if (zone_watermark_ok(zone, 0, 0, classzone_idx, 0) &&
			    other_free) {
				if (!use_cma_pages) {
					*other_free -= min(
					  zone->lowmem_reserve[classzone_idx] +
					  zone_page_state(
					    zone, NR_FREE_CMA_PAGES),
					  zone_page_state(
					    zone, NR_FREE_PAGES));
				} else {
					*other_free -=
					  zone->lowmem_reserve[classzone_idx];
				}
			} else {
				if (other_free)
					*other_free -=
					  zone_page_state(zone, NR_FREE_PAGES);
			}
		}
	}
}

#ifdef CONFIG_HIGHMEM
static void adjust_gfp_mask(gfp_t *gfp_mask)
{
	struct zone *preferred_zone;
	struct zoneref *zref;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx;

	if (current_is_kswapd()) {
		zonelist = node_zonelist(0, *gfp_mask);
		high_zoneidx = gfp_zone(*gfp_mask);
		zref = first_zones_zonelist(zonelist, high_zoneidx, NULL);
		preferred_zone = zref->zone;

		if (high_zoneidx == ZONE_NORMAL) {
			if (zone_watermark_ok_safe(
					preferred_zone, 0,
					high_wmark_pages(preferred_zone), 0))
				*gfp_mask |= __GFP_HIGHMEM;
		} else if (high_zoneidx == ZONE_HIGHMEM) {
			*gfp_mask |= __GFP_HIGHMEM;
		}
	}
}
#else
static void adjust_gfp_mask(gfp_t *unused)
{
}
#endif

void tune_lmk_param(int *other_free, int *other_file, struct shrink_control *sc)
{
	gfp_t gfp_mask;
	struct zone *preferred_zone;
	struct zoneref *zref;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx, classzone_idx;
	unsigned long balance_gap;
	int use_cma_pages;

	gfp_mask = sc->gfp_mask;
	adjust_gfp_mask(&gfp_mask);

	zonelist = node_zonelist(0, gfp_mask);
	high_zoneidx = gfp_zone(gfp_mask);
	zref = first_zones_zonelist(zonelist, high_zoneidx, NULL);
	preferred_zone = zref->zone;
	classzone_idx = zone_idx(preferred_zone);
	use_cma_pages = can_use_cma_pages(gfp_mask);

	balance_gap = min(low_wmark_pages(preferred_zone),
			  (preferred_zone->present_pages +
			   100-1) /
			   100);

	if (likely(current_is_kswapd() && zone_watermark_ok(preferred_zone, 0,
			  high_wmark_pages(preferred_zone) + SWAP_CLUSTER_MAX +
			  balance_gap, 0, 0))) {
		if (lmk_fast_run)
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       other_file, use_cma_pages);
		else
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       NULL, use_cma_pages);

		if (zone_watermark_ok(preferred_zone, 0, 0, _ZONE, 0)) {
			if (!use_cma_pages) {
				*other_free -= min(
				  preferred_zone->lowmem_reserve[_ZONE]
				  + zone_page_state(
				    preferred_zone, NR_FREE_CMA_PAGES),
				  zone_page_state(
				    preferred_zone, NR_FREE_PAGES));
			} else {
				*other_free -=
				  preferred_zone->lowmem_reserve[_ZONE];
			}
		} else {
			*other_free -= zone_page_state(preferred_zone,
						      NR_FREE_PAGES);
		}

		lowmem_print(4, "lowmem_shrink of kswapd tunning for highmem "
			     "ofree %d, %d\n", *other_free, *other_file);
	} else {
		tune_lmk_zone_param(zonelist, classzone_idx, other_free,
			       other_file, use_cma_pages);

		if (!use_cma_pages) {
			*other_free -=
			  zone_page_state(preferred_zone, NR_FREE_CMA_PAGES);
		}

		lowmem_print(4, "lowmem_shrink tunning for others ofree %d, "
			     "%d\n", *other_free, *other_file);
	}
}

/*
 * Return the percent of memory which gfp_mask is allowed to allocate from.
 * CMA memory is assumed to be a small percent and is not considered.
 * The goal is to apply a margin of minfree over all zones, rather than to
 * each zone individually.
 */
static int get_minfree_scalefactor(gfp_t gfp_mask)
{
	struct zonelist *zonelist = node_zonelist(0, gfp_mask);
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_usable = 0;

	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask))
		nr_usable += zone->managed_pages;

	return max_t(int, 1, mult_frac(100, nr_usable, totalram_pages));
}

void mark_lmk_victim(struct task_struct *tsk)
{
	struct mm_struct *mm = tsk->mm;

	if (!cmpxchg(&tsk->signal->oom_mm, NULL, mm)) {
		atomic_inc(&tsk->signal->oom_mm->mm_count);
		set_bit(MMF_OOM_VICTIM, &mm->flags);
	}
}

#ifdef CONFIG_HSWAP
static bool reclaim_task_is_ok(int selected_task_anon_size)
{
	int free_size = zram0_free_size() - get_lowest_prio_swapper_space_nrpages();

	if (selected_task_anon_size < free_size)
		return true;

	return false;
}

#define OOM_SCORE_SERVICE_B_ADJ 800
#define OOM_SCORE_CACHED_APP_MIN_ADJ 900

static DEFINE_MUTEX(reclaim_mutex);

static struct completion reclaim_completion;
static struct task_struct *selected_task;

#define RESET_TIME 3600000 /* activity top time reset time(msec) */
static int reset_task_time_thread(void *p)
{
	struct task_struct *tsk;

	while (1) {
		struct task_struct *p;

		rcu_read_lock();
		for_each_process(tsk) {
			if (tsk->flags & PF_KTHREAD)
				continue;

			/* if task no longer has any memory ignore it */
			if (test_task_flag(tsk, TIF_MEMDIE))
				continue;

			if (tsk->exit_state || !tsk->mm)
				continue;

			p = find_lock_task_mm(tsk);
			if (!p)
				continue;

			if (p->signal->top_time)
				p->signal->top_time =
					(p->signal->top_time * 3) / 4;

			task_unlock(p);
		}
		rcu_read_unlock();
		msleep(RESET_TIME);
	}
	return 0;
}

static int reclaim_task_thread(void *p)
{
	int selected_tasksize;
	int efficiency;
	struct reclaim_param rp;

	init_completion(&reclaim_completion);

	while (1) {
		wait_for_completion(&reclaim_completion);

		mutex_lock(&reclaim_mutex);
		if (!selected_task)
			goto reclaim_end;

		lowmem_print(3, "hswap: scheduled reclaim task '%s'(%d), adj%hd\n",
				selected_task->comm, selected_task->pid,
				selected_task->signal->oom_score_adj);

		task_lock(selected_task);
		if (selected_task->exit_state || !selected_task->mm) {
			task_unlock(selected_task);
			put_task_struct(selected_task);
			goto reclaim_end;
		}

		selected_tasksize = get_mm_rss(selected_task->mm);
		if (!selected_tasksize) {
			task_unlock(selected_task);
			put_task_struct(selected_task);
			goto reclaim_end;
		}
		efficiency = selected_task->signal->reclaim_efficiency;
		task_unlock(selected_task);

		rp = reclaim_task_anon(selected_task, selected_tasksize);
		lowmem_print(3, "Reclaimed '%s' (%d), adj %hd,\n" \
				"   nr_reclaimed %d\n",
			     selected_task->comm, selected_task->pid,
			     selected_task->signal->oom_score_adj,
			     rp.nr_reclaimed);
		++lmk_reclaim_cnt;
		if (efficiency)
			efficiency = (efficiency + (rp.nr_reclaimed * 100) / selected_tasksize) / 2;
		else
			efficiency = (rp.nr_reclaimed * 100) / selected_tasksize;
		lowmem_print(3, "Reclaimed efficiency(%s, %d, %d) = %d\n",
				selected_task->comm,
				selected_tasksize,
				rp.nr_reclaimed,
				efficiency);
		selected_task->signal->reclaim_efficiency = efficiency;

		put_task_struct(selected_task);

reclaim_end:
		selected_task = NULL;

		init_completion(&reclaim_completion);
		mutex_unlock(&reclaim_mutex);
	}

	return 0;
}

#define SHRINK_TASK_MAX_CNT 100
#define LOOKING_SERVICE_MAX_CNT 5
struct task_struct* shrink_task[SHRINK_TASK_MAX_CNT];
char killed_task_comm[LOOKING_SERVICE_MAX_CNT][TASK_COMM_LEN];
char pre_killed_task_comm[TASK_COMM_LEN];
static int looking_service_cnt = 0;

struct sorted_task {
	struct task_struct *tp;
	int score;
	int tasksize;
	struct list_head list;
};

struct sorted_task st_by_time[SHRINK_TASK_MAX_CNT];
struct sorted_task st_by_count[SHRINK_TASK_MAX_CNT];
struct sorted_task st_by_memory[SHRINK_TASK_MAX_CNT];

struct list_head stl_by_time;
struct list_head stl_by_count;
struct list_head stl_by_memory;

struct task_struct *calc_hswap_kill_score(int shrink_task_cnt, int *rss_size)
{
	int i, j, k;
	struct sorted_task *cursor;
	struct sorted_task victim_task;
	int is_inserted;
	int high_frequent_kill_task = 0;
	int already_checked = 0;
	unsigned long tasksize;

	INIT_LIST_HEAD(&stl_by_time);
	INIT_LIST_HEAD(&stl_by_count);
	INIT_LIST_HEAD(&stl_by_memory);

	for (i = 0, j = 0; i < shrink_task_cnt; i++) {
		struct sorted_task *stp_by_time;
		struct sorted_task *stp_by_count;
		struct sorted_task *stp_by_memory;
		struct task_struct *task = shrink_task[i];

		task_lock(task);
		if (task->signal->oom_score_adj <= OOM_SCORE_CACHED_APP_MIN_ADJ) {
			if (already_checked || strncmp(task->comm, "earchbox:search", 15) != 0) {
				task_unlock(task);
				continue;
			} else {
				already_checked = 1;
			}
		}

		if (strncmp(task->comm, "dboxed_process0", 15) != 0) {
			if (pre_killed_task_comm[0]) {
				if (!strcmp(pre_killed_task_comm, task->comm)) {
					strcpy(killed_task_comm[looking_service_cnt], task->comm);
					looking_service_cnt = (looking_service_cnt + 1) % LOOKING_SERVICE_MAX_CNT;
					task_unlock(task);
					continue;
				}
			}

			for (k = 0; k < LOOKING_SERVICE_MAX_CNT; k++) {
				if (killed_task_comm[k][0]) {
					if (!strcmp(killed_task_comm[k], task->comm)) {
						high_frequent_kill_task = 1;
						break;
					}
				}
			}
		}

		if (high_frequent_kill_task) {
			lowmem_print(3, "%s: skip high frequent_kill task %s \n", __func__, task->comm);
			high_frequent_kill_task = 0;
			task_unlock(task);
			continue;
		}

		if (task->exit_state || !task->mm) {
			task_unlock(task);
			continue;
		}

		tasksize = get_mm_rss(task->mm);
		if (task->signal->oom_score_adj == OOM_SCORE_CACHED_APP_MIN_ADJ &&
				tasksize < 51200) {
			task_unlock(task);
			continue;
		}
		stp_by_time = &st_by_time[j];
		stp_by_count = &st_by_count[j];
		stp_by_memory = &st_by_memory[j];
		j++;
		INIT_LIST_HEAD(&stp_by_time->list);
		INIT_LIST_HEAD(&stp_by_count->list);
		INIT_LIST_HEAD(&stp_by_memory->list);

		stp_by_time->tp = task;
		stp_by_count->tp = task;
		stp_by_memory->tp = task;
		stp_by_time->score = 0;
		stp_by_count->score = 0;
		stp_by_memory->score = 0;
		stp_by_time->tasksize = tasksize;
		stp_by_count->tasksize = tasksize;
		stp_by_memory->tasksize = tasksize;
		if (list_empty(&stl_by_time) && list_empty(&stl_by_count)
				&& list_empty(&stl_by_memory)) {
			list_add(&stp_by_time->list, &stl_by_time);
			list_add(&stp_by_count->list, &stl_by_count);
			list_add(&stp_by_memory->list, &stl_by_memory);
			task_unlock(task);
			continue;
		}

		is_inserted = 0;
		list_for_each_entry(cursor, &stl_by_time, list) {
			if (stp_by_time->tp->signal->top_time <= cursor->tp->signal->top_time) {
				if (!is_inserted) {
					stp_by_time->score = cursor->score;
					list_add(&stp_by_time->list, cursor->list.prev);
					is_inserted = 1;
				}

				if (stp_by_time->tp->signal->top_time == cursor->tp->signal->top_time)
					break;

				cursor->score++;
			}
			if (list_is_last(&cursor->list, &stl_by_time)) {
				if (!is_inserted) {
					stp_by_time->score = cursor->score + 1;
					list_add(&stp_by_time->list, &cursor->list);
				}
				break;
			}
		}

		is_inserted = 0;
		list_for_each_entry(cursor, &stl_by_count, list) {
			if (stp_by_count->tp->signal->top_count <= cursor->tp->signal->top_count) {
				if (!is_inserted) {
					stp_by_count->score = cursor->score;
					list_add(&stp_by_count->list, cursor->list.prev);
					is_inserted = 1;
				}
				if (stp_by_count->tp->signal->top_count == cursor->tp->signal->top_count)
					break;

				cursor->score++;
			}

			if (list_is_last(&cursor->list, &stl_by_count)) {
				if (!is_inserted) {
					stp_by_count->score = cursor->score + 1;
					list_add(&stp_by_count->list, &cursor->list);
				}
				break;
			}
		}

		is_inserted = 0;
		list_for_each_entry(cursor, &stl_by_memory, list) {
			if (stp_by_memory->tasksize >= cursor->tasksize) {
				if (!is_inserted) {
					stp_by_memory->score = cursor->score;
					list_add(&stp_by_memory->list, cursor->list.prev);
					is_inserted = 1;
				}
				if (stp_by_memory->tasksize == cursor->tasksize)
					break;

				cursor->score++;
			}

			if (list_is_last(&cursor->list, &stl_by_memory)) {
				if (!is_inserted) {
					stp_by_memory->score = cursor->score + 1;
					list_add(&stp_by_memory->list, &cursor->list);
				}
				break;
			}
		}

		task_unlock(task);
	}

	lowmem_print(3, "%s: targeting killing task count = %d\n", __func__, j);
	victim_task.tp = NULL;
	victim_task.score = 0;
	victim_task.tasksize = 0;

	list_for_each_entry(cursor, &stl_by_time, list) {
		trace_lowmemory_kill_task_list(cursor->tp, lmk_kill_cnt);
	}

	for (i = 0 ; i < LOOKING_SERVICE_MAX_CNT; i++) {
		if (killed_task_comm[i][0])
			lowmem_print(3, "%s: abnormal service %s\n", __func__, killed_task_comm[i]);
	}

	while (!list_empty(&stl_by_time)) {
		struct sorted_task *cursor_other;
		struct sorted_task comp_task;
		cursor = list_first_entry(&stl_by_time, struct sorted_task, list);
		list_del(&cursor->list);
		comp_task.tp = NULL;
		comp_task.score = cursor->score;
		comp_task.tasksize = cursor->tasksize;
		list_for_each_entry(cursor_other, &stl_by_count, list) {
			if (cursor->tp->pid == cursor_other->tp->pid) {
				list_del(&cursor_other->list);
				comp_task.tp = cursor_other->tp;
				comp_task.score += cursor_other->score;
				break;
			}
		}

		list_for_each_entry(cursor_other, &stl_by_memory, list) {
			if (cursor->tp->pid == cursor_other->tp->pid) {
				list_del(&cursor_other->list);
				comp_task.tp = cursor_other->tp;
				comp_task.score += cursor_other->score;
				break;
			}
		}

		if (comp_task.tp == NULL)
			BUG();

		if (victim_task.tp == NULL) {
			victim_task.tp = comp_task.tp;
			victim_task.score = comp_task.score;
			victim_task.tasksize = comp_task.tasksize;
			continue;
		}

		if (comp_task.score < victim_task.score) {
			victim_task.tp = comp_task.tp;
			victim_task.score = comp_task.score;
			victim_task.tasksize = comp_task.tasksize;
		} else if (comp_task.score == victim_task.score) {
			if (comp_task.tp->signal->top_time <
					victim_task.tp->signal->top_time) {
				victim_task.tp = comp_task.tp;
				victim_task.tasksize = comp_task.tasksize;
			}
		}
	}

	*rss_size = victim_task.tasksize;
	return victim_task.tp;
}


static struct task_struct *find_suitable_reclaim(int shrink_task_cnt,
		int *rss_size)
{
	struct task_struct *selected = NULL;
	int selected_tasksize = 0;
	int tasksize, anonsize;
	long selected_top_time = -1;
	int i = 0;
	int efficiency = 0;

	for (i = 0; i < shrink_task_cnt; i++) {
		struct task_struct *p;

		p = shrink_task[i];

		task_lock(p);
		if (p->exit_state || !p->mm || p->signal->reclaimed) {
			task_unlock(p);
			continue;
		}

		tasksize = get_mm_rss(p->mm);
		anonsize = get_mm_counter(p->mm, MM_ANONPAGES);
		efficiency = p->signal->reclaim_efficiency;
		task_unlock(p);

		if (!tasksize)
			continue;

		if (!reclaim_task_is_ok(anonsize))
			continue;

		if (efficiency && tasksize > 100)
			tasksize = (tasksize * efficiency) / 100;

		if (selected_tasksize > tasksize)
			continue;

		selected_top_time = p->signal->top_time;
		selected_tasksize = tasksize;
		selected = p;
	}

	*rss_size = selected_tasksize;

	return selected;
}

static struct task_struct *find_suitable_kill_task(int shrink_task_cnt,
		int *rss_size)
{
	struct task_struct *selected = NULL;

	selected = calc_hswap_kill_score(shrink_task_cnt, rss_size);
	if (selected) {
		task_lock(selected);
		if (!(selected->exit_state || !selected->mm)) {
			*rss_size += get_mm_counter(selected->mm, MM_SWAPENTS);
		}
		task_unlock(selected);
	}

	return selected;
}

static void reclaim_arr_free(int shrink_task_cnt)
{
	int i;

	for (i = 0; i < shrink_task_cnt; i++)
		shrink_task[i] = NULL;
}

static unsigned long before_called_ts = 0;
int is_first_latency = 1;
#define TIME_ARR_SIZE  100
static int time_arr_size = 3;
static long arr_ts[TIME_ARR_SIZE] = {0, };
static int ts_idx = 0;
static long avg_treshold = 100;

static long calc_ts_avg(long *arr_ts, int arr_size)
{
	long avg = 0;
	int i = 0;

	for (; i < arr_size; i++) {
		avg += arr_ts[i];
	}

	return (avg / arr_size);
}

static int reset_latency(void)
{
	int i = 0;

	for (i = 0; i < time_arr_size; i++)
		arr_ts[i] = -1;
	ts_idx = 0;
	before_called_ts = 0;
	is_first_latency = 1;

	return 0;
}

static long get_lmk_latency(short min_score_adj)
{
	unsigned int timediff_ms;

	if (min_score_adj <= 900) {
		int arr_size = 0;
		if (is_first_latency) {
			before_called_ts = jiffies;
			is_first_latency = 0;
		} else {
			timediff_ms = jiffies_to_msecs(jiffies - before_called_ts);
			before_called_ts = jiffies;
			arr_ts[ts_idx++] = timediff_ms;
			ts_idx %= time_arr_size;
			if (arr_ts[ts_idx] == -1)
				return -1;
			else
				arr_size = time_arr_size;
			return calc_ts_avg(arr_ts, arr_size);
		}
	} else {
		reset_latency();
	}

	return -1;
}

static enum alloc_pressure check_memory_allocation_pressure(short min_score_adj)
{
	long avg_latency = 0;
	if (!current_is_kswapd()) {
		lowmem_print(3, "It's direct reclaim\n");
		return PRESSURE_HIGH;
	}


	avg_latency = get_lmk_latency(min_score_adj);
	if (avg_latency > 0 && avg_latency < avg_treshold) {
		lowmem_print(3, "Check Latency %ldmsec\n", avg_latency);
		reset_latency();
		return PRESSURE_HIGH;
	}

	return PRESSURE_NORMAL;
}
#endif

static void send_sig_group(int sig, struct task_struct *selected, int priv) {
    struct task_struct *child;
    list_for_each_entry(child, &selected->children, sibling) {
        send_sig_group(sig, child, priv);
        send_sig(sig, child, priv);
        lowmem_print(1, "Killing Child '%s' (%d)\n", child->comm, child->pid);
    }
}

static unsigned long lowmem_scan(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	unsigned long rem = 0;
	int tasksize;
	int i;
	int ret = 0;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
	int scale_percent;
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	bool lock_required = true;
	lmk_inc_stats(LMK_SCAN);
#ifdef CONFIG_HSWAP
	int reclaimed_cnt = 0, reclaimable_cnt = 0, shrink_task_cnt = 0;
	int hswap_tasksize = 0;
	int swapsize = 0, selected_swapsize = 0;
	struct task_struct *hswap_kill_selected = NULL;
	int kill_reason = KILL_LMK;
#endif

	other_free = global_zone_page_state(NR_FREE_PAGES);

#ifdef CONFIG_MIGRATE_HIGHORDER
	other_free -= global_zone_page_state(NR_FREE_HIGHORDER_PAGES);
#endif

	if (global_node_page_state(NR_SHMEM) + total_swapcache_pages() +
			global_node_page_state(NR_UNEVICTABLE) <
			global_node_page_state(NR_FILE_PAGES))
		other_file = global_node_page_state(NR_FILE_PAGES) -
					global_node_page_state(NR_SHMEM) -
					global_node_page_state(NR_UNEVICTABLE) -
					total_swapcache_pages();
	else
		other_file = 0;

#ifndef CONFIG_HSWAP
	if (!get_nr_swap_pages() && (other_free <= lowmem_minfree[0] >> 1) &&
	    (other_file <= lowmem_minfree[0] >> 1))
		lock_required = false;
#endif

	if (likely(lock_required) && !mutex_trylock(&scan_mutex)) {
#ifdef LMK_TNG_ENABLE_TRACE
		trace_lmk_remain_scan(0, sc->nr_to_scan, sc->gfp_mask);
#endif
		lmk_inc_stats(LMK_BUSY);
		return 0;
        }

#ifdef CONFIG_HSWAP
	if (!mutex_trylock(&reclaim_mutex)) {
		if (likely(lock_required))
			mutex_unlock(&scan_mutex);
		return 0;
	}
	mutex_unlock(&reclaim_mutex);
#endif

#ifndef CONFIG_HSWAP
	tune_lmk_param(&other_free, &other_file, sc);
#endif

	scale_percent = get_minfree_scalefactor(sc->gfp_mask);
	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
#ifndef CONFIG_HSWAP
		minfree = mult_frac(lowmem_minfree[i], scale_percent, 100);
#else
		minfree = lowmem_minfree[i];
#endif
		if (other_free < minfree && other_file < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}

	ret = adjust_minadj(&min_score_adj);

	lowmem_print(3, "%s %lu, %x, ofree %d %d, ma %hd\n",
		     __func__, sc->nr_to_scan, sc->gfp_mask, other_free,
		     other_file, min_score_adj);

	if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		trace_almk_shrink(0, ret, other_free, other_file, 0);
		lowmem_print(5, "%s %lu, %x, return 0\n",
			     __func__, sc->nr_to_scan, sc->gfp_mask);
#ifdef LMK_TNG_ENABLE_TRACE
		trace_lmk_remain_scan(0, sc->nr_to_scan, sc->gfp_mask);
#endif
		lmk_inc_stats(LMK_NO_KILL);
		if (lock_required)
			mutex_unlock(&scan_mutex);
		return 0;
	}

	selected_oom_score_adj = min_score_adj;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		/* Ignore task if coredump in progress */
		if (tsk->mm && tsk->mm->core_state)
			continue;

		if (oom_reaper) {
			p = find_lock_task_mm(tsk);
			if (!p)
				continue;

			if (test_bit(MMF_OOM_VICTIM, &p->mm->flags)) {
				if (test_bit(MMF_OOM_SKIP, &p->mm->flags)) {
					task_unlock(p);
					continue;
				} else if (time_before_eq(jiffies,
						lowmem_deathpending_timeout)) {
					task_unlock(p);
					rcu_read_unlock();
#ifdef CONFIG_HSWAP
					rem = SHRINK_STOP;
					goto end_lmk;
#endif
					if (lock_required)
						mutex_unlock(&scan_mutex);
#ifdef LMK_TNG_ENABLE_TRACE
					trace_lmk_remain_scan(0, sc->nr_to_scan,
							      sc->gfp_mask);
#endif
					lmk_inc_stats(LMK_TIMEOUT);
					return 0;
				}
			}
		} else {
			if (time_before_eq(jiffies,
					   lowmem_deathpending_timeout))
				if (test_task_lmk_waiting(tsk)) {
					rcu_read_unlock();
#ifdef CONFIG_HSWAP
					goto end_lmk;
#endif
					if (lock_required)
						mutex_unlock(&scan_mutex);
#ifdef LMK_TNG_ENABLE_TRACE
					trace_lmk_remain_scan(0, sc->nr_to_scan,
							      sc->gfp_mask);
#endif
					lmk_inc_stats(LMK_TIMEOUT);
					return 0;
				}

			p = find_lock_task_mm(tsk);
			if (!p)
				continue;
		}

		oom_score_adj = p->signal->oom_score_adj;
#ifdef CONFIG_HSWAP
		if (p->signal->reclaimed)
			reclaimed_cnt++;

		if (oom_score_adj >= OOM_SCORE_SERVICE_B_ADJ) {
			if (shrink_task_cnt < SHRINK_TASK_MAX_CNT)
				shrink_task[shrink_task_cnt++] = p;
			if (!p->signal->reclaimed)
				reclaimable_cnt++;
		}
#endif
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}

		tasksize = get_mm_rss(p->mm);
#ifdef CONFIG_HSWAP
		swapsize = get_mm_counter(p->mm, MM_SWAPENTS);
#endif
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_score_adj = oom_score_adj;
#ifdef CONFIG_HSWAP
		selected_swapsize = swapsize;
#endif
		lowmem_print(3, "select '%s' (%d), adj %hd, size %d, to kill\n",
			     p->comm, p->pid, oom_score_adj, tasksize);
	}
	if (selected) {
		long cache_size = other_file * (long)(PAGE_SIZE / 1024);
		long cache_limit = minfree * (long)(PAGE_SIZE / 1024);
		long free = other_free * (long)(PAGE_SIZE / 1024);
		struct task_struct *parent;

		if (test_task_lmk_waiting(selected) &&
		    (test_task_state(selected, TASK_UNINTERRUPTIBLE))) {
			lowmem_print(2, "'%s' (%d) is already killed\n",
				     selected->comm,
				     selected->pid);
			rcu_read_unlock();
			if (lock_required)
				mutex_unlock(&scan_mutex);
#ifdef LMK_TNG_ENABLE_TRACE
			trace_lmk_remain_scan(0, sc->nr_to_scan, sc->gfp_mask);
#endif
			lmk_inc_stats(LMK_BUSY);
			return 0;
		}

#ifdef CONFIG_HSWAP
		if (min_score_adj < OOM_SCORE_SERVICE_B_ADJ) {
			selected_tasksize += selected_swapsize;
			goto hswap_kill;
		}

		if (check_memory_allocation_pressure(min_score_adj) == PRESSURE_HIGH) {
			kill_reason = KILL_MEMORY_PRESSURE;
			lowmem_print(3, "Memory Alloctions is High\n");
			goto hswap_kill;
		}

		if (!reclaimable_cnt &&
				(min_score_adj > OOM_SCORE_CACHED_APP_MIN_ADJ)) {
			rem = SHRINK_STOP;
			rcu_read_unlock();
			goto end_lmk;
		}

		if (reclaimable_cnt && selected_task == NULL && mutex_trylock(&reclaim_mutex)) {
			selected_task = find_suitable_reclaim(shrink_task_cnt, &hswap_tasksize);
			if (selected_task) {
				unsigned long flags;

				if (lock_task_sighand(selected_task, &flags)) {
					selected_task->signal->reclaimed = 1;
					unlock_task_sighand(selected_task, &flags);
				}
				get_task_struct(selected_task);
				complete(&reclaim_completion);
				rem += hswap_tasksize;
				lowmem_print(1, "Reclaiming '%s' (%d), adj %hd,\n" \
						"   top time = %ld, top count %d,\n" \
						"   to free %ldkB on behalf of '%s' (%d) because\n" \
						"   cache %ldkB is below limit %ldkB for oom_score_adj %hd\n" \
						"   Free memory is %ldkB above reserved.\n",
						selected_task->comm, selected_task->pid,
						selected_task->signal->oom_score_adj,
						selected_task->signal->top_time,
						selected_task->signal->top_count,
						hswap_tasksize * (long)(PAGE_SIZE / 1024),
						current->comm, current->pid,
						other_file * (long)(PAGE_SIZE / 1024),
						minfree * (long)(PAGE_SIZE / 1024),
						min_score_adj,
						other_free * (long)(PAGE_SIZE / 1024));
				lowmem_print(3, "reclaimed cnt = %d, reclaimable cont = %d, min oom score= %hd\n",
						reclaimed_cnt, reclaimable_cnt, min_score_adj);
				mutex_unlock(&reclaim_mutex);
				lowmem_deathpending_timeout = jiffies + HZ;
				rcu_read_unlock();
				msleep_interruptible(5);
				goto end_lmk;
			} else {
				mutex_unlock(&reclaim_mutex);
				kill_reason = KILL_SWAP_FULL;
			}
		} else {
			if (!reclaimable_cnt)
				kill_reason = KILL_NO_RECLAIMABLE;
			else
				kill_reason = KILL_RECLAIMING;
		}

hswap_kill:
		if (shrink_task_cnt > 0) {
			hswap_kill_selected = find_suitable_kill_task(shrink_task_cnt, &selected_tasksize);
			if (hswap_kill_selected)
				selected = hswap_kill_selected;
		}
#endif
		task_lock(selected);
		send_sig(SIGKILL, selected, 0);

		/* kill child processes when zygote is parent
		 * some apps use fork, remain child proc causes breaks AM's work
		 */
		parent = rcu_dereference(selected->parent);
		if (parent) {
			if (strcmp(parent->comm, "main") == 0) {
				send_sig_group(SIGKILL, selected, 0);
			}
		}

		if (selected->mm) {
			task_set_lmk_waiting(selected);
			if (!test_bit(MMF_OOM_SKIP, &selected->mm->flags) &&
			    oom_reaper) {
				mark_lmk_victim(selected);
				wake_oom_reaper(selected);
			}
		}
		task_unlock(selected);
		trace_lowmemory_kill(selected, cache_size, cache_limit, free);
#ifndef CONFIG_HSWAP
		lowmem_print(1, "Killing '%s' (%d) (tgid %d), adj %hd,\n"
#else
		lowmem_print(1, "%s '%s' (%d) (tgid %d), adj %hd, \n"
			"reclaim_cnt %d, top (%ld, %d), reason %s\n"
#endif
			"to free %ldkB on behalf of '%s' (%d) because\n"
			"cache %ldkB is below limit %ldkB for oom score %hd\n"
			"Free memory is %ldkB above reserved.\n"
			"Free CMA is %ldkB\n"
			"Total reserve is %ldkB\n"
			"Total free pages is %ldkB\n"
			"Total file cache is %ldkB\n"
			"GFP mask is 0x%x\n",
#ifdef CONFIG_HSWAP
			hswap_kill_selected ? "HSWAP Killing" : "Orig Killing",
#endif
			selected->comm, selected->pid, selected->tgid,
			selected_oom_score_adj,
#ifdef CONFIG_HSWAP
			reclaimable_cnt,
			selected->signal->top_time,
			selected->signal->top_count,
			kill_reason_str[kill_reason],
#endif
			selected_tasksize * (long)(PAGE_SIZE / 1024),
			current->comm, current->pid,
			cache_size, cache_limit,
			min_score_adj,
			free,
			global_zone_page_state(NR_FREE_CMA_PAGES) *
			(long)(PAGE_SIZE / 1024),
			totalreserve_pages * (long)(PAGE_SIZE / 1024),
			global_zone_page_state(NR_FREE_PAGES) *
			(long)(PAGE_SIZE / 1024),
			global_node_page_state(NR_FILE_PAGES) *
			(long)(PAGE_SIZE / 1024),
			sc->gfp_mask);

		if (lowmem_debug_level >= 2 && selected_oom_score_adj == 0) {
			show_mem(SHOW_MEM_FILTER_NODES, NULL);
			show_mem_call_notifiers();
			dump_tasks(NULL, NULL);
		}

		lowmem_deathpending_timeout = jiffies + HZ;
		rem += selected_tasksize;
#ifdef LMK_TNG_ENABLE_TRACE
		trace_lmk_sigkill(selected->pid, selected->comm,
				  selected_oom_score_adj, selected_tasksize,
				  sc->gfp_mask);
#endif
#ifdef CONFIG_HSWAP
		if (kill_reason == KILL_MEMORY_PRESSURE)
			rem = SHRINK_STOP;

		lowmem_print(3, "reclaimed cnt = %d, reclaim cont = %d, min oom score= %hd\n",
				reclaimed_cnt, reclaimable_cnt, min_score_adj);
#endif
		++lmk_kill_cnt;
		rcu_read_unlock();
		/* give the system time to free up the memory */
		msleep_interruptible(20);
		trace_almk_shrink(selected_tasksize, ret,
				  other_free, other_file,
				  selected_oom_score_adj);
		lmk_inc_stats(LMK_KILL);
	} else {
		trace_almk_shrink(1, ret, other_free, other_file, 0);
		lmk_inc_stats(LMK_WASTE);
		rcu_read_unlock();
	}

#ifdef CONFIG_HSWAP
end_lmk:
	reclaim_arr_free(shrink_task_cnt);
#endif
	lowmem_print(4, "%s %lu, %x, return %lu\n",
		     __func__, sc->nr_to_scan, sc->gfp_mask, rem);
	if (lock_required)
		mutex_unlock(&scan_mutex);
#ifdef LMK_TNG_ENABLE_TRACE
	trace_lmk_remain_scan(rem, sc->nr_to_scan, sc->gfp_mask);
#endif
	return rem;
}

static int lmk_hotplug_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	switch (action) {
	case MEM_GOING_OFFLINE:
		if (enable_adaptive_lmk == ADAPTIVE_LMK_ENABLED)
			enable_adaptive_lmk = ADAPTIVE_LMK_WAS_ENABLED;
		break;
	case MEM_OFFLINE:
		if (enable_adaptive_lmk == ADAPTIVE_LMK_WAS_ENABLED)
			enable_adaptive_lmk = ADAPTIVE_LMK_ENABLED;
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct shrinker lowmem_shrinker = {
	.scan_objects = lowmem_scan,
	.count_objects = lowmem_count,
	.seeks = DEFAULT_SEEKS * 16
};

static struct notifier_block lmk_memory_callback_nb = {
	.notifier_call = lmk_hotplug_callback,
	.priority = 0,
};

static int __init lowmem_init(void)
{
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_TNG
	lowmem_init_tng(&lowmem_shrinker);
#endif
#ifdef CONFIG_HSWAP
	struct task_struct *reclaim_tsk;
	struct task_struct *reset_top_time_tsk;
	int i = 0;

	reclaim_tsk = kthread_run(reclaim_task_thread, NULL, "reclaim_task");
	reset_top_time_tsk = kthread_run(reset_task_time_thread, NULL, "reset_task");

	for (; i < TIME_ARR_SIZE; i++)
		arr_ts[i] = -1;
#endif
	register_shrinker(&lowmem_shrinker);
	vmpressure_notifier_register(&lmk_vmpr_nb);
	if (register_hotmemory_notifier(&lmk_memory_callback_nb))
		lowmem_print(1, "Registering memory hotplug notifier failed\n");
	init_procfs_lmk();
	return 0;
}
device_initcall(lowmem_init);

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	short oom_adj;
	short oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param_named(cost, lowmem_shrinker.seeks, int, 0644);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
module_param_cb(adj, &lowmem_adj_array_ops,
		.arr = &__param_arr_adj,
		0644);
__MODULE_PARM_TYPE(adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size, 0644);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmk_fast_run, lmk_fast_run, int, S_IRUGO | S_IWUSR);
module_param_named(lmk_kill_cnt, lmk_kill_cnt, int, S_IRUGO);
#ifdef CONFIG_HSWAP
module_param_named(lmk_reclaim_cnt, lmk_reclaim_cnt, int, S_IRUGO);
#endif
