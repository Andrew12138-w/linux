#ifndef BLK_THROTTLE_H
#define BLK_THROTTLE_H
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/blktrace_api.h>
#include <linux/blk-cgroup.h>
#include "blk.h"
#include "blk-cgroup-rwstat.h"

/* Max dispatch from a group in 1 round */
#define THROTL_GRP_QUANTUM 8

/* Total max dispatch from all groups in one round */
#define THROTL_QUANTUM 32

/* Throttling is performed over a slice and after that slice is renewed */
#define DFL_THROTL_SLICE_HD (HZ / 10)
#define DFL_THROTL_SLICE_SSD (HZ / 50)
#define MAX_THROTL_SLICE (HZ)
#define MAX_IDLE_TIME (5L * 1000 * 1000) /* 5 s */
#define MIN_THROTL_BPS (320 * 1024)
#define MIN_THROTL_IOPS (10)
#define DFL_LATENCY_TARGET (-1L)
#define DFL_IDLE_THRESHOLD (0)
#define DFL_HD_BASELINE_LATENCY (4000L) /* 4ms */
#define LATENCY_FILTERED_SSD (0)
/*
 * For HD, very small latency comes from sequential IO. Such IO is helpless to
 * help determine if its IO is impacted by others, hence we ignore the IO
 */
#define LATENCY_FILTERED_HD (1000L) /* 1ms */

/* A workqueue to queue throttle related work */

/*
 * To implement hierarchical throttling, throtl_grps form a tree and bios
 * are dispatched upwards level by level until they reach the top and get
 * issued.  When dispatching bios from the children and local group at each
 * level, if the bios are dispatched into a single bio_list, there's a risk
 * of a local or child group which can queue many bios at once filling up
 * the list starving others.
 *
 * To avoid such starvation, dispatched bios are queued separately
 * according to where they came from.  When they are again dispatched to
 * the parent, they're popped in round-robin order so that no single source
 * hogs the dispatch window.
 *
 * throtl_qnode is used to keep the queued bios separated by their sources.
 * Bios are queued to throtl_qnode which in turn is queued to
 * throtl_service_queue and then dispatched in round-robin order.
 *
 * It's also used to track the reference counts on blkg's.  A qnode always
 * belongs to a throtl_grp and gets queued on itself or the parent, so
 * incrementing the reference of the associated throtl_grp when a qnode is
 * queued and decrementing when dequeued is enough to keep the whole blkg
 * tree pinned while bios are in flight.
 */
struct throtl_qnode {
	struct list_head node; /* service_queue->queued[] */
	struct bio_list bios; /* queued bios */
	struct throtl_grp *tg; /* tg this qnode belongs to */
};

struct wyz_bio_list_node {
	struct list_head node;
	struct bio *bio;
};

struct throtl_service_queue {
	struct throtl_service_queue *parent_sq; /* the parent service_queue */

	/*
	 * Bios queued directly to this service_queue or dispatched from
	 * children throtl_grp's.
	 */
	struct list_head queued[2]; /* throtl_qnode [READ/WRITE] */
	unsigned int nr_queued[2]; /* number of queued bios */

	/*
	 * RB tree of active children throtl_grp's, which are sorted by
	 * their ->disptime.
	 */
	struct rb_root_cached pending_tree; /* RB tree of active tgs */
	unsigned int nr_pending; /* # queued in the tree */
	unsigned long first_pending_disptime; /* disptime of the first tg */
	struct timer_list pending_timer; /* fires on first_pending_disptime */
};

enum tg_state_flags {
	THROTL_TG_PENDING = 1 << 0, /* on parent's pending tree */
	THROTL_TG_WAS_EMPTY = 1 << 1, /* bio_lists[] became non-empty */
};

#define rb_entry_tg(node) rb_entry((node), struct throtl_grp, rb_node)

enum {
	LIMIT_LOW,
	LIMIT_MAX,
	LIMIT_CNT,
};

struct throtl_grp {
	/* must be the first member */
	struct blkg_policy_data pd;

	/* active throtl group service_queue member */
	struct rb_node rb_node;

	/* throtl_data this group belongs to */
	struct throtl_data *td;

	/* this group's service queue */
	struct throtl_service_queue service_queue;

	/*
	 * qnode_on_self is used when bios are directly queued to this
	 * throtl_grp so that local bios compete fairly with bios
	 * dispatched from children.  qnode_on_parent is used when bios are
	 * dispatched from this throtl_grp into its parent and will compete
	 * with the sibling qnode_on_parents and the parent's
	 * qnode_on_self.
	 */
	struct throtl_qnode qnode_on_self[2];
	struct throtl_qnode qnode_on_parent[2];

	/*
	 * Dispatch time in jiffies. This is the estimated time when group
	 * will unthrottle and is ready to dispatch more bio. It is used as
	 * key to sort active groups in service tree.
	 */
	unsigned long disptime;

	unsigned int flags;

	/* are there any throtl rules between this group and td? */
	bool has_rules[2];

	/* internally used bytes per second rate limits */
	uint64_t bps[2][LIMIT_CNT];
	/* user configured bps limits */
	uint64_t bps_conf[2][LIMIT_CNT];

	/* internally used IOPS limits */
	unsigned int iops[2][LIMIT_CNT];
	/* user configured IOPS limits */
	unsigned int iops_conf[2][LIMIT_CNT];

	/* Number of bytes dispatched in current slice */
	uint64_t bytes_disp[2];
	/* Number of bio's dispatched in current slice */
	unsigned int io_disp[2];

	unsigned long last_low_overflow_time[2];

	uint64_t last_bytes_disp[2];
	unsigned int last_io_disp[2];

	unsigned long last_check_time;

	unsigned long latency_target; /* us */
	unsigned long latency_target_conf; /* us */
	/* When did we start a new slice */
	unsigned long slice_start[2];
	unsigned long slice_end[2];

	unsigned long last_finish_time; /* ns / 1024 */
	unsigned long checked_last_finish_time; /* ns / 1024 */
	unsigned long avg_idletime; /* ns / 1024 */
	unsigned long idletime_threshold; /* us */
	unsigned long idletime_threshold_conf; /* us */

	unsigned int bio_cnt; /* total bios */
	unsigned int bad_bio_cnt; /* bios exceeding latency threshold */
	unsigned long bio_cnt_reset_time;

	atomic_t io_split_cnt[2];
	atomic_t last_io_split_cnt[2];

	struct blkg_rwstat stat_bytes;
	struct blkg_rwstat stat_ios;

	struct list_head wyz_queued[2];
	struct throtl_qnode wyz_qnode_on_self[2];
	struct wyz_bio_list_node wyz_node[2];
	struct work_struct wyz_dispatch_work;
};

#define IO_SCHED
#include <linux/bpf_sched.h>
#undef IO_SCHED

/* We measure latency for request size from <= 4k to >= 1M */
#define LATENCY_BUCKET_SIZE 9

struct latency_bucket {
	unsigned long total_latency; /* ns / 1024 */
	int samples;
};

struct avg_latency_bucket {
	unsigned long latency; /* ns / 1024 */
	bool valid;
};

struct throtl_data {
	/* service tree for active throtl groups */
	struct throtl_service_queue service_queue;

	struct request_queue *queue;

	/* Total Number of queued bios on READ and WRITE lists */
	unsigned int nr_queued[2];

	unsigned int throtl_slice;

	/* Work for dispatching throttled bios */
	struct work_struct dispatch_work;
	unsigned int limit_index;
	bool limit_valid[LIMIT_CNT];

	unsigned long low_upgrade_time;
	unsigned long low_downgrade_time;

	unsigned int scale;

	struct latency_bucket tmp_buckets[2][LATENCY_BUCKET_SIZE];
	struct avg_latency_bucket avg_buckets[2][LATENCY_BUCKET_SIZE];
	struct latency_bucket __percpu *latency_buckets[2];
	unsigned long last_calculate_time;
	unsigned long filtered_latency;

	bool track_bio_latency;
};

#endif