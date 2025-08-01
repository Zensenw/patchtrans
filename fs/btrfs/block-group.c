// SPDX-License-Identifier: GPL-2.0

#include <linux/sizes.h>
#include <linux/list_sort.h>
#include "misc.h"
#include "ctree.h"
#include "block-group.h"
#include "space-info.h"
#include "disk-io.h"
#include "free-space-cache.h"
#include "free-space-tree.h"
#include "volumes.h"
#include "transaction.h"
#include "ref-verify.h"
#include "sysfs.h"
#include "tree-log.h"
#include "delalloc-space.h"
#include "discard.h"
#include "raid56.h"
#include "zoned.h"
#include "fs.h"
#include "accessors.h"
#include "extent-tree.h"

#ifdef CONFIG_BTRFS_DEBUG
int btrfs_should_fragment_free_space(struct btrfs_block_group *block_group)
{
	struct btrfs_fs_info *fs_info = block_group->fs_info;

	return (btrfs_test_opt(fs_info, FRAGMENT_METADATA) &&
		block_group->flags & BTRFS_BLOCK_GROUP_METADATA) ||
	       (btrfs_test_opt(fs_info, FRAGMENT_DATA) &&
		block_group->flags &  BTRFS_BLOCK_GROUP_DATA);
}
#endif

/*
 * Return target flags in extended format or 0 if restripe for this chunk_type
 * is not in progress
 *
 * Should be called with balance_lock held
 */
static u64 get_restripe_target(struct btrfs_fs_info *fs_info, u64 flags)
{
	struct btrfs_balance_control *bctl = fs_info->balance_ctl;
	u64 target = 0;

	if (!bctl)
		return 0;

	if (flags & BTRFS_BLOCK_GROUP_DATA &&
	    bctl->data.flags & BTRFS_BALANCE_ARGS_CONVERT) {
		target = BTRFS_BLOCK_GROUP_DATA | bctl->data.target;
	} else if (flags & BTRFS_BLOCK_GROUP_SYSTEM &&
		   bctl->sys.flags & BTRFS_BALANCE_ARGS_CONVERT) {
		target = BTRFS_BLOCK_GROUP_SYSTEM | bctl->sys.target;
	} else if (flags & BTRFS_BLOCK_GROUP_METADATA &&
		   bctl->meta.flags & BTRFS_BALANCE_ARGS_CONVERT) {
		target = BTRFS_BLOCK_GROUP_METADATA | bctl->meta.target;
	}

	return target;
}

/*
 * @flags: available profiles in extended format (see ctree.h)
 *
 * Return reduced profile in chunk format.  If profile changing is in progress
 * (either running or paused) picks the target profile (if it's already
 * available), otherwise falls back to plain reducing.
 */
static u64 btrfs_reduce_alloc_profile(struct btrfs_fs_info *fs_info, u64 flags)
{
	u64 num_devices = fs_info->fs_devices->rw_devices;
	u64 target;
	u64 raid_type;
	u64 allowed = 0;

	/*
	 * See if restripe for this chunk_type is in progress, if so try to
	 * reduce to the target profile
	 */
	spin_lock(&fs_info->balance_lock);
	target = get_restripe_target(fs_info, flags);
	if (target) {
		spin_unlock(&fs_info->balance_lock);
		return extended_to_chunk(target);
	}
	spin_unlock(&fs_info->balance_lock);

	/* First, mask out the RAID levels which aren't possible */
	for (raid_type = 0; raid_type < BTRFS_NR_RAID_TYPES; raid_type++) {
		if (num_devices >= btrfs_raid_array[raid_type].devs_min)
			allowed |= btrfs_raid_array[raid_type].bg_flag;
	}
	allowed &= flags;

	/* Select the highest-redundancy RAID level. */
	if (allowed & BTRFS_BLOCK_GROUP_RAID1C4)
		allowed = BTRFS_BLOCK_GROUP_RAID1C4;
	else if (allowed & BTRFS_BLOCK_GROUP_RAID6)
		allowed = BTRFS_BLOCK_GROUP_RAID6;
	else if (allowed & BTRFS_BLOCK_GROUP_RAID1C3)
		allowed = BTRFS_BLOCK_GROUP_RAID1C3;
	else if (allowed & BTRFS_BLOCK_GROUP_RAID5)
		allowed = BTRFS_BLOCK_GROUP_RAID5;
	else if (allowed & BTRFS_BLOCK_GROUP_RAID10)
		allowed = BTRFS_BLOCK_GROUP_RAID10;
	else if (allowed & BTRFS_BLOCK_GROUP_RAID1)
		allowed = BTRFS_BLOCK_GROUP_RAID1;
	else if (allowed & BTRFS_BLOCK_GROUP_DUP)
		allowed = BTRFS_BLOCK_GROUP_DUP;
	else if (allowed & BTRFS_BLOCK_GROUP_RAID0)
		allowed = BTRFS_BLOCK_GROUP_RAID0;

	flags &= ~BTRFS_BLOCK_GROUP_PROFILE_MASK;

	return extended_to_chunk(flags | allowed);
}

u64 btrfs_get_alloc_profile(struct btrfs_fs_info *fs_info, u64 orig_flags)
{
	unsigned seq;
	u64 flags;

	do {
		flags = orig_flags;
		seq = read_seqbegin(&fs_info->profiles_lock);

		if (flags & BTRFS_BLOCK_GROUP_DATA)
			flags |= fs_info->avail_data_alloc_bits;
		else if (flags & BTRFS_BLOCK_GROUP_SYSTEM)
			flags |= fs_info->avail_system_alloc_bits;
		else if (flags & BTRFS_BLOCK_GROUP_METADATA)
			flags |= fs_info->avail_metadata_alloc_bits;
	} while (read_seqretry(&fs_info->profiles_lock, seq));

	return btrfs_reduce_alloc_profile(fs_info, flags);
}

void btrfs_get_block_group(struct btrfs_block_group *cache)
{
	refcount_inc(&cache->refs);
}

void btrfs_put_block_group(struct btrfs_block_group *cache)
{
	if (refcount_dec_and_test(&cache->refs)) {
		WARN_ON(cache->pinned > 0);
		/*
		 * If there was a failure to cleanup a log tree, very likely due
		 * to an IO failure on a writeback attempt of one or more of its
		 * extent buffers, we could not do proper (and cheap) unaccounting
		 * of their reserved space, so don't warn on reserved > 0 in that
		 * case.
		 */
		if (!(cache->flags & BTRFS_BLOCK_GROUP_METADATA) ||
		    !BTRFS_FS_LOG_CLEANUP_ERROR(cache->fs_info))
			WARN_ON(cache->reserved > 0);

		/*
		 * A block_group shouldn't be on the discard_list anymore.
		 * Remove the block_group from the discard_list to prevent us
		 * from causing a panic due to NULL pointer dereference.
		 */
		if (WARN_ON(!list_empty(&cache->discard_list)))
			btrfs_discard_cancel_work(&cache->fs_info->discard_ctl,
						  cache);

		kfree(cache->free_space_ctl);
		btrfs_free_chunk_map(cache->physical_map);
		kfree(cache);
	}
}

/*
 * This adds the block group to the fs_info rb tree for the block group cache
 */
static int btrfs_add_block_group_cache(struct btrfs_fs_info *info,
				       struct btrfs_block_group *block_group)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct btrfs_block_group *cache;
	bool leftmost = true;

	ASSERT(block_group->length != 0);

	write_lock(&info->block_group_cache_lock);
	p = &info->block_group_cache_tree.rb_root.rb_node;

	while (*p) {
		parent = *p;
		cache = rb_entry(parent, struct btrfs_block_group, cache_node);
		if (block_group->start < cache->start) {
			p = &(*p)->rb_left;
		} else if (block_group->start > cache->start) {
			p = &(*p)->rb_right;
			leftmost = false;
		} else {
			write_unlock(&info->block_group_cache_lock);
			return -EEXIST;
		}
	}

	rb_link_node(&block_group->cache_node, parent, p);
	rb_insert_color_cached(&block_group->cache_node,
			       &info->block_group_cache_tree, leftmost);

	write_unlock(&info->block_group_cache_lock);

	return 0;
}

/*
 * This will return the block group at or after bytenr if contains is 0, else
 * it will return the block group that contains the bytenr
 */
static struct btrfs_block_group *block_group_cache_tree_search(
		struct btrfs_fs_info *info, u64 bytenr, int contains)
{
	struct btrfs_block_group *cache, *ret = NULL;
	struct rb_node *n;
	u64 end, start;

	read_lock(&info->block_group_cache_lock);
	n = info->block_group_cache_tree.rb_root.rb_node;

	while (n) {
		cache = rb_entry(n, struct btrfs_block_group, cache_node);
		end = cache->start + cache->length - 1;
		start = cache->start;

		if (bytenr < start) {
			if (!contains && (!ret || start < ret->start))
				ret = cache;
			n = n->rb_left;
		} else if (bytenr > start) {
			if (contains && bytenr <= end) {
				ret = cache;
				break;
			}
			n = n->rb_right;
		} else {
			ret = cache;
			break;
		}
	}
	if (ret)
		btrfs_get_block_group(ret);
	read_unlock(&info->block_group_cache_lock);

	return ret;
}

/*
 * Return the block group that starts at or after bytenr
 */
struct btrfs_block_group *btrfs_lookup_first_block_group(
		struct btrfs_fs_info *info, u64 bytenr)
{
	return block_group_cache_tree_search(info, bytenr, 0);
}

/*
 * Return the block group that contains the given bytenr
 */
struct btrfs_block_group *btrfs_lookup_block_group(
		struct btrfs_fs_info *info, u64 bytenr)
{
	return block_group_cache_tree_search(info, bytenr, 1);
}

struct btrfs_block_group *btrfs_next_block_group(
		struct btrfs_block_group *cache)
{
	struct btrfs_fs_info *fs_info = cache->fs_info;
	struct rb_node *node;

	read_lock(&fs_info->block_group_cache_lock);

	/* If our block group was removed, we need a full search. */
	if (RB_EMPTY_NODE(&cache->cache_node)) {
		const u64 next_bytenr = cache->start + cache->length;

		read_unlock(&fs_info->block_group_cache_lock);
		btrfs_put_block_group(cache);
		return btrfs_lookup_first_block_group(fs_info, next_bytenr);
	}
	node = rb_next(&cache->cache_node);
	btrfs_put_block_group(cache);
	if (node) {
		cache = rb_entry(node, struct btrfs_block_group, cache_node);
		btrfs_get_block_group(cache);
	} else
		cache = NULL;
	read_unlock(&fs_info->block_group_cache_lock);
	return cache;
}

/*
 * Check if we can do a NOCOW write for a given extent.
 *
 * @fs_info:       The filesystem information object.
 * @bytenr:        Logical start address of the extent.
 *
 * Check if we can do a NOCOW write for the given extent, and increments the
 * number of NOCOW writers in the block group that contains the extent, as long
 * as the block group exists and it's currently not in read-only mode.
 *
 * Returns: A non-NULL block group pointer if we can do a NOCOW write, the caller
 *          is responsible for calling btrfs_dec_nocow_writers() later.
 *
 *          Or NULL if we can not do a NOCOW write
 */
struct btrfs_block_group *btrfs_inc_nocow_writers(struct btrfs_fs_info *fs_info,
						  u64 bytenr)
{
	struct btrfs_block_group *bg;
	bool can_nocow = true;

	bg = btrfs_lookup_block_group(fs_info, bytenr);
	if (!bg)
		return NULL;

	spin_lock(&bg->lock);
	if (bg->ro)
		can_nocow = false;
	else
		atomic_inc(&bg->nocow_writers);
	spin_unlock(&bg->lock);

	if (!can_nocow) {
		btrfs_put_block_group(bg);
		return NULL;
	}

	/* No put on block group, done by btrfs_dec_nocow_writers(). */
	return bg;
}

/*
 * Decrement the number of NOCOW writers in a block group.
 *
 * This is meant to be called after a previous call to btrfs_inc_nocow_writers(),
 * and on the block group returned by that call. Typically this is called after
 * creating an ordered extent for a NOCOW write, to prevent races with scrub and
 * relocation.
 *
 * After this call, the caller should not use the block group anymore. It it wants
 * to use it, then it should get a reference on it before calling this function.
 */
void btrfs_dec_nocow_writers(struct btrfs_block_group *bg)
{
	if (atomic_dec_and_test(&bg->nocow_writers))
		wake_up_var(&bg->nocow_writers);

	/* For the lookup done by a previous call to btrfs_inc_nocow_writers(). */
	btrfs_put_block_group(bg);
}

void btrfs_wait_nocow_writers(struct btrfs_block_group *bg)
{
	wait_var_event(&bg->nocow_writers, !atomic_read(&bg->nocow_writers));
}

void btrfs_dec_block_group_reservations(struct btrfs_fs_info *fs_info,
					const u64 start)
{
	struct btrfs_block_group *bg;

	bg = btrfs_lookup_block_group(fs_info, start);
	ASSERT(bg);
	if (atomic_dec_and_test(&bg->reservations))
		wake_up_var(&bg->reservations);
	btrfs_put_block_group(bg);
}

void btrfs_wait_block_group_reservations(struct btrfs_block_group *bg)
{
	struct btrfs_space_info *space_info = bg->space_info;

	ASSERT(bg->ro);

	if (!(bg->flags & BTRFS_BLOCK_GROUP_DATA))
		return;

	/*
	 * Our block group is read only but before we set it to read only,
	 * some task might have had allocated an extent from it already, but it
	 * has not yet created a respective ordered extent (and added it to a
	 * root's list of ordered extents).
	 * Therefore wait for any task currently allocating extents, since the
	 * block group's reservations counter is incremented while a read lock
	 * on the groups' semaphore is held and decremented after releasing
	 * the read access on that semaphore and creating the ordered extent.
	 */
	down_write(&space_info->groups_sem);
	up_write(&space_info->groups_sem);

	wait_var_event(&bg->reservations, !atomic_read(&bg->reservations));
}

struct btrfs_caching_control *btrfs_get_caching_control(
		struct btrfs_block_group *cache)
{
	struct btrfs_caching_control *ctl;

	spin_lock(&cache->lock);
	if (!cache->caching_ctl) {
		spin_unlock(&cache->lock);
		return NULL;
	}

	ctl = cache->caching_ctl;
	refcount_inc(&ctl->count);
	spin_unlock(&cache->lock);
	return ctl;
}

void btrfs_put_caching_control(struct btrfs_caching_control *ctl)
{
	if (refcount_dec_and_test(&ctl->count))
		kfree(ctl);
}

/*
 * When we wait for progress in the block group caching, its because our
 * allocation attempt failed at least once.  So, we must sleep and let some
 * progress happen before we try again.
 *
 * This function will sleep at least once waiting for new free space to show
 * up, and then it will check the block group free space numbers for our min
 * num_bytes.  Another option is to have it go ahead and look in the rbtree for
 * a free extent of a given size, but this is a good start.
 *
 * Callers of this must check if cache->cached == BTRFS_CACHE_ERROR before using
 * any of the information in this block group.
 */
void btrfs_wait_block_group_cache_progress(struct btrfs_block_group *cache,
					   u64 num_bytes)
{
	struct btrfs_caching_control *caching_ctl;
	int progress;

	caching_ctl = btrfs_get_caching_control(cache);
	if (!caching_ctl)
		return;

	/*
	 * We've already failed to allocate from this block group, so even if
	 * there's enough space in the block group it isn't contiguous enough to
	 * allow for an allocation, so wait for at least the next wakeup tick,
	 * or for the thing to be done.
	 */
	progress = atomic_read(&caching_ctl->progress);

	wait_event(caching_ctl->wait, btrfs_block_group_done(cache) ||
		   (progress != atomic_read(&caching_ctl->progress) &&
		    (cache->free_space_ctl->free_space >= num_bytes)));

	btrfs_put_caching_control(caching_ctl);
}

static int btrfs_caching_ctl_wait_done(struct btrfs_block_group *cache,
				       struct btrfs_caching_control *caching_ctl)
{
	wait_event(caching_ctl->wait, btrfs_block_group_done(cache));
	return cache->cached == BTRFS_CACHE_ERROR ? -EIO : 0;
}

static int btrfs_wait_block_group_cache_done(struct btrfs_block_group *cache)
{
	struct btrfs_caching_control *caching_ctl;
	int ret;

	caching_ctl = btrfs_get_caching_control(cache);
	if (!caching_ctl)
		return (cache->cached == BTRFS_CACHE_ERROR) ? -EIO : 0;
	ret = btrfs_caching_ctl_wait_done(cache, caching_ctl);
	btrfs_put_caching_control(caching_ctl);
	return ret;
}

#ifdef CONFIG_BTRFS_DEBUG
static void fragment_free_space(struct btrfs_block_group *block_group)
{
	struct btrfs_fs_info *fs_info = block_group->fs_info;
	u64 start = block_group->start;
	u64 len = block_group->length;
	u64 chunk = block_group->flags & BTRFS_BLOCK_GROUP_METADATA ?
		fs_info->nodesize : fs_info->sectorsize;
	u64 step = chunk << 1;

	while (len > chunk) {
		btrfs_remove_free_space(block_group, start, chunk);
		start += step;
		if (len < step)
			len = 0;
		else
			len -= step;
	}
}
#endif

/*
 * Add a free space range to the in memory free space cache of a block group.
 * This checks if the range contains super block locations and any such
 * locations are not added to the free space cache.
 *
 * @block_group:      The target block group.
 * @start:            Start offset of the range.
 * @end:              End offset of the range (exclusive).
 * @total_added_ret:  Optional pointer to return the total amount of space
 *                    added to the block group's free space cache.
 *
 * Returns 0 on success or < 0 on error.
 */
int btrfs_add_new_free_space(struct btrfs_block_group *block_group, u64 start,
			     u64 end, u64 *total_added_ret)
{
	struct btrfs_fs_info *info = block_group->fs_info;
	u64 extent_start, extent_end, size;
	int ret;

	if (total_added_ret)
		*total_added_ret = 0;

	while (start < end) {
		if (!find_first_extent_bit(&info->excluded_extents, start,
					   &extent_start, &extent_end,
					   EXTENT_DIRTY | EXTENT_UPTODATE,
					   NULL))
			break;

		if (extent_start <= start) {
			start = extent_end + 1;
		} else if (extent_start > start && extent_start < end) {
			size = extent_start - start;
			ret = btrfs_add_free_space_async_trimmed(block_group,
								 start, size);
			if (ret)
				return ret;
			if (total_added_ret)
				*total_added_ret += size;
			start = extent_end + 1;
		} else {
			break;
		}
	}

	if (start < end) {
		size = end - start;
		ret = btrfs_add_free_space_async_trimmed(block_group, start,
							 size);
		if (ret)
			return ret;
		if (total_added_ret)
			*total_added_ret += size;
	}

	return 0;
}

/*
 * Get an arbitrary extent item index / max_index through the block group
 *
 * @block_group   the block group to sample from
 * @index:        the integral step through the block group to grab from
 * @max_index:    the granularity of the sampling
 * @key:          return value parameter for the item we find
 *
 * Pre-conditions on indices:
 * 0 <= index <= max_index
 * 0 < max_index
 *
 * Returns: 0 on success, 1 if the search didn't yield a useful item, negative
 * error code on error.
 */
static int sample_block_group_extent_item(struct btrfs_caching_control *caching_ctl,
					  struct btrfs_block_group *block_group,
					  int index, int max_index,
					  struct btrfs_key *found_key)
{
	struct btrfs_fs_info *fs_info = block_group->fs_info;
	struct btrfs_root *extent_root;
	u64 search_offset;
	u64 search_end = block_group->start + block_group->length;
	struct btrfs_path *path;
	struct btrfs_key search_key;
	int ret = 0;

	ASSERT(index >= 0);
	ASSERT(index <= max_index);
	ASSERT(max_index > 0);
	lockdep_assert_held(&caching_ctl->mutex);
	lockdep_assert_held_read(&fs_info->commit_root_sem);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	extent_root = btrfs_extent_root(fs_info, max_t(u64, block_group->start,
						       BTRFS_SUPER_INFO_OFFSET));

	path->skip_locking = 1;
	path->search_commit_root = 1;
	path->reada = READA_FORWARD;

	search_offset = index * div_u64(block_group->length, max_index);
	search_key.objectid = block_group->start + search_offset;
	search_key.type = BTRFS_EXTENT_ITEM_KEY;
	search_key.offset = 0;

	btrfs_for_each_slot(extent_root, &search_key, found_key, path, ret) {
		/* Success; sampled an extent item in the block group */
		if (found_key->type == BTRFS_EXTENT_ITEM_KEY &&
		    found_key->objectid >= block_group->start &&
		    found_key->objectid + found_key->offset <= search_end)
			break;

		/* We can't possibly find a valid extent item anymore */
		if (found_key->objectid >= search_end) {
			ret = 1;
			break;
		}
	}

	lockdep_assert_held(&caching_ctl->mutex);
	lockdep_assert_held_read(&fs_info->commit_root_sem);
	btrfs_free_path(path);
	return ret;
}

/*
 * Best effort attempt to compute a block group's size class while caching it.
 *
 * @block_group: the block group we are caching
 *
 * We cannot infer the size class while adding free space extents, because that
 * logic doesn't care about contiguous file extents (it doesn't differentiate
 * between a 100M extent and 100 contiguous 1M extents). So we need to read the
 * file extent items. Reading all of them is quite wasteful, because usually
 * only a handful are enough to give a good answer. Therefore, we just grab 5 of
 * them at even steps through the block group and pick the smallest size class
 * we see. Since size class is best effort, and not guaranteed in general,
 * inaccuracy is acceptable.
 *
 * To be more explicit about why this algorithm makes sense:
 *
 * If we are caching in a block group from disk, then there are three major cases
 * to consider:
 * 1. the block group is well behaved and all extents in it are the same size
 *    class.
 * 2. the block group is mostly one size class with rare exceptions for last
 *    ditch allocations
 * 3. the block group was populated before size classes and can have a totally
 *    arbitrary mix of size classes.
 *
 * In case 1, looking at any extent in the block group will yield the correct
 * result. For the mixed cases, taking the minimum size class seems like a good
 * approximation, since gaps from frees will be usable to the size class. For
 * 2., a small handful of file extents is likely to yield the right answer. For
 * 3, we can either read every file extent, or admit that this is best effort
 * anyway and try to stay fast.
 *
 * Returns: 0 on success, negative error code on error.
 */
static int load_block_group_size_class(struct btrfs_caching_control *caching_ctl,
				       struct btrfs_block_group *block_group)
{
	struct btrfs_fs_info *fs_info = block_group->fs_info;
	struct btrfs_key key;
	int i;
	u64 min_size = block_group->length;
	enum btrfs_block_group_size_class size_class = BTRFS_BG_SZ_NONE;
	int ret;

	if (!btrfs_block_group_should_use_size_class(block_group))
		return 0;

	lockdep_assert_held(&caching_ctl->mutex);
	lockdep_assert_held_read(&fs_info->commit_root_sem);
	for (i = 0; i < 5; ++i) {
		ret = sample_block_group_extent_item(caching_ctl, block_group, i, 5, &key);
		if (ret < 0)
			goto out;
		if (ret > 0)
			continue;
		min_size = min_t(u64, min_size, key.offset);
		size_class = btrfs_calc_block_group_size_class(min_size);
	}
	if (size_class != BTRFS_BG_SZ_NONE) {
		spin_lock(&block_group->lock);
		block_group->size_class = size_class;
		spin_unlock(&block_group->lock);
	}
out:
	return ret;
}

static int load_extent_tree_free(struct btrfs_caching_control *caching_ctl)
{
	struct btrfs_block_group *block_group = caching_ctl->block_group;
	struct btrfs_fs_info *fs_info = block_group->fs_info;
	struct btrfs_root *extent_root;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 total_found = 0;
	u64 last = 0;
	u32 nritems;
	int ret;
	bool wakeup = true;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	last = max_t(u64, block_group->start, BTRFS_SUPER_INFO_OFFSET);
	extent_root = btrfs_extent_root(fs_info, last);

#ifdef CONFIG_BTRFS_DEBUG
	/*
	 * If we're fragmenting we don't want to make anybody think we can
	 * allocate from this block group until we've had a chance to fragment
	 * the free space.
	 */
	if (btrfs_should_fragment_free_space(block_group))
		wakeup = false;
#endif
	/*
	 * We don't want to deadlock with somebody trying to allocate a new
	 * extent for the extent root while also trying to search the extent
	 * root to add free space.  So we skip locking and search the commit
	 * root, since its read-only
	 */
	path->skip_locking = 1;
	path->search_commit_root = 1;
	path->reada = READA_FORWARD;

	key.objectid = last;
	key.offset = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;

next:
	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;

	leaf = path->nodes[0];
	nritems = btrfs_header_nritems(leaf);

	while (1) {
		if (btrfs_fs_closing(fs_info) > 1) {
			last = (u64)-1;
			break;
		}

		if (path->slots[0] < nritems) {
			btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		} else {
			ret = btrfs_find_next_key(extent_root, path, &key, 0, 0);
			if (ret)
				break;

			if (need_resched() ||
			    rwsem_is_contended(&fs_info->commit_root_sem)) {
				btrfs_release_path(path);
				up_read(&fs_info->commit_root_sem);
				mutex_unlock(&caching_ctl->mutex);
				cond_resched();
				mutex_lock(&caching_ctl->mutex);
				down_read(&fs_info->commit_root_sem);
				goto next;
			}

			ret = btrfs_next_leaf(extent_root, path);
			if (ret < 0)
				goto out;
			if (ret)
				break;
			leaf = path->nodes[0];
			nritems = btrfs_header_nritems(leaf);
			continue;
		}

		if (key.objectid < last) {
			key.objectid = last;
			key.offset = 0;
			key.type = BTRFS_EXTENT_ITEM_KEY;
			btrfs_release_path(path);
			goto next;
		}

		if (key.objectid < block_group->start) {
			path->slots[0]++;
			continue;
		}

		if (key.objectid >= block_group->start + block_group->length)
			break;

		if (key.type == BTRFS_EXTENT_ITEM_KEY ||
		    key.type == BTRFS_METADATA_ITEM_KEY) {
			u64 space_added;

			ret = btrfs_add_new_free_space(block_group, last,
						       key.objectid, &space_added);
			if (ret)
				goto out;
			total_found += space_added;
			if (key.type == BTRFS_METADATA_ITEM_KEY)
				last = key.objectid +
					fs_info->nodesize;
			else
				last = key.objectid + key.offset;

			if (total_found > CACHING_CTL_WAKE_UP) {
				total_found = 0;
				if (wakeup) {
					atomic_inc(&caching_ctl->progress);
					wake_up(&caching_ctl->wait);
				}
			}
		}
		path->slots[0]++;
	}

	ret = btrfs_add_new_free_space(block_group, last,
				       block_group->start + block_group->length,
				       NULL);
out:
	btrfs_free_path(path);
	return ret;
}

static inline void btrfs_free_excluded_extents(const struct btrfs_block_group *bg)
{
	clear_extent_bits(&bg->fs_info->excluded_extents, bg->start,
			  bg->start + bg->length - 1, EXTENT_UPTODATE);
}

static noinline void caching_thread(struct btrfs_work *work)
{
	struct btrfs_block_group *block_group;
	struct btrfs_fs_info *fs_info;
	struct btrfs_caching_control *caching_ctl;
	int ret;

	caching_ctl = container_of(work, struct btrfs_caching_control, work);
	block_group = caching_ctl->block_group;
	fs_info = block_group->fs_info;

	mutex_lock(&caching_ctl->mutex);
	down_read(&fs_info->commit_root_sem);

	load_block_group_size_class(caching_ctl, block_group);
	if (btrfs_test_opt(fs_info, SPACE_CACHE)) {
		ret = load_free_space_cache(block_group);
		if (ret == 1) {
			ret = 0;
			goto done;
		}

		/*
		 * We failed to load the space cache, set ourselves to
		 * CACHE_STARTED and carry on.
		 */
		spin_lock(&block_group->lock);
		block_group->cached = BTRFS_CACHE_STARTED;
		spin_unlock(&block_group->lock);
		wake_up(&caching_ctl->wait);
	}

	/*
	 * If we are in the transaction that populated the free space tree we
	 * can't actually cache from the free space tree as our commit root and
	 * real root are the same, so we could change the contents of the blocks
	 * while caching.  Instead do the slow caching in this case, and after
	 * the transaction has committed we will be safe.
	 */
	if (btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE) &&
	    !(test_bit(BTRFS_FS_FREE_SPACE_TREE_UNTRUSTED, &fs_info->flags)))
		ret = load_free_space_tree(caching_ctl);
	else
		ret = load_extent_tree_free(caching_ctl);
done:
	spin_lock(&block_group->lock);
	block_group->caching_ctl = NULL;
	block_group->cached = ret ? BTRFS_CACHE_ERROR : BTRFS_CACHE_FINISHED;
	spin_unlock(&block_group->lock);

#ifdef CONFIG_BTRFS_DEBUG
	if (btrfs_should_fragment_free_space(block_group)) {
		u64 bytes_used;

		spin_lock(&block_group->space_info->lock);
		spin_lock(&block_group->lock);
		bytes_used = block_group->length - block_group->used;
		block_group->space_info->bytes_used += bytes_used >> 1;
		spin_unlock(&block_group->lock);
		spin_unlock(&block_group->space_info->lock);
		fragment_free_space(block_group);
	}
#endif

	up_read(&fs_info->commit_root_sem);
	btrfs_free_excluded_extents(block_group);
	mutex_unlock(&caching_ctl->mutex);

	wake_up(&caching_ctl->wait);

	btrfs_put_caching_control(caching_ctl);
	btrfs_put_block_group(block_group);
}

int btrfs_cache_block_group(struct btrfs_block_group *cache, bool wait)
{
	struct btrfs_fs_info *fs_info = cache->fs_info;
	struct btrfs_caching_control *caching_ctl = NULL;
	int ret = 0;

	/* Allocator for zoned filesystems does not use the cache at all */
	if (btrfs_is_zoned(fs_info))
		return 0;

	caching_ctl = kzalloc(sizeof(*caching_ctl), GFP_NOFS);
	if (!caching_ctl)
		return -ENOMEM;

	INIT_LIST_HEAD(&caching_ctl->list);
	mutex_init(&caching_ctl->mutex);
	init_waitqueue_head(&caching_ctl->wait);
	caching_ctl->block_group = cache;
	refcount_set(&caching_ctl->count, 2);
	atomic_set(&caching_ctl->progress, 0);
	btrfs_init_work(&caching_ctl->work, caching_thread, NULL);

	spin_lock(&cache->lock);
	if (cache->cached != BTRFS_CACHE_NO) {
		kfree(caching_ctl);

		caching_ctl = cache->caching_ctl;
		if (caching_ctl)
			refcount_inc(&caching_ctl->count);
		spin_unlock(&cache->lock);
		goto out;
	}
	WARN_ON(cache->caching_ctl);
	cache->caching_ctl = caching_ctl;
	cache->cached = BTRFS_CACHE_STARTED;
	spin_unlock(&cache->lock);

	write_lock(&fs_info->block_group_cache_lock);
	refcount_inc(&caching_ctl->count);
	list_add_tail(&caching_ctl->list, &fs_info->caching_block_groups);
	write_unlock(&fs_info->block_group_cache_lock);

	btrfs_get_block_group(cache);

	btrfs_queue_work(fs_info->caching_workers, &caching_ctl->work);
out:
	if (wait && caching_ctl)
		ret = btrfs_caching_ctl_wait_done(cache, caching_ctl);
	if (caching_ctl)
		btrfs_put_caching_control(caching_ctl);

	return ret;
}

static void clear_avail_alloc_bits(struct btrfs_fs_info *fs_info, u64 flags)
{
	u64 extra_flags = chunk_to_extended(flags) &
				BTRFS_EXTENDED_PROFILE_MASK;

	write_seqlock(&fs_info->profiles_lock);
	if (flags & BTRFS_BLOCK_GROUP_DATA)
		fs_info->avail_data_alloc_bits &= ~extra_flags;
	if (flags & BTRFS_BLOCK_GROUP_METADATA)
		fs_info->avail_metadata_alloc_bits &= ~extra_flags;
	if (flags & BTRFS_BLOCK_GROUP_SYSTEM)
		fs_info->avail_system_alloc_bits &= ~extra_flags;
	write_sequnlock(&fs_info->profiles_lock);
}

/*
 * Clear incompat bits for the following feature(s):
 *
 * - RAID56 - in case there's neither RAID5 nor RAID6 profile block group
 *            in the whole filesystem
 *
 * - RAID1C34 - same as above for RAID1C3 and RAID1C4 block groups
 */
static void clear_incompat_bg_bits(struct btrfs_fs_info *fs_info, u64 flags)
{
	bool found_raid56 = false;
	bool found_raid1c34 = false;

	if ((flags & BTRFS_BLOCK_GROUP_RAID56_MASK) ||
	    (flags & BTRFS_BLOCK_GROUP_RAID1C3) ||
	    (flags & BTRFS_BLOCK_GROUP_RAID1C4)) {
		struct list_head *head = &fs_info->space_info;
		struct btrfs_space_info *sinfo;

		list_for_each_entry_rcu(sinfo, head, list) {
			down_read(&sinfo->groups_sem);
			if (!list_empty(&sinfo->block_groups[BTRFS_RAID_RAID5]))
				found_raid56 = true;
			if (!list_empty(&sinfo->block_groups[BTRFS_RAID_RAID6]))
				found_raid56 = true;
			if (!list_empty(&sinfo->block_groups[BTRFS_RAID_RAID1C3]))
				found_raid1c34 = true;
			if (!list_empty(&sinfo->block_groups[BTRFS_RAID_RAID1C4]))
				found_raid1c34 = true;
			up_read(&sinfo->groups_sem);
		}
		if (!found_raid56)
			btrfs_clear_fs_incompat(fs_info, RAID56);
		if (!found_raid1c34)
			btrfs_clear_fs_incompat(fs_info, RAID1C34);
	}
}

static int remove_block_group_item(struct btrfs_trans_handle *trans,
				   struct btrfs_path *path,
				   struct btrfs_block_group *block_group)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root;
	struct btrfs_key key;
	int ret;

	root = btrfs_block_group_root(fs_info);
	key.objectid = block_group->start;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = block_group->length;

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		return ret;

	ret = btrfs_del_item(trans, root, path);
	return ret;
}

int btrfs_remove_block_group(struct btrfs_trans_handle *trans,
			     struct btrfs_chunk_map *map)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_path *path;
	struct btrfs_block_group *block_group;
	struct btrfs_free_cluster *cluster;
	struct inode *inode;
	struct kobject *kobj = NULL;
	int ret;
	int index;
	int factor;
	struct btrfs_caching_control *caching_ctl = NULL;
	bool remove_map;
	bool remove_rsv = false;

	block_group = btrfs_lookup_block_group(fs_info, map->start);
	BUG_ON(!block_group);
	BUG_ON(!block_group->ro);

	trace_btrfs_remove_block_group(block_group);
	/*
	 * Free the reserved super bytes from this block group before
	 * remove it.
	 */
	btrfs_free_excluded_extents(block_group);
	btrfs_free_ref_tree_range(fs_info, block_group->start,
				  block_group->length);

	index = btrfs_bg_flags_to_raid_index(block_group->flags);
	factor = btrfs_bg_type_to_factor(block_group->flags);

	/* make sure this block group isn't part of an allocation cluster */
	cluster = &fs_info->data_alloc_cluster;
	spin_lock(&cluster->refill_lock);
	btrfs_return_cluster_to_free_space(block_group, cluster);
	spin_unlock(&cluster->refill_lock);

	/*
	 * make sure this block group isn't part of a metadata
	 * allocation cluster
	 */
	cluster = &fs_info->meta_alloc_cluster;
	spin_lock(&cluster->refill_lock);
	btrfs_return_cluster_to_free_space(block_group, cluster);
	spin_unlock(&cluster->refill_lock);

	btrfs_clear_treelog_bg(block_group);
	btrfs_clear_data_reloc_bg(block_group);

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * get the inode first so any iput calls done for the io_list
	 * aren't the final iput (no unlinks allowed now)
	 */
	inode = lookup_free_space_inode(block_group, path);

	mutex_lock(&trans->transaction->cache_write_mutex);
	/*
	 * Make sure our free space cache IO is done before removing the
	 * free space inode
	 */
	spin_lock(&trans->transaction->dirty_bgs_lock);
	if (!list_empty(&block_group->io_list)) {
		list_del_init(&block_group->io_list);

		WARN_ON(!IS_ERR(inode) && inode != block_group->io_ctl.inode);

		spin_unlock(&trans->transaction->dirty_bgs_lock);
		btrfs_wait_cache_io(trans, block_group, path);
		btrfs_put_block_group(block_group);
		spin_lock(&trans->transaction->dirty_bgs_lock);
	}

	if (!list_empty(&block_group->dirty_list)) {
		list_del_init(&block_group->dirty_list);
		remove_rsv = true;
		btrfs_put_block_group(block_group);
	}
	spin_unlock(&trans->transaction->dirty_bgs_lock);
	mutex_unlock(&trans->transaction->cache_write_mutex);

	ret = btrfs_remove_free_space_inode(trans, inode, block_group);
	if (ret)
		goto out;

	write_lock(&fs_info->block_group_cache_lock);
	rb_erase_cached(&block_group->cache_node,
			&fs_info->block_group_cache_tree);
	RB_CLEAR_NODE(&block_group->cache_node);

	/* Once for the block groups rbtree */
	btrfs_put_block_group(block_group);

	write_unlock(&fs_info->block_group_cache_lock);

	down_write(&block_group->space_info->groups_sem);
	/*
	 * we must use list_del_init so people can check to see if they
	 * are still on the list after taking the semaphore
	 */
	list_del_init(&block_group->list);
	if (list_empty(&block_group->space_info->block_groups[index])) {
		kobj = block_group->space_info->block_group_kobjs[index];
		block_group->space_info->block_group_kobjs[index] = NULL;
		clear_avail_alloc_bits(fs_info, block_group->flags);
	}
	up_write(&block_group->space_info->groups_sem);
	clear_incompat_bg_bits(fs_info, block_group->flags);
	if (kobj) {
		kobject_del(kobj);
		kobject_put(kobj);
	}

	if (block_group->cached == BTRFS_CACHE_STARTED)
		btrfs_wait_block_group_cache_done(block_group);

	write_lock(&fs_info->block_group_cache_lock);
	caching_ctl = btrfs_get_caching_control(block_group);
	if (!caching_ctl) {
		struct btrfs_caching_control *ctl;

		list_for_each_entry(ctl, &fs_info->caching_block_groups, list) {
			if (ctl->block_group == block_group) {
				caching_ctl = ctl;
				refcount_inc(&caching_ctl->count);
				break;
			}
		}
	}
	if (caching_ctl)
		list_del_init(&caching_ctl->list);
	write_unlock(&fs_info->block_group_cache_lock);

	if (caching_ctl) {
		/* Once for the caching bgs list and once for us. */
		btrfs_put_caching_control(caching_ctl);
		btrfs_put_caching_control(caching_ctl);
	}

	spin_lock(&trans->transaction->dirty_bgs_lock);
	WARN_ON(!list_empty(&block_group->dirty_list));
	WARN_ON(!list_empty(&block_group->io_list));
	spin_unlock(&trans->transaction->dirty_bgs_lock);

	btrfs_remove_free_space_cache(block_group);

	spin_lock(&block_group->space_info->lock);
	list_del_init(&block_group->ro_list);

	if (btrfs_test_opt(fs_info, ENOSPC_DEBUG)) {
		WARN_ON(block_group->space_info->total_bytes
			< block_group->length);
		WARN_ON(block_group->space_info->bytes_readonly
			< block_group->length - block_group->zone_unusable);
		WARN_ON(block_group->space_info->bytes_zone_unusable
			< block_group->zone_unusable);
		WARN_ON(block_group->space_info->disk_total
			< block_group->length * factor);
	}
	block_group->space_info->total_bytes -= block_group->length;
	block_group->space_info->bytes_readonly -=
		(block_group->length - block_group->zone_unusable);
	block_group->space_info->bytes_zone_unusable -=
		block_group->zone_unusable;
	block_group->space_info->disk_total -= block_group->length * factor;

	spin_unlock(&block_group->space_info->lock);

	/*
	 * Remove the free space for the block group from the free space tree
	 * and the block group's item from the extent tree before marking the
	 * block group as removed. This is to prevent races with tasks that
	 * freeze and unfreeze a block group, this task and another task
	 * allocating a new block group - the unfreeze task ends up removing
	 * the block group's extent map before the task calling this function
	 * deletes the block group item from the extent tree, allowing for
	 * another task to attempt to create another block group with the same
	 * item key (and failing with -EEXIST and a transaction abort).
	 */
	ret = remove_block_group_free_space(trans, block_group);
	if (ret)
		goto out;

	ret = remove_block_group_item(trans, path, block_group);
	if (ret < 0)
		goto out;

	spin_lock(&block_group->lock);
	set_bit(BLOCK_GROUP_FLAG_REMOVED, &block_group->runtime_flags);

	/*
	 * At this point trimming or scrub can't start on this block group,
	 * because we removed the block group from the rbtree
	 * fs_info->block_group_cache_tree so no one can't find it anymore and
	 * even if someone already got this block group before we removed it
	 * from the rbtree, they have already incremented block_group->frozen -
	 * if they didn't, for the trimming case they won't find any free space
	 * entries because we already removed them all when we called
	 * btrfs_remove_free_space_cache().
	 *
	 * And we must not remove the chunk map from the fs_info->mapping_tree
	 * to prevent the same logical address range and physical device space
	 * ranges from being reused for a new block group. This is needed to
	 * avoid races with trimming and scrub.
	 *
	 * An fs trim operation (btrfs_trim_fs() / btrfs_ioctl_fitrim()) is
	 * completely transactionless, so while it is trimming a range the
	 * currently running transaction might finish and a new one start,
	 * allowing for new block groups to be created that can reuse the same
	 * physical device locations unless we take this special care.
	 *
	 * There may also be an implicit trim operation if the file system
	 * is mounted with -odiscard. The same protections must remain
	 * in place until the extents have been discarded completely when
	 * the transaction commit has completed.
	 */
	remove_map = (atomic_read(&block_group->frozen) == 0);
	spin_unlock(&block_group->lock);

	if (remove_map)
		btrfs_remove_chunk_map(fs_info, map);

out:
	/* Once for the lookup reference */
	btrfs_put_block_group(block_group);
	if (remove_rsv)
		btrfs_dec_delayed_refs_rsv_bg_updates(fs_info);
	btrfs_free_path(path);
	return ret;
}

struct btrfs_trans_handle *btrfs_start_trans_remove_block_group(
		struct btrfs_fs_info *fs_info, const u64 chunk_offset)
{
	struct btrfs_root *root = btrfs_block_group_root(fs_info);
	struct btrfs_chunk_map *map;
	unsigned int num_items;

	map = btrfs_find_chunk_map(fs_info, chunk_offset, 1);
	ASSERT(map != NULL);
	ASSERT(map->start == chunk_offset);

	/*
	 * We need to reserve 3 + N units from the metadata space info in order
	 * to remove a block group (done at btrfs_remove_chunk() and at
	 * btrfs_remove_block_group()), which are used for:
	 *
	 * 1 unit for adding the free space inode's orphan (located in the tree
	 * of tree roots).
	 * 1 unit for deleting the block group item (located in the extent
	 * tree).
	 * 1 unit for deleting the free space item (located in tree of tree
	 * roots).
	 * N units for deleting N device extent items corresponding to each
	 * stripe (located in the device tree).
	 *
	 * In order to remove a block group we also need to reserve units in the
	 * system space info in order to update the chunk tree (update one or
	 * more device items and remove one chunk item), but this is done at
	 * btrfs_remove_chunk() through a call to check_system_chunk().
	 */
	num_items = 3 + map->num_stripes;
	btrfs_free_chunk_map(map);

	return btrfs_start_transaction_fallback_global_rsv(root, num_items);
}

/*
 * Mark block group @cache read-only, so later write won't happen to block
 * group @cache.
 *
 * If @force is not set, this function will only mark the block group readonly
 * if we have enough free space (1M) in other metadata/system block groups.
 * If @force is not set, this function will mark the block group readonly
 * without checking free space.
 *
 * NOTE: This function doesn't care if other block groups can contain all the
 * data in this block group. That check should be done by relocation routine,
 * not this function.
 */
static int inc_block_group_ro(struct btrfs_block_group *cache, int force)
{
	struct btrfs_space_info *sinfo = cache->space_info;
	u64 num_bytes;
	int ret = -ENOSPC;

	spin_lock(&sinfo->lock);
	spin_lock(&cache->lock);

	if (cache->swap_extents) {
		ret = -ETXTBSY;
		goto out;
	}

	if (cache->ro) {
		cache->ro++;
		ret = 0;
		goto out;
	}

	num_bytes = cache->length - cache->reserved - cache->pinned -
		    cache->bytes_super - cache->zone_unusable - cache->used;

	/*
	 * Data never overcommits, even in mixed mode, so do just the straight
	 * check of left over space in how much we have allocated.
	 */
	if (force) {
		ret = 0;
	} else if (sinfo->flags & BTRFS_BLOCK_GROUP_DATA) {
		u64 sinfo_used = btrfs_space_info_used(sinfo, true);

		/*
		 * Here we make sure if we mark this bg RO, we still have enough
		 * free space as buffer.
		 */
		if (sinfo_used + num_bytes <= sinfo->total_bytes)
			ret = 0;
	} else {
		/*
		 * We overcommit metadata, so we need to do the
		 * btrfs_can_overcommit check here, and we need to pass in
		 * BTRFS_RESERVE_NO_FLUSH to give ourselves the most amount of
		 * leeway to allow us to mark this block group as read only.
		 */
		if (btrfs_can_overcommit(cache->fs_info, sinfo, num_bytes,
					 BTRFS_RESERVE_NO_FLUSH))
			ret = 0;
	}

	if (!ret) {
		sinfo->bytes_readonly += num_bytes;
		if (btrfs_is_zoned(cache->fs_info)) {
			/* Migrate zone_unusable bytes to readonly */
			sinfo->bytes_readonly += cache->zone_unusable;
			sinfo->bytes_zone_unusable -= cache->zone_unusable;
			cache->zone_unusable = 0;
		}
		cache->ro++;
		list_add_tail(&cache->ro_list, &sinfo->ro_bgs);
	}
out:
	spin_unlock(&cache->lock);
	spin_unlock(&sinfo->lock);
	if (ret == -ENOSPC && btrfs_test_opt(cache->fs_info, ENOSPC_DEBUG)) {
		btrfs_info(cache->fs_info,
			"unable to make block group %llu ro", cache->start);
		btrfs_dump_space_info(cache->fs_info, cache->space_info, 0, 0);
	}
	return ret;
}

static bool clean_pinned_extents(struct btrfs_trans_handle *trans,
				 struct btrfs_block_group *bg)
{
	struct btrfs_fs_info *fs_info = bg->fs_info;
	struct btrfs_transaction *prev_trans = NULL;
	const u64 start = bg->start;
	const u64 end = start + bg->length - 1;
	int ret;

	spin_lock(&fs_info->trans_lock);
	if (trans->transaction->list.prev != &fs_info->trans_list) {
		prev_trans = list_last_entry(&trans->transaction->list,
					     struct btrfs_transaction, list);
		refcount_inc(&prev_trans->use_count);
	}
	spin_unlock(&fs_info->trans_lock);

	/*
	 * Hold the unused_bg_unpin_mutex lock to avoid racing with
	 * btrfs_finish_extent_commit(). If we are at transaction N, another
	 * task might be running finish_extent_commit() for the previous
	 * transaction N - 1, and have seen a range belonging to the block
	 * group in pinned_extents before we were able to clear the whole block
	 * group range from pinned_extents. This means that task can lookup for
	 * the block group after we unpinned it from pinned_extents and removed
	 * it, leading to a BUG_ON() at unpin_extent_range().
	 */
	mutex_lock(&fs_info->unused_bg_unpin_mutex);
	if (prev_trans) {
		ret = clear_extent_bits(&prev_trans->pinned_extents, start, end,
					EXTENT_DIRTY);
		if (ret)
			goto out;
	}

	ret = clear_extent_bits(&trans->transaction->pinned_extents, start, end,
				EXTENT_DIRTY);
out:
	mutex_unlock(&fs_info->unused_bg_unpin_mutex);
	if (prev_trans)
		btrfs_put_transaction(prev_trans);

	return ret == 0;
}

/*
 * Process the unused_bgs list and remove any that don't have any allocated
 * space inside of them.
 */
void btrfs_delete_unused_bgs(struct btrfs_fs_info *fs_info)
{
	LIST_HEAD(retry_list);
	struct btrfs_block_group *block_group;
	struct btrfs_space_info *space_info;
	struct btrfs_trans_handle *trans;
	const bool async_trim_enabled = btrfs_test_opt(fs_info, DISCARD_ASYNC);
	int ret = 0;

	if (!test_bit(BTRFS_FS_OPEN, &fs_info->flags))
		return;

	if (btrfs_fs_closing(fs_info))
		return;

	/*
	 * Long running balances can keep us blocked here for eternity, so
	 * simply skip deletion if we're unable to get the mutex.
	 */
	if (!mutex_trylock(&fs_info->reclaim_bgs_lock))
		return;

	spin_lock(&fs_info->unused_bgs_lock);
	while (!list_empty(&fs_info->unused_bgs)) {
		u64 used;
		int trimming;

		block_group = list_first_entry(&fs_info->unused_bgs,
					       struct btrfs_block_group,
					       bg_list);
		list_del_init(&block_group->bg_list);

		space_info = block_group->space_info;

		if (ret || btrfs_mixed_space_info(space_info)) {
			btrfs_put_block_group(block_group);
			continue;
		}
		spin_unlock(&fs_info->unused_bgs_lock);

		btrfs_discard_cancel_work(&fs_info->discard_ctl, block_group);

		/* Don't want to race with allocators so take the groups_sem */
		down_write(&space_info->groups_sem);

		/*
		 * Async discard moves the final block group discard to be prior
		 * to the unused_bgs code path.  Therefore, if it's not fully
		 * trimmed, punt it back to the async discard lists.
		 */
		if (btrfs_test_opt(fs_info, DISCARD_ASYNC) &&
		    !btrfs_is_free_space_trimmed(block_group)) {
			trace_btrfs_skip_unused_block_group(block_group);
			up_write(&space_info->groups_sem);
			/* Requeue if we failed because of async discard */
			btrfs_discard_queue_work(&fs_info->discard_ctl,
						 block_group);
			goto next;
		}

		spin_lock(&space_info->lock);
		spin_lock(&block_group->lock);
		if (btrfs_is_block_group_used(block_group) || block_group->ro ||
		    list_is_singular(&block_group->list)) {
			/*
			 * We want to bail if we made new allocations or have
			 * outstanding allocations in this block group.  We do
			 * the ro check in case balance is currently acting on
			 * this block group.
			 */
			trace_btrfs_skip_unused_block_group(block_group);
			spin_unlock(&block_group->lock);
			spin_unlock(&space_info->lock);
			up_write(&space_info->groups_sem);
			goto next;
		}

		/*
		 * The block group may be unused but there may be space reserved
		 * accounting with the existence of that block group, that is,
		 * space_info->bytes_may_use was incremented by a task but no
		 * space was yet allocated from the block group by the task.
		 * That space may or may not be allocated, as we are generally
		 * pessimistic about space reservation for metadata as well as
		 * for data when using compression (as we reserve space based on
		 * the worst case, when data can't be compressed, and before
		 * actually attempting compression, before starting writeback).
		 *
		 * So check if the total space of the space_info minus the size
		 * of this block group is less than the used space of the
		 * space_info - if that's the case, then it means we have tasks
		 * that might be relying on the block group in order to allocate
		 * extents, and add back the block group to the unused list when
		 * we finish, so that we retry later in case no tasks ended up
		 * needing to allocate extents from the block group.
		 */
		used = btrfs_space_info_used(space_info, true);
		if (space_info->total_bytes - block_group->length < used &&
		    block_group->zone_unusable < block_group->length) {
			/*
			 * Add a reference for the list, compensate for the ref
			 * drop under the "next" label for the
			 * fs_info->unused_bgs list.
			 */
			btrfs_get_block_group(block_group);
			list_add_tail(&block_group->bg_list, &retry_list);

			trace_btrfs_skip_unused_block_group(block_group);
			spin_unlock(&block_group->lock);
			spin_unlock(&space_info->lock);
			up_write(&space_info->groups_sem);
			goto next;
		}

		spin_unlock(&block_group->lock);
		spin_unlock(&space_info->lock);

		/* We don't want to force the issue, only flip if it's ok. */
		ret = inc_block_group_ro(block_group, 0);
		up_write(&space_info->groups_sem);
		if (ret < 0) {
			ret = 0;
			goto next;
		}

		ret = btrfs_zone_finish(block_group);
		if (ret < 0) {
			btrfs_dec_block_group_ro(block_group);
			if (ret == -EAGAIN)
				ret = 0;
			goto next;
		}

		/*
		 * Want to do this before we do anything else so we can recover
		 * properly if we fail to join the transaction.
		 */
		trans = btrfs_start_trans_remove_block_group(fs_info,
						     block_group->start);
		if (IS_ERR(trans)) {
			btrfs_dec_block_group_ro(block_group);
			ret = PTR_ERR(trans);
			goto next;
		}

		/*
		 * We could have pending pinned extents for this block group,
		 * just delete them, we don't care about them anymore.
		 */
		if (!clean_pinned_extents(trans, block_group)) {
			btrfs_dec_block_group_ro(block_group);
			goto end_trans;
		}

		/*
		 * At this point, the block_group is read only and should fail
		 * new allocations.  However, btrfs_finish_extent_commit() can
		 * cause this block_group to be placed back on the discard
		 * lists because now the block_group isn't fully discarded.
		 * Bail here and try again later after discarding everything.
		 */
		spin_lock(&fs_info->discard_ctl.lock);
		if (!list_empty(&block_group->discard_list)) {
			spin_unlock(&fs_info->discard_ctl.lock);
			btrfs_dec_block_group_ro(block_group);
			btrfs_discard_queue_work(&fs_info->discard_ctl,
						 block_group);
			goto end_trans;
		}
		spin_unlock(&fs_info->discard_ctl.lock);

		/* Reset pinned so btrfs_put_block_group doesn't complain */
		spin_lock(&space_info->lock);
		spin_lock(&block_group->lock);

		btrfs_space_info_update_bytes_pinned(fs_info, space_info,
						     -block_group->pinned);
		space_info->bytes_readonly += block_group->pinned;
		block_group->pinned = 0;

		spin_unlock(&block_group->lock);
		spin_unlock(&space_info->lock);

		/*
		 * The normal path here is an unused block group is passed here,
		 * then trimming is handled in the transaction commit path.
		 * Async discard interposes before this to do the trimming
		 * before coming down the unused block group path as trimming
		 * will no longer be done later in the transaction commit path.
		 */
		if (!async_trim_enabled && btrfs_test_opt(fs_info, DISCARD_ASYNC))
			goto flip_async;

		/*
		 * DISCARD can flip during remount. On zoned filesystems, we
		 * need to reset sequential-required zones.
		 */
		trimming = btrfs_test_opt(fs_info, DISCARD_SYNC) ||
				btrfs_is_zoned(fs_info);

		/* Implicit trim during transaction commit. */
		if (trimming)
			btrfs_freeze_block_group(block_group);

		/*
		 * Btrfs_remove_chunk will abort the transaction if things go
		 * horribly wrong.
		 */
		ret = btrfs_remove_chunk(trans, block_group->start);

		if (ret) {
			if (trimming)
				btrfs_unfreeze_block_group(block_group);
			goto end_trans;
		}

		/*
		 * If we're not mounted with -odiscard, we can just forget
		 * about this block group. Otherwise we'll need to wait
		 * until transaction commit to do the actual discard.
		 */
		if (trimming) {
			spin_lock(&fs_info->unused_bgs_lock);
			/*
			 * A concurrent scrub might have added us to the list
			 * fs_info->unused_bgs, so use a list_move operation
			 * to add the block group to the deleted_bgs list.
			 */
			list_move(&block_group->bg_list,
				  &trans->transaction->deleted_bgs);
			spin_unlock(&fs_info->unused_bgs_lock);
			btrfs_get_block_group(block_group);
		}
end_trans:
		btrfs_end_transaction(trans);
next:
		btrfs_put_block_group(block_group);
		spin_lock(&fs_info->unused_bgs_lock);
	}
	list_splice_tail(&retry_list, &fs_info->unused_bgs);
	spin_unlock(&fs_info->unused_bgs_lock);
	mutex_unlock(&fs_info->reclaim_bgs_lock);
	return;

flip_async:
	btrfs_end_transaction(trans);
	spin_lock(&fs_info->unused_bgs_lock);
	list_splice_tail(&retry_list, &fs_info->unused_bgs);
	spin_unlock(&fs_info->unused_bgs_lock);
	mutex_unlock(&fs_info->reclaim_bgs_lock);
	btrfs_put_block_group(block_group);
	btrfs_discard_punt_unused_bgs_list(fs_info);
}

void btrfs_mark_bg_unused(struct btrfs_block_group *bg)
{
	struct btrfs_fs_info *fs_info = bg->fs_info;

	spin_lock(&fs_info->unused_bgs_lock);
	if (list_empty(&bg->bg_list)) {
		btrfs_get_block_group(bg);
		trace_btrfs_add_unused_block_group(bg);
		list_add_tail(&bg->bg_list, &fs_info->unused_bgs);
	} else if (!test_bit(BLOCK_GROUP_FLAG_NEW, &bg->runtime_flags)) {
		/* Pull out the block group from the reclaim_bgs list. */
		trace_btrfs_add_unused_block_group(bg);
		list_move_tail(&bg->bg_list, &fs_info->unused_bgs);
	}
	spin_unlock(&fs_info->unused_bgs_lock);
}

/*
 * We want block groups with a low number of used bytes to be in the beginning
 * of the list, so they will get reclaimed first.
 */
static int reclaim_bgs_cmp(void *unused, const struct list_head *a,
			   const struct list_head *b)
{
	const struct btrfs_block_group *bg1, *bg2;

	bg1 = list_entry(a, struct btrfs_block_group, bg_list);
	bg2 = list_entry(b, struct btrfs_block_group, bg_list);

	return bg1->used > bg2->used;
}

static inline bool btrfs_should_reclaim(struct btrfs_fs_info *fs_info)
{
	if (btrfs_is_zoned(fs_info))
		return btrfs_zoned_should_reclaim(fs_info);
	return true;
}

static bool should_reclaim_block_group(struct btrfs_block_group *bg, u64 bytes_freed)
{
	const struct btrfs_space_info *space_info = bg->space_info;
	const int reclaim_thresh = READ_ONCE(space_info->bg_reclaim_threshold);
	const u64 new_val = bg->used;
	const u64 old_val = new_val + bytes_freed;
	u64 thresh;

	if (reclaim_thresh == 0)
		return false;

	thresh = mult_perc(bg->length, reclaim_thresh);

	/*
	 * If we were below the threshold before don't reclaim, we are likely a
	 * brand new block group and we don't want to relocate new block groups.
	 */
	if (old_val < thresh)
		return false;
	if (new_val >= thresh)
		return false;
	return true;
}

void btrfs_reclaim_bgs_work(struct work_struct *work)
{
	struct btrfs_fs_info *fs_info =
		container_of(work, struct btrfs_fs_info, reclaim_bgs_work);
	struct btrfs_block_group *bg;
	struct btrfs_space_info *space_info;
	LIST_HEAD(retry_list);

	if (!test_bit(BTRFS_FS_OPEN, &fs_info->flags))
		return;

	if (btrfs_fs_closing(fs_info))
		return;

	if (!btrfs_should_reclaim(fs_info))
		return;

	sb_start_write(fs_info->sb);

	if (!btrfs_exclop_start(fs_info, BTRFS_EXCLOP_BALANCE)) {
		sb_end_write(fs_info->sb);
		return;
	}

	/*
	 * Long running balances can keep us blocked here for eternity, so
	 * simply skip reclaim if we're unable to get the mutex.
	 */
	if (!mutex_trylock(&fs_info->reclaim_bgs_lock)) {
		btrfs_exclop_finish(fs_info);
		sb_end_write(fs_info->sb);
		return;
	}

	spin_lock(&fs_info->unused_bgs_lock);
	/*
	 * Sort happens under lock because we can't simply splice it and sort.
	 * The block groups might still be in use and reachable via bg_list,
	 * and their presence in the reclaim_bgs list must be preserved.
	 */
	list_sort(NULL, &fs_info->reclaim_bgs, reclaim_bgs_cmp);
	while (!list_empty(&fs_info->reclaim_bgs)) {
		u64 zone_unusable;
		int ret = 0;

		bg = list_first_entry(&fs_info->reclaim_bgs,
				      struct btrfs_block_group,
				      bg_list);
		list_del_init(&bg->bg_list);

		space_info = bg->space_info;
		spin_unlock(&fs_info->unused_bgs_lock);

		/* Don't race with allocators so take the groups_sem */
		down_write(&space_info->groups_sem);

		spin_lock(&bg->lock);
		if (bg->reserved || bg->pinned || bg->ro) {
			/*
			 * We want to bail if we made new allocations or have
			 * outstanding allocations in this block group.  We do
			 * the ro check in case balance is currently acting on
			 * this block group.
			 */
			spin_unlock(&bg->lock);
			up_write(&space_info->groups_sem);
			goto next;
		}
		if (bg->used == 0) {
			/*
			 * It is possible that we trigger relocation on a block
			 * group as its extents are deleted and it first goes
			 * below the threshold, then shortly after goes empty.
			 *
			 * In this case, relocating it does delete it, but has
			 * some overhead in relocation specific metadata, looking
			 * for the non-existent extents and running some extra
			 * transactions, which we can avoid by using one of the
			 * other mechanisms for dealing with empty block groups.
			 */
			if (!btrfs_test_opt(fs_info, DISCARD_ASYNC))
				btrfs_mark_bg_unused(bg);
			spin_unlock(&bg->lock);
			up_write(&space_info->groups_sem);
			goto next;

		}
		/*
		 * The block group might no longer meet the reclaim condition by
		 * the time we get around to reclaiming it, so to avoid
		 * reclaiming overly full block_groups, skip reclaiming them.
		 *
		 * Since the decision making process also depends on the amount
		 * being freed, pass in a fake giant value to skip that extra
		 * check, which is more meaningful when adding to the list in
		 * the first place.
		 */
		if (!should_reclaim_block_group(bg, bg->length)) {
			spin_unlock(&bg->lock);
			up_write(&space_info->groups_sem);
			goto next;
		}
		spin_unlock(&bg->lock);

		/*
		 * Get out fast, in case we're read-only or unmounting the
		 * filesystem. It is OK to drop block groups from the list even
		 * for the read-only case. As we did sb_start_write(),
		 * "mount -o remount,ro" won't happen and read-only filesystem
		 * means it is forced read-only due to a fatal error. So, it
		 * never gets back to read-write to let us reclaim again.
		 */
		if (btrfs_need_cleaner_sleep(fs_info)) {
			up_write(&space_info->groups_sem);
			goto next;
		}

		/*
		 * Cache the zone_unusable value before turning the block group
		 * to read only. As soon as the blog group is read only it's
		 * zone_unusable value gets moved to the block group's read-only
		 * bytes and isn't available for calculations anymore.
		 */
		zone_unusable = bg->zone_unusable;
		ret = inc_block_group_ro(bg, 0);
		up_write(&space_info->groups_sem);
		if (ret < 0)
			goto next;

		btrfs_info(fs_info,
			"reclaiming chunk %llu with %llu%% used %llu%% unusable",
				bg->start,
				div64_u64(bg->used * 100, bg->length),
				div64_u64(zone_unusable * 100, bg->length));
		trace_btrfs_reclaim_block_group(bg);
		ret = btrfs_relocate_chunk(fs_info, bg->start);
		if (ret) {
			btrfs_dec_block_group_ro(bg);
			btrfs_err(fs_info, "error relocating chunk %llu",
				  bg->start);
		}

next:
		if (ret) {
			/* Refcount held by the reclaim_bgs list after splice. */
			btrfs_get_block_group(bg);
			list_add_tail(&bg->bg_list, &retry_list);
		}
		btrfs_put_block_group(bg);

		mutex_unlock(&fs_info->reclaim_bgs_lock);
		/*
		 * Reclaiming all the block groups in the list can take really
		 * long.  Prioritize cleaning up unused block groups.
		 */
		btrfs_delete_unused_bgs(fs_info);
		/*
		 * If we are interrupted by a balance, we can just bail out. The
		 * cleaner thread restart again if necessary.
		 */
		if (!mutex_trylock(&fs_info->reclaim_bgs_lock))
			goto end;
		spin_lock(&fs_info->unused_bgs_lock);
	}
	spin_unlock(&fs_info->unused_bgs_lock);
	mutex_unlock(&fs_info->reclaim_bgs_lock);
end:
	spin_lock(&fs_info->unused_bgs_lock);
	list_splice_tail(&retry_list, &fs_info->reclaim_bgs);
	spin_unlock(&fs_info->unused_bgs_lock);
	btrfs_exclop_finish(fs_info);
	sb_end_write(fs_info->sb);
}

void btrfs_reclaim_bgs(struct btrfs_fs_info *fs_info)
{
	spin_lock(&fs_info->unused_bgs_lock);
	if (!list_empty(&fs_info->reclaim_bgs))
		queue_work(system_unbound_wq, &fs_info->reclaim_bgs_work);
	spin_unlock(&fs_info->unused_bgs_lock);
}

void btrfs_mark_bg_to_reclaim(struct btrfs_block_group *bg)
{
	struct btrfs_fs_info *fs_info = bg->fs_info;

	spin_lock(&fs_info->unused_bgs_lock);
	if (list_empty(&bg->bg_list)) {
		btrfs_get_block_group(bg);
		trace_btrfs_add_reclaim_block_group(bg);
		list_add_tail(&bg->bg_list, &fs_info->reclaim_bgs);
	}
	spin_unlock(&fs_info->unused_bgs_lock);
}

static int read_bg_from_eb(struct btrfs_fs_info *fs_info, struct btrfs_key *key,
			   struct btrfs_path *path)
{
	struct btrfs_chunk_map *map;
	struct btrfs_block_group_item bg;
	struct extent_buffer *leaf;
	int slot;
	u64 flags;
	int ret = 0;

	slot = path->slots[0];
	leaf = path->nodes[0];

	map = btrfs_find_chunk_map(fs_info, key->objectid, key->offset);
	if (!map) {
		btrfs_err(fs_info,
			  "logical %llu len %llu found bg but no related chunk",
			  key->objectid, key->offset);
		return -ENOENT;
	}

	if (map->start != key->objectid || map->chunk_len != key->offset) {
		btrfs_err(fs_info,
			"block group %llu len %llu mismatch with chunk %llu len %llu",
			  key->objectid, key->offset, map->start, map->chunk_len);
		ret = -EUCLEAN;
		goto out_free_map;
	}

	read_extent_buffer(leaf, &bg, btrfs_item_ptr_offset(leaf, slot),
			   sizeof(bg));
	flags = btrfs_stack_block_group_flags(&bg) &
		BTRFS_BLOCK_GROUP_TYPE_MASK;

	if (flags != (map->type & BTRFS_BLOCK_GROUP_TYPE_MASK)) {
		btrfs_err(fs_info,
"block group %llu len %llu type flags 0x%llx mismatch with chunk type flags 0x%llx",
			  key->objectid, key->offset, flags,
			  (BTRFS_BLOCK_GROUP_TYPE_MASK & map->type));
		ret = -EUCLEAN;
	}

out_free_map:
	btrfs_free_chunk_map(map);
	return ret;
}

static int find_first_block_group(struct btrfs_fs_info *fs_info,
				  struct btrfs_path *path,
				  struct btrfs_key *key)
{
	struct btrfs_root *root = btrfs_block_group_root(fs_info);
	int ret;
	struct btrfs_key found_key;

	btrfs_for_each_slot(root, key, &found_key, path, ret) {
		if (found_key.objectid >= key->objectid &&
		    found_key.type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
			return read_bg_from_eb(fs_info, &found_key, path);
		}
	}
	return ret;
}

static void set_avail_alloc_bits(struct btrfs_fs_info *fs_info, u64 flags)
{
	u64 extra_flags = chunk_to_extended(flags) &
				BTRFS_EXTENDED_PROFILE_MASK;

	write_seqlock(&fs_info->profiles_lock);
	if (flags & BTRFS_BLOCK_GROUP_DATA)
		fs_info->avail_data_alloc_bits |= extra_flags;
	if (flags & BTRFS_BLOCK_GROUP_METADATA)
		fs_info->avail_metadata_alloc_bits |= extra_flags;
	if (flags & BTRFS_BLOCK_GROUP_SYSTEM)
		fs_info->avail_system_alloc_bits |= extra_flags;
	write_sequnlock(&fs_info->profiles_lock);
}

/*
 * Map a physical disk address to a list of logical addresses.
 *
 * @fs_info:       the filesystem
 * @chunk_start:   logical address of block group
 * @physical:	   physical address to map to logical addresses
 * @logical:	   return array of logical addresses which map to @physical
 * @naddrs:	   length of @logical
 * @stripe_len:    size of IO stripe for the given block group
 *
 * Maps a particular @physical disk address to a list of @logical addresses.
 * Used primarily to exclude those portions of a block group that contain super
 * block copies.
 */
int btrfs_rmap_block(struct btrfs_fs_info *fs_info, u64 chunk_start,
		     u64 physical, u64 **logical, int *naddrs, int *stripe_len)
{
	struct btrfs_chunk_map *map;
	u64 *buf;
	u64 bytenr;
	u64 data_stripe_length;
	u64 io_stripe_size;
	int i, nr = 0;
	int ret = 0;

	map = btrfs_get_chunk_map(fs_info, chunk_start, 1);
	if (IS_ERR(map))
		return -EIO;

	data_stripe_length = map->stripe_size;
	io_stripe_size = BTRFS_STRIPE_LEN;
	chunk_start = map->start;

	/* For RAID5/6 adjust to a full IO stripe length */
	if (map->type & BTRFS_BLOCK_GROUP_RAID56_MASK)
		io_stripe_size = btrfs_stripe_nr_to_offset(nr_data_stripes(map));

	buf = kcalloc(map->num_stripes, sizeof(u64), GFP_NOFS);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < map->num_stripes; i++) {
		bool already_inserted = false;
		u32 stripe_nr;
		u32 offset;
		int j;

		if (!in_range(physical, map->stripes[i].physical,
			      data_stripe_length))
			continue;

		stripe_nr = (physical - map->stripes[i].physical) >>
			    BTRFS_STRIPE_LEN_SHIFT;
		offset = (physical - map->stripes[i].physical) &
			 BTRFS_STRIPE_LEN_MASK;

		if (map->type & (BTRFS_BLOCK_GROUP_RAID0 |
				 BTRFS_BLOCK_GROUP_RAID10))
			stripe_nr = div_u64(stripe_nr * map->num_stripes + i,
					    map->sub_stripes);
		/*
		 * The remaining case would be for RAID56, multiply by
		 * nr_data_stripes().  Alternatively, just use rmap_len below
		 * instead of map->stripe_len
		 */
		bytenr = chunk_start + stripe_nr * io_stripe_size + offset;

		/* Ensure we don't add duplicate addresses */
		for (j = 0; j < nr; j++) {
			if (buf[j] == bytenr) {
				already_inserted = true;
				break;
			}
		}

		if (!already_inserted)
			buf[nr++] = bytenr;
	}

	*logical = buf;
	*naddrs = nr;
	*stripe_len = io_stripe_size;
out:
	btrfs_free_chunk_map(map);
	return ret;
}

static int exclude_super_stripes(struct btrfs_block_group *cache)
{
	struct btrfs_fs_info *fs_info = cache->fs_info;
	const bool zoned = btrfs_is_zoned(fs_info);
	u64 bytenr;
	u64 *logical;
	int stripe_len;
	int i, nr, ret;

	if (cache->start < BTRFS_SUPER_INFO_OFFSET) {
		stripe_len = BTRFS_SUPER_INFO_OFFSET - cache->start;
		cache->bytes_super += stripe_len;
		ret = set_extent_bit(&fs_info->excluded_extents, cache->start,
				     cache->start + stripe_len - 1,
				     EXTENT_UPTODATE, NULL);
		if (ret)
			return ret;
	}

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(fs_info, cache->start,
				       bytenr, &logical, &nr, &stripe_len);
		if (ret)
			return ret;

		/* Shouldn't have super stripes in sequential zones */
		if (zoned && nr) {
			kfree(logical);
			btrfs_err(fs_info,
			"zoned: block group %llu must not contain super block",
				  cache->start);
			return -EUCLEAN;
		}

		while (nr--) {
			u64 len = min_t(u64, stripe_len,
				cache->start + cache->length - logical[nr]);

			cache->bytes_super += len;
			ret = set_extent_bit(&fs_info->excluded_extents, logical[nr],
					     logical[nr] + len - 1,
					     EXTENT_UPTODATE, NULL);
			if (ret) {
				kfree(logical);
				return ret;
			}
		}

		kfree(logical);
	}
	return 0;
}

static struct btrfs_block_group *btrfs_create_block_group_cache(
		struct btrfs_fs_info *fs_info, u64 start)
{
	struct btrfs_block_group *cache;

	cache = kzalloc(sizeof(*cache), GFP_NOFS);
	if (!cache)
		return NULL;

	cache->free_space_ctl = kzalloc(sizeof(*cache->free_space_ctl),
					GFP_NOFS);
	if (!cache->free_space_ctl) {
		kfree(cache);
		return NULL;
	}

	cache->start = start;

	cache->fs_info = fs_info;
	cache->full_stripe_len = btrfs_full_stripe_len(fs_info, start);

	cache->discard_index = BTRFS_DISCARD_INDEX_UNUSED;

	refcount_set(&cache->refs, 1);
	spin_lock_init(&cache->lock);
	init_rwsem(&cache->data_rwsem);
	INIT_LIST_HEAD(&cache->list);
	INIT_LIST_HEAD(&cache->cluster_list);
	INIT_LIST_HEAD(&cache->bg_list);
	INIT_LIST_HEAD(&cache->ro_list);
	INIT_LIST_HEAD(&cache->discard_list);
	INIT_LIST_HEAD(&cache->dirty_list);
	INIT_LIST_HEAD(&cache->io_list);
	INIT_LIST_HEAD(&cache->active_bg_list);
	btrfs_init_free_space_ctl(cache, cache->free_space_ctl);
	atomic_set(&cache->frozen, 0);
	mutex_init(&cache->free_space_lock);

	return cache;
}

/*
 * Iterate all chunks and verify that each of them has the corresponding block
 * group
 */
static int check_chunk_block_group_mappings(struct btrfs_fs_info *fs_info)
{
	u64 start = 0;
	int ret = 0;

	while (1) {
		struct btrfs_chunk_map *map;
		struct btrfs_block_group *bg;

		/*
		 * btrfs_find_chunk_map() will return the first chunk map
		 * intersecting the range, so setting @length to 1 is enough to
		 * get the first chunk.
		 */
		map = btrfs_find_chunk_map(fs_info, start, 1);
		if (!map)
			break;

		bg = btrfs_lookup_block_group(fs_info, map->start);
		if (!bg) {
			btrfs_err(fs_info,
	"chunk start=%llu len=%llu doesn't have corresponding block group",
				     map->start, map->chunk_len);
			ret = -EUCLEAN;
			btrfs_free_chunk_map(map);
			break;
		}
		if (bg->start != map->start || bg->length != map->chunk_len ||
		    (bg->flags & BTRFS_BLOCK_GROUP_TYPE_MASK) !=
		    (map->type & BTRFS_BLOCK_GROUP_TYPE_MASK)) {
			btrfs_err(fs_info,
"chunk start=%llu len=%llu flags=0x%llx doesn't match block group start=%llu len=%llu flags=0x%llx",
				map->start, map->chunk_len,
				map->type & BTRFS_BLOCK_GROUP_TYPE_MASK,
				bg->start, bg->length,
				bg->flags & BTRFS_BLOCK_GROUP_TYPE_MASK);
			ret = -EUCLEAN;
			btrfs_free_chunk_map(map);
			btrfs_put_block_group(bg);
			break;
		}
		start = map->start + map->chunk_len;
		btrfs_free_chunk_map(map);
		btrfs_put_block_group(bg);
	}
	return ret;
}

static int read_one_block_group(struct btrfs_fs_info *info,
				struct btrfs_block_group_item *bgi,
				const struct btrfs_key *key,
				int need_clear)
{
	struct btrfs_block_group *cache;
	const bool mixed = btrfs_fs_incompat(info, MIXED_GROUPS);
	int ret;

	ASSERT(key->type == BTRFS_BLOCK_GROUP_ITEM_KEY);

	cache = btrfs_create_block_group_cache(info, key->objectid);
	if (!cache)
		return -ENOMEM;

	cache->length = key->offset;
	cache->used = btrfs_stack_block_group_used(bgi);
	cache->commit_used = cache->used;
	cache->flags = btrfs_stack_block_group_flags(bgi);
	cache->global_root_id = btrfs_stack_block_group_chunk_objectid(bgi);

	set_free_space_tree_thresholds(cache);

	if (need_clear) {
		/*
		 * When we mount with old space cache, we need to
		 * set BTRFS_DC_CLEAR and set dirty flag.
		 *
		 * a) Setting 'BTRFS_DC_CLEAR' makes sure that we
		 *    truncate the old free space cache inode and
		 *    setup a new one.
		 * b) Setting 'dirty flag' makes sure that we flush
		 *    the new space cache info onto disk.
		 */
		if (btrfs_test_opt(info, SPACE_CACHE))
			cache->disk_cache_state = BTRFS_DC_CLEAR;
	}
	if (!mixed && ((cache->flags & BTRFS_BLOCK_GROUP_METADATA) &&
	    (cache->flags & BTRFS_BLOCK_GROUP_DATA))) {
			btrfs_err(info,
"bg %llu is a mixed block group but filesystem hasn't enabled mixed block groups",
				  cache->start);
			ret = -EINVAL;
			goto error;
	}

	ret = btrfs_load_block_group_zone_info(cache, false);
	if (ret) {
		btrfs_err(info, "zoned: failed to load zone info of bg %llu",
			  cache->start);
		goto error;
	}

	/*
	 * We need to exclude the super stripes now so that the space info has
	 * super bytes accounted for, otherwise we'll think we have more space
	 * than we actually do.
	 */
	ret = exclude_super_stripes(cache);
	if (ret) {
		/* We may have excluded something, so call this just in case. */
		btrfs_free_excluded_extents(cache);
		goto error;
	}

	/*
	 * For zoned filesystem, space after the allocation offset is the only
	 * free space for a block group. So, we don't need any caching work.
	 * btrfs_calc_zone_unusable() will set the amount of free space and
	 * zone_unusable space.
	 *
	 * For regular filesystem, check for two cases, either we are full, and
	 * therefore don't need to bother with the caching work since we won't
	 * find any space, or we are empty, and we can just add all the space
	 * in and be done with it.  This saves us _a_lot_ of time, particularly
	 * in the full case.
	 */
	if (btrfs_is_zoned(info)) {
		btrfs_calc_zone_unusable(cache);
		/* Should not have any excluded extents. Just in case, though. */
		btrfs_free_excluded_extents(cache);
	} else if (cache->length == cache->used) {
		cache->cached = BTRFS_CACHE_FINISHED;
		btrfs_free_excluded_extents(cache);
	} else if (cache->used == 0) {
		cache->cached = BTRFS_CACHE_FINISHED;
		ret = btrfs_add_new_free_space(cache, cache->start,
					       cache->start + cache->length, NULL);
		btrfs_free_excluded_extents(cache);
		if (ret)
			goto error;
	}

	ret = btrfs_add_block_group_cache(info, cache);
	if (ret) {
		btrfs_remove_free_space_cache(cache);
		goto error;
	}
	trace_btrfs_add_block_group(info, cache, 0);
	btrfs_add_bg_to_space_info(info, cache);

	set_avail_alloc_bits(info, cache->flags);
	if (btrfs_chunk_writeable(info, cache->start)) {
		if (cache->used == 0) {
			ASSERT(list_empty(&cache->bg_list));
			if (btrfs_test_opt(info, DISCARD_ASYNC))
				btrfs_discard_queue_work(&info->discard_ctl, cache);
			else
				btrfs_mark_bg_unused(cache);
		}
	} else {
		inc_block_group_ro(cache, 1);
	}

	return 0;
error:
	btrfs_put_block_group(cache);
	return ret;
}

static int fill_dummy_bgs(struct btrfs_fs_info *fs_info)
{
	struct rb_node *node;
	int ret = 0;

	for (node = rb_first_cached(&fs_info->mapping_tree); node; node = rb_next(node)) {
		struct btrfs_chunk_map *map;
		struct btrfs_block_group *bg;

		map = rb_entry(node, struct btrfs_chunk_map, rb_node);
		bg = btrfs_create_block_group_cache(fs_info, map->start);
		if (!bg) {
			ret = -ENOMEM;
			break;
		}

		/* Fill dummy cache as FULL */
		bg->length = map->chunk_len;
		bg->flags = map->type;
		bg->cached = BTRFS_CACHE_FINISHED;
		bg->used = map->chunk_len;
		bg->flags = map->type;
		ret = btrfs_add_block_group_cache(fs_info, bg);
		/*
		 * We may have some valid block group cache added already, in
		 * that case we skip to the next one.
		 */
		if (ret == -EEXIST) {
			ret = 0;
			btrfs_put_block_group(bg);
			continue;
		}

		if (ret) {
			btrfs_remove_free_space_cache(bg);
			btrfs_put_block_group(bg);
			break;
		}

		btrfs_add_bg_to_space_info(fs_info, bg);

		set_avail_alloc_bits(fs_info, bg->flags);
	}
	if (!ret)
		btrfs_init_global_block_rsv(fs_info);
	return ret;
}

int btrfs_read_block_groups(struct btrfs_fs_info *info)
{
	struct btrfs_root *root = btrfs_block_group_root(info);
	struct btrfs_path *path;
	int ret;
	struct btrfs_block_group *cache;
	struct btrfs_space_info *space_info;
	struct btrfs_key key;
	int need_clear = 0;
	u64 cache_gen;

	/*
	 * Either no extent root (with ibadroots rescue option) or we have
	 * unsupported RO options. The fs can never be mounted read-write, so no
	 * need to waste time searching block group items.
	 *
	 * This also allows new extent tree related changes to be RO compat,
	 * no need for a full incompat flag.
	 */
	if (!root || (btrfs_super_compat_ro_flags(info->super_copy) &
		      ~BTRFS_FEATURE_COMPAT_RO_SUPP))
		return fill_dummy_bgs(info);

	key.objectid = 0;
	key.offset = 0;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	cache_gen = btrfs_super_cache_generation(info->super_copy);
	if (btrfs_test_opt(info, SPACE_CACHE) &&
	    btrfs_super_generation(info->super_copy) != cache_gen)
		need_clear = 1;
	if (btrfs_test_opt(info, CLEAR_CACHE))
		need_clear = 1;

	while (1) {
		struct btrfs_block_group_item bgi;
		struct extent_buffer *leaf;
		int slot;

		ret = find_first_block_group(info, path, &key);
		if (ret > 0)
			break;
		if (ret != 0)
			goto error;

		leaf = path->nodes[0];
		slot = path->slots[0];

		read_extent_buffer(leaf, &bgi, btrfs_item_ptr_offset(leaf, slot),
				   sizeof(bgi));

		btrfs_item_key_to_cpu(leaf, &key, slot);
		btrfs_release_path(path);
		ret = read_one_block_group(info, &bgi, &key, need_clear);
		if (ret < 0)
			goto error;
		key.objectid += key.offset;
		key.offset = 0;
	}
	btrfs_release_path(path);

	list_for_each_entry(space_info, &info->space_info, list) {
		int i;

		for (i = 0; i < BTRFS_NR_RAID_TYPES; i++) {
			if (list_empty(&space_info->block_groups[i]))
				continue;
			cache = list_first_entry(&space_info->block_groups[i],
						 struct btrfs_block_group,
						 list);
			btrfs_sysfs_add_block_group_type(cache);
		}

		if (!(btrfs_get_alloc_profile(info, space_info->flags) &
		      (BTRFS_BLOCK_GROUP_RAID10 |
		       BTRFS_BLOCK_GROUP_RAID1_MASK |
		       BTRFS_BLOCK_GROUP_RAID56_MASK |
		       BTRFS_BLOCK_GROUP_DUP)))
			continue;
		/*
		 * Avoid allocating from un-mirrored block group if there are
		 * mirrored block groups.
		 */
		list_for_each_entry(cache,
				&space_info->block_groups[BTRFS_RAID_RAID0],
				list)
			inc_block_group_ro(cache, 1);
		list_for_each_entry(cache,
				&space_info->block_groups[BTRFS_RAID_SINGLE],
				list)
			inc_block_group_ro(cache, 1);
	}

	btrfs_init_global_block_rsv(info);
	ret = check_chunk_block_group_mappings(info);
error:
	btrfs_free_path(path);
	/*
	 * We've hit some error while reading the extent tree, and have
	 * rescue=ibadroots mount option.
	 * Try to fill the tree using dummy block groups so that the user can
	 * continue to mount and grab their data.
	 */
	if (ret && btrfs_test_opt(info, IGNOREBADROOTS))
		ret = fill_dummy_bgs(info);
	return ret;
}

/*
 * This function, insert_block_group_item(), belongs to the phase 2 of chunk
 * allocation.
 *
 * See the comment at btrfs_chunk_alloc() for details about the chunk allocation
 * phases.
 */
static int insert_block_group_item(struct btrfs_trans_handle *trans,
				   struct btrfs_block_group *block_group)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group_item bgi;
	struct btrfs_root *root = btrfs_block_group_root(fs_info);
	struct btrfs_key key;
	u64 old_commit_used;
	int ret;

	spin_lock(&block_group->lock);
	btrfs_set_stack_block_group_used(&bgi, block_group->used);
	btrfs_set_stack_block_group_chunk_objectid(&bgi,
						   block_group->global_root_id);
	btrfs_set_stack_block_group_flags(&bgi, block_group->flags);
	old_commit_used = block_group->commit_used;
	block_group->commit_used = block_group->used;
	key.objectid = block_group->start;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = block_group->length;
	spin_unlock(&block_group->lock);

	ret = btrfs_insert_item(trans, root, &key, &bgi, sizeof(bgi));
	if (ret < 0) {
		spin_lock(&block_group->lock);
		block_group->commit_used = old_commit_used;
		spin_unlock(&block_group->lock);
	}

	return ret;
}

static int insert_dev_extent(struct btrfs_trans_handle *trans,
			    struct btrfs_device *device, u64 chunk_offset,
			    u64 start, u64 num_bytes)
{
	struct btrfs_fs_info *fs_info = device->fs_info;
	struct btrfs_root *root = fs_info->dev_root;
	struct btrfs_path *path;
	struct btrfs_dev_extent *extent;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int ret;

	WARN_ON(!test_bit(BTRFS_DEV_STATE_IN_FS_METADATA, &device->dev_state));
	WARN_ON(test_bit(BTRFS_DEV_STATE_REPLACE_TGT, &device->dev_state));
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = device->devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = start;
	ret = btrfs_insert_empty_item(trans, root, path, &key, sizeof(*extent));
	if (ret)
		goto out;

	leaf = path->nodes[0];
	extent = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_dev_extent);
	btrfs_set_dev_extent_chunk_tree(leaf, extent, BTRFS_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_chunk_objectid(leaf, extent,
					    BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_chunk_offset(leaf, extent, chunk_offset);

	btrfs_set_dev_extent_length(leaf, extent, num_bytes);
	btrfs_mark_buffer_dirty(trans, leaf);
out:
	btrfs_free_path(path);
	return ret;
}

/*
 * This function belongs to phase 2.
 *
 * See the comment at btrfs_chunk_alloc() for details about the chunk allocation
 * phases.
 */
static int insert_dev_extents(struct btrfs_trans_handle *trans,
				   u64 chunk_offset, u64 chunk_size)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_device *device;
	struct btrfs_chunk_map *map;
	u64 dev_offset;
	int i;
	int ret = 0;

	map = btrfs_get_chunk_map(fs_info, chunk_offset, chunk_size);
	if (IS_ERR(map))
		return PTR_ERR(map);

	/*
	 * Take the device list mutex to prevent races with the final phase of
	 * a device replace operation that replaces the device object associated
	 * with the map's stripes, because the device object's id can change
	 * at any time during that final phase of the device replace operation
	 * (dev-replace.c:btrfs_dev_replace_finishing()), so we could grab the
	 * replaced device and then see it with an ID of BTRFS_DEV_REPLACE_DEVID,
	 * resulting in persisting a device extent item with such ID.
	 */
	mutex_lock(&fs_info->fs_devices->device_list_mutex);
	for (i = 0; i < map->num_stripes; i++) {
		device = map->stripes[i].dev;
		dev_offset = map->stripes[i].physical;

		ret = insert_dev_extent(trans, device, chunk_offset, dev_offset,
					map->stripe_size);
		if (ret)
			break;
	}
	mutex_unlock(&fs_info->fs_devices->device_list_mutex);

	btrfs_free_chunk_map(map);
	return ret;
}

/*
 * This function, btrfs_create_pending_block_groups(), belongs to the phase 2 of
 * chunk allocation.
 *
 * See the comment at btrfs_chunk_alloc() for details about the chunk allocation
 * phases.
 */
void btrfs_create_pending_block_groups(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *block_group;
	int ret = 0;

	while (!list_empty(&trans->new_bgs)) {
		int index;

		block_group = list_first_entry(&trans->new_bgs,
					       struct btrfs_block_group,
					       bg_list);
		if (ret)
			goto next;

		index = btrfs_bg_flags_to_raid_index(block_group->flags);

		ret = insert_block_group_item(trans, block_group);
		if (ret)
			btrfs_abort_transaction(trans, ret);
		if (!test_bit(BLOCK_GROUP_FLAG_CHUNK_ITEM_INSERTED,
			      &block_group->runtime_flags)) {
			mutex_lock(&fs_info->chunk_mutex);
			ret = btrfs_chunk_alloc_add_chunk_item(trans, block_group);
			mutex_unlock(&fs_info->chunk_mutex);
			if (ret)
				btrfs_abort_transaction(trans, ret);
		}
		ret = insert_dev_extents(trans, block_group->start,
					 block_group->length);
		if (ret)
			btrfs_abort_transaction(trans, ret);
		add_block_group_free_space(trans, block_group);

		/*
		 * If we restriped during balance, we may have added a new raid
		 * type, so now add the sysfs entries when it is safe to do so.
		 * We don't have to worry about locking here as it's handled in
		 * btrfs_sysfs_add_block_group_type.
		 */
		if (block_group->space_info->block_group_kobjs[index] == NULL)
			btrfs_sysfs_add_block_group_type(block_group);

		/* Already aborted the transaction if it failed. */
next:
		btrfs_dec_delayed_refs_rsv_bg_inserts(fs_info);
		list_del_init(&block_group->bg_list);
		clear_bit(BLOCK_GROUP_FLAG_NEW, &block_group->runtime_flags);

		/*
		 * If the block group is still unused, add it to the list of
		 * unused block groups. The block group may have been created in
		 * order to satisfy a space reservation, in which case the
		 * extent allocation only happens later. But often we don't
		 * actually need to allocate space that we previously reserved,
		 * so the block group may become unused for a long time. For
		 * example for metadata we generally reserve space for a worst
		 * possible scenario, but then don't end up allocating all that
		 * space or none at all (due to no need to COW, extent buffers
		 * were already COWed in the current transaction and still
		 * unwritten, tree heights lower than the maximum possible
		 * height, etc). For data we generally reserve the axact amount
		 * of space we are going to allocate later, the exception is
		 * when using compression, as we must reserve space based on the
		 * uncompressed data size, because the compression is only done
		 * when writeback triggered and we don't know how much space we
		 * are actually going to need, so we reserve the uncompressed
		 * size because the data may be uncompressible in the worst case.
		 */
		if (ret == 0) {
			bool used;

			spin_lock(&block_group->lock);
			used = btrfs_is_block_group_used(block_group);
			spin_unlock(&block_group->lock);

			if (!used)
				btrfs_mark_bg_unused(block_group);
		}
	}
	btrfs_trans_release_chunk_metadata(trans);
}

/*
 * For extent tree v2 we use the block_group_item->chunk_offset to point at our
 * global root id.  For v1 it's always set to BTRFS_FIRST_CHUNK_TREE_OBJECTID.
 */
static u64 calculate_global_root_id(struct btrfs_fs_info *fs_info, u64 offset)
{
	u64 div = SZ_1G;
	u64 index;

	if (!btrfs_fs_incompat(fs_info, EXTENT_TREE_V2))
		return BTRFS_FIRST_CHUNK_TREE_OBJECTID;

	/* If we have a smaller fs index based on 128MiB. */
	if (btrfs_super_total_bytes(fs_info->super_copy) <= (SZ_1G * 10ULL))
		div = SZ_128M;

	offset = div64_u64(offset, div);
	div64_u64_rem(offset, fs_info->nr_global_roots, &index);
	return index;
}

struct btrfs_block_group *btrfs_make_block_group(struct btrfs_trans_handle *trans,
						 u64 type,
						 u64 chunk_offset, u64 size)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *cache;
	int ret;

	btrfs_set_log_full_commit(trans);

	cache = btrfs_create_block_group_cache(fs_info, chunk_offset);
	if (!cache)
		return ERR_PTR(-ENOMEM);

	/*
	 * Mark it as new before adding it to the rbtree of block groups or any
	 * list, so that no other task finds it and calls btrfs_mark_bg_unused()
	 * before the new flag is set.
	 */
	set_bit(BLOCK_GROUP_FLAG_NEW, &cache->runtime_flags);

	cache->length = size;
	set_free_space_tree_thresholds(cache);
	cache->flags = type;
	cache->cached = BTRFS_CACHE_FINISHED;
	cache->global_root_id = calculate_global_root_id(fs_info, cache->start);

	if (btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE))
		set_bit(BLOCK_GROUP_FLAG_NEEDS_FREE_SPACE, &cache->runtime_flags);

	ret = btrfs_load_block_group_zone_info(cache, true);
	if (ret) {
		btrfs_put_block_group(cache);
		return ERR_PTR(ret);
	}

	ret = exclude_super_stripes(cache);
	if (ret) {
		/* We may have excluded something, so call this just in case */
		btrfs_free_excluded_extents(cache);
		btrfs_put_block_group(cache);
		return ERR_PTR(ret);
	}

	ret = btrfs_add_new_free_space(cache, chunk_offset, chunk_offset + size, NULL);
	btrfs_free_excluded_extents(cache);
	if (ret) {
		btrfs_put_block_group(cache);
		return ERR_PTR(ret);
	}

	/*
	 * Ensure the corresponding space_info object is created and
	 * assigned to our block group. We want our bg to be added to the rbtree
	 * with its ->space_info set.
	 */
	cache->space_info = btrfs_find_space_info(fs_info, cache->flags);
	ASSERT(cache->space_info);

	ret = btrfs_add_block_group_cache(fs_info, cache);
	if (ret) {
		btrfs_remove_free_space_cache(cache);
		btrfs_put_block_group(cache);
		return ERR_PTR(ret);
	}

	/*
	 * Now that our block group has its ->space_info set and is inserted in
	 * the rbtree, update the space info's counters.
	 */
	trace_btrfs_add_block_group(fs_info, cache, 1);
	btrfs_add_bg_to_space_info(fs_info, cache);
	btrfs_update_global_block_rsv(fs_info);

#ifdef CONFIG_BTRFS_DEBUG
	if (btrfs_should_fragment_free_space(cache)) {
		cache->space_info->bytes_used += size >> 1;
		fragment_free_space(cache);
	}
#endif

	list_add_tail(&cache->bg_list, &trans->new_bgs);
	btrfs_inc_delayed_refs_rsv_bg_inserts(fs_info);

	set_avail_alloc_bits(fs_info, type);
	return cache;
}

/*
 * Mark one block group RO, can be called several times for the same block
 * group.
 *
 * @cache:		the destination block group
 * @do_chunk_alloc:	whether need to do chunk pre-allocation, this is to
 * 			ensure we still have some free space after marking this
 * 			block group RO.
 */
int btrfs_inc_block_group_ro(struct btrfs_block_group *cache,
			     bool do_chunk_alloc)
{
	struct btrfs_fs_info *fs_info = cache->fs_info;
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = btrfs_block_group_root(fs_info);
	u64 alloc_flags;
	int ret;
	bool dirty_bg_running;

	/*
	 * This can only happen when we are doing read-only scrub on read-only
	 * mount.
	 * In that case we should not start a new transaction on read-only fs.
	 * Thus here we skip all chunk allocations.
	 */
	if (sb_rdonly(fs_info->sb)) {
		mutex_lock(&fs_info->ro_block_group_mutex);
		ret = inc_block_group_ro(cache, 0);
		mutex_unlock(&fs_info->ro_block_group_mutex);
		return ret;
	}

	do {
		trans = btrfs_join_transaction(root);
		if (IS_ERR(trans))
			return PTR_ERR(trans);

		dirty_bg_running = false;

		/*
		 * We're not allowed to set block groups readonly after the dirty
		 * block group cache has started writing.  If it already started,
		 * back off and let this transaction commit.
		 */
		mutex_lock(&fs_info->ro_block_group_mutex);
		if (test_bit(BTRFS_TRANS_DIRTY_BG_RUN, &trans->transaction->flags)) {
			u64 transid = trans->transid;

			mutex_unlock(&fs_info->ro_block_group_mutex);
			btrfs_end_transaction(trans);

			ret = btrfs_wait_for_commit(fs_info, transid);
			if (ret)
				return ret;
			dirty_bg_running = true;
		}
	} while (dirty_bg_running);

	if (do_chunk_alloc) {
		/*
		 * If we are changing raid levels, try to allocate a
		 * corresponding block group with the new raid level.
		 */
		alloc_flags = btrfs_get_alloc_profile(fs_info, cache->flags);
		if (alloc_flags != cache->flags) {
			ret = btrfs_chunk_alloc(trans, alloc_flags,
						CHUNK_ALLOC_FORCE);
			/*
			 * ENOSPC is allowed here, we may have enough space
			 * already allocated at the new raid level to carry on
			 */
			if (ret == -ENOSPC)
				ret = 0;
			if (ret < 0)
				goto out;
		}
	}

	ret = inc_block_group_ro(cache, 0);
	if (!ret)
		goto out;
	if (ret == -ETXTBSY)
		goto unlock_out;

	/*
	 * Skip chunk allocation if the bg is SYSTEM, this is to avoid system
	 * chunk allocation storm to exhaust the system chunk array.  Otherwise
	 * we still want to try our best to mark the block group read-only.
	 */
	if (!do_chunk_alloc && ret == -ENOSPC &&
	    (cache->flags & BTRFS_BLOCK_GROUP_SYSTEM))
		goto unlock_out;

	alloc_flags = btrfs_get_alloc_profile(fs_info, cache->space_info->flags);
	ret = btrfs_chunk_alloc(trans, alloc_flags, CHUNK_ALLOC_FORCE);
	if (ret < 0)
		goto out;
	/*
	 * We have allocated a new chunk. We also need to activate that chunk to
	 * grant metadata tickets for zoned filesystem.
	 */
	ret = btrfs_zoned_activate_one_bg(fs_info, cache->space_info, true);
	if (ret < 0)
		goto out;

	ret = inc_block_group_ro(cache, 0);
	if (ret == -ETXTBSY)
		goto unlock_out;
out:
	if (cache->flags & BTRFS_BLOCK_GROUP_SYSTEM) {
		alloc_flags = btrfs_get_alloc_profile(fs_info, cache->flags);
		mutex_lock(&fs_info->chunk_mutex);
		check_system_chunk(trans, alloc_flags);
		mutex_unlock(&fs_info->chunk_mutex);
	}
unlock_out:
	mutex_unlock(&fs_info->ro_block_group_mutex);

	btrfs_end_transaction(trans);
	return ret;
}

void btrfs_dec_block_group_ro(struct btrfs_block_group *cache)
{
	struct btrfs_space_info *sinfo = cache->space_info;
	u64 num_bytes;

	BUG_ON(!cache->ro);

	spin_lock(&sinfo->lock);
	spin_lock(&cache->lock);
	if (!--cache->ro) {
		if (btrfs_is_zoned(cache->fs_info)) {
			/* Migrate zone_unusable bytes back */
			cache->zone_unusable =
				(cache->alloc_offset - cache->used) +
				(cache->length - cache->zone_capacity);
			sinfo->bytes_zone_unusable += cache->zone_unusable;
			sinfo->bytes_readonly -= cache->zone_unusable;
		}
		num_bytes = cache->length - cache->reserved -
			    cache->pinned - cache->bytes_super -
			    cache->zone_unusable - cache->used;
		sinfo->bytes_readonly -= num_bytes;
		list_del_init(&cache->ro_list);
	}
	spin_unlock(&cache->lock);
	spin_unlock(&sinfo->lock);
}

static int update_block_group_item(struct btrfs_trans_handle *trans,
				   struct btrfs_path *path,
				   struct btrfs_block_group *cache)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	int ret;
	struct btrfs_root *root = btrfs_block_group_root(fs_info);
	unsigned long bi;
	struct extent_buffer *leaf;
	struct btrfs_block_group_item bgi;
	struct btrfs_key key;
	u64 old_commit_used;
	u64 used;

	/*
	 * Block group items update can be triggered out of commit transaction
	 * critical section, thus we need a consistent view of used bytes.
	 * We cannot use cache->used directly outside of the spin lock, as it
	 * may be changed.
	 */
	spin_lock(&cache->lock);
	old_commit_used = cache->commit_used;
	used = cache->used;
	/* No change in used bytes, can safely skip it. */
	if (cache->commit_used == used) {
		spin_unlock(&cache->lock);
		return 0;
	}
	cache->commit_used = used;
	spin_unlock(&cache->lock);

	key.objectid = cache->start;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = cache->length;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;
		goto fail;
	}

	leaf = path->nodes[0];
	bi = btrfs_item_ptr_offset(leaf, path->slots[0]);
	btrfs_set_stack_block_group_used(&bgi, used);
	btrfs_set_stack_block_group_chunk_objectid(&bgi,
						   cache->global_root_id);
	btrfs_set_stack_block_group_flags(&bgi, cache->flags);
	write_extent_buffer(leaf, &bgi, bi, sizeof(bgi));
	btrfs_mark_buffer_dirty(trans, leaf);
fail:
	btrfs_release_path(path);
	/*
	 * We didn't update the block group item, need to revert commit_used
	 * unless the block group item didn't exist yet - this is to prevent a
	 * race with a concurrent insertion of the block group item, with
	 * insert_block_group_item(), that happened just after we attempted to
	 * update. In that case we would reset commit_used to 0 just after the
	 * insertion set it to a value greater than 0 - if the block group later
	 * becomes with 0 used bytes, we would incorrectly skip its update.
	 */
	if (ret < 0 && ret != -ENOENT) {
		spin_lock(&cache->lock);
		cache->commit_used = old_commit_used;
		spin_unlock(&cache->lock);
	}
	return ret;

}

static int cache_save_setup(struct btrfs_block_group *block_group,
			    struct btrfs_trans_handle *trans,
			    struct btrfs_path *path)
{
	struct btrfs_fs_info *fs_info = block_group->fs_info;
	struct inode *inode = NULL;
	struct extent_changeset *data_reserved = NULL;
	u64 alloc_hint = 0;
	int dcs = BTRFS_DC_ERROR;
	u64 cache_size = 0;
	int retries = 0;
	int ret = 0;

	if (!btrfs_test_opt(fs_info, SPACE_CACHE))
		return 0;

	/*
	 * If this block group is smaller than 100 megs don't bother caching the
	 * block group.
	 */
	if (block_group->length < (100 * SZ_1M)) {
		spin_lock(&block_group->lock);
		block_group->disk_cache_state = BTRFS_DC_WRITTEN;
		spin_unlock(&block_group->lock);
		return 0;
	}

	if (TRANS_ABORTED(trans))
		return 0;
again:
	inode = lookup_free_space_inode(block_group, path);
	if (IS_ERR(inode) && PTR_ERR(inode) != -ENOENT) {
		ret = PTR_ERR(inode);
		btrfs_release_path(path);
		goto out;
	}

	if (IS_ERR(inode)) {
		BUG_ON(retries);
		retries++;

		if (block_group->ro)
			goto out_free;

		ret = create_free_space_inode(trans, block_group, path);
		if (ret)
			goto out_free;
		goto again;
	}

	/*
	 * We want to set the generation to 0, that way if anything goes wrong
	 * from here on out we know not to trust this cache when we load up next
	 * time.
	 */
	BTRFS_I(inode)->generation = 0;
	ret = btrfs_update_inode(trans, BTRFS_I(inode));
	if (ret) {
		/*
		 * So theoretically we could recover from this, simply set the
		 * super cache generation to 0 so we know to invalidate the
		 * cache, but then we'd have to keep track of the block groups
		 * that fail this way so we know we _have_ to reset this cache
		 * before the next commit or risk reading stale cache.  So to
		 * limit our exposure to horrible edge cases lets just abort the
		 * transaction, this only happens in really bad situations
		 * anyway.
		 */
		btrfs_abort_transaction(trans, ret);
		goto out_put;
	}
	WARN_ON(ret);

	/* We've already setup this transaction, go ahead and exit */
	if (block_group->cache_generation == trans->transid &&
	    i_size_read(inode)) {
		dcs = BTRFS_DC_SETUP;
		goto out_put;
	}

	if (i_size_read(inode) > 0) {
		ret = btrfs_check_trunc_cache_free_space(fs_info,
					&fs_info->global_block_rsv);
		if (ret)
			goto out_put;

		ret = btrfs_truncate_free_space_cache(trans, NULL, inode);
		if (ret)
			goto out_put;
	}

	spin_lock(&block_group->lock);
	if (block_group->cached != BTRFS_CACHE_FINISHED ||
	    !btrfs_test_opt(fs_info, SPACE_CACHE)) {
		/*
		 * don't bother trying to write stuff out _if_
		 * a) we're not cached,
		 * b) we're with nospace_cache mount option,
		 * c) we're with v2 space_cache (FREE_SPACE_TREE).
		 */
		dcs = BTRFS_DC_WRITTEN;
		spin_unlock(&block_group->lock);
		goto out_put;
	}
	spin_unlock(&block_group->lock);

	/*
	 * We hit an ENOSPC when setting up the cache in this transaction, just
	 * skip doing the setup, we've already cleared the cache so we're safe.
	 */
	if (test_bit(BTRFS_TRANS_CACHE_ENOSPC, &trans->transaction->flags)) {
		ret = -ENOSPC;
		goto out_put;
	}

	/*
	 * Try to preallocate enough space based on how big the block group is.
	 * Keep in mind this has to include any pinned space which could end up
	 * taking up quite a bit since it's not folded into the other space
	 * cache.
	 */
	cache_size = div_u64(block_group->length, SZ_256M);
	if (!cache_size)
		cache_size = 1;

	cache_size *= 16;
	cache_size *= fs_info->sectorsize;

	ret = btrfs_check_data_free_space(BTRFS_I(inode), &data_reserved, 0,
					  cache_size, false);
	if (ret)
		goto out_put;

	ret = btrfs_prealloc_file_range_trans(inode, trans, 0, 0, cache_size,
					      cache_size, cache_size,
					      &alloc_hint);
	/*
	 * Our cache requires contiguous chunks so that we don't modify a bunch
	 * of metadata or split extents when writing the cache out, which means
	 * we can enospc if we are heavily fragmented in addition to just normal
	 * out of space conditions.  So if we hit this just skip setting up any
	 * other block groups for this transaction, maybe we'll unpin enough
	 * space the next time around.
	 */
	if (!ret)
		dcs = BTRFS_DC_SETUP;
	else if (ret == -ENOSPC)
		set_bit(BTRFS_TRANS_CACHE_ENOSPC, &trans->transaction->flags);

out_put:
	iput(inode);
out_free:
	btrfs_release_path(path);
out:
	spin_lock(&block_group->lock);
	if (!ret && dcs == BTRFS_DC_SETUP)
		block_group->cache_generation = trans->transid;
	block_group->disk_cache_state = dcs;
	spin_unlock(&block_group->lock);

	extent_changeset_free(data_reserved);
	return ret;
}

int btrfs_setup_space_cache(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *cache, *tmp;
	struct btrfs_transaction *cur_trans = trans->transaction;
	struct btrfs_path *path;

	if (list_empty(&cur_trans->dirty_bgs) ||
	    !btrfs_test_opt(fs_info, SPACE_CACHE))
		return 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* Could add new block groups, use _safe just in case */
	list_for_each_entry_safe(cache, tmp, &cur_trans->dirty_bgs,
				 dirty_list) {
		if (cache->disk_cache_state == BTRFS_DC_CLEAR)
			cache_save_setup(cache, trans, path);
	}

	btrfs_free_path(path);
	return 0;
}

/*
 * Transaction commit does final block group cache writeback during a critical
 * section where nothing is allowed to change the FS.  This is required in
 * order for the cache to actually match the block group, but can introduce a
 * lot of latency into the commit.
 *
 * So, btrfs_start_dirty_block_groups is here to kick off block group cache IO.
 * There's a chance we'll have to redo some of it if the block group changes
 * again during the commit, but it greatly reduces the commit latency by
 * getting rid of the easy block groups while we're still allowing others to
 * join the commit.
 */
int btrfs_start_dirty_block_groups(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *cache;
	struct btrfs_transaction *cur_trans = trans->transaction;
	int ret = 0;
	int should_put;
	struct btrfs_path *path = NULL;
	LIST_HEAD(dirty);
	struct list_head *io = &cur_trans->io_bgs;
	int loops = 0;

	spin_lock(&cur_trans->dirty_bgs_lock);
	if (list_empty(&cur_trans->dirty_bgs)) {
		spin_unlock(&cur_trans->dirty_bgs_lock);
		return 0;
	}
	list_splice_init(&cur_trans->dirty_bgs, &dirty);
	spin_unlock(&cur_trans->dirty_bgs_lock);

again:
	/* Make sure all the block groups on our dirty list actually exist */
	btrfs_create_pending_block_groups(trans);

	if (!path) {
		path = btrfs_alloc_path();
		if (!path) {
			ret = -ENOMEM;
			goto out;
		}
	}

	/*
	 * cache_write_mutex is here only to save us from balance or automatic
	 * removal of empty block groups deleting this block group while we are
	 * writing out the cache
	 */
	mutex_lock(&trans->transaction->cache_write_mutex);
	while (!list_empty(&dirty)) {
		bool drop_reserve = true;

		cache = list_first_entry(&dirty, struct btrfs_block_group,
					 dirty_list);
		/*
		 * This can happen if something re-dirties a block group that
		 * is already under IO.  Just wait for it to finish and then do
		 * it all again
		 */
		if (!list_empty(&cache->io_list)) {
			list_del_init(&cache->io_list);
			btrfs_wait_cache_io(trans, cache, path);
			btrfs_put_block_group(cache);
		}


		/*
		 * btrfs_wait_cache_io uses the cache->dirty_list to decide if
		 * it should update the cache_state.  Don't delete until after
		 * we wait.
		 *
		 * Since we're not running in the commit critical section
		 * we need the dirty_bgs_lock to protect from update_block_group
		 */
		spin_lock(&cur_trans->dirty_bgs_lock);
		list_del_init(&cache->dirty_list);
		spin_unlock(&cur_trans->dirty_bgs_lock);

		should_put = 1;

		cache_save_setup(cache, trans, path);

		if (cache->disk_cache_state == BTRFS_DC_SETUP) {
			cache->io_ctl.inode = NULL;
			ret = btrfs_write_out_cache(trans, cache, path);
			if (ret == 0 && cache->io_ctl.inode) {
				should_put = 0;

				/*
				 * The cache_write_mutex is protecting the
				 * io_list, also refer to the definition of
				 * btrfs_transaction::io_bgs for more details
				 */
				list_add_tail(&cache->io_list, io);
			} else {
				/*
				 * If we failed to write the cache, the
				 * generation will be bad and life goes on
				 */
				ret = 0;
			}
		}
		if (!ret) {
			ret = update_block_group_item(trans, path, cache);
			/*
			 * Our block group might still be attached to the list
			 * of new block groups in the transaction handle of some
			 * other task (struct btrfs_trans_handle->new_bgs). This
			 * means its block group item isn't yet in the extent
			 * tree. If this happens ignore the error, as we will
			 * try again later in the critical section of the
			 * transaction commit.
			 */
			if (ret == -ENOENT) {
				ret = 0;
				spin_lock(&cur_trans->dirty_bgs_lock);
				if (list_empty(&cache->dirty_list)) {
					list_add_tail(&cache->dirty_list,
						      &cur_trans->dirty_bgs);
					btrfs_get_block_group(cache);
					drop_reserve = false;
				}
				spin_unlock(&cur_trans->dirty_bgs_lock);
			} else if (ret) {
				btrfs_abort_transaction(trans, ret);
			}
		}

		/* If it's not on the io list, we need to put the block group */
		if (should_put)
			btrfs_put_block_group(cache);
		if (drop_reserve)
			btrfs_dec_delayed_refs_rsv_bg_updates(fs_info);
		/*
		 * Avoid blocking other tasks for too long. It might even save
		 * us from writing caches for block groups that are going to be
		 * removed.
		 */
		mutex_unlock(&trans->transaction->cache_write_mutex);
		if (ret)
			goto out;
		mutex_lock(&trans->transaction->cache_write_mutex);
	}
	mutex_unlock(&trans->transaction->cache_write_mutex);

	/*
	 * Go through delayed refs for all the stuff we've just kicked off
	 * and then loop back (just once)
	 */
	if (!ret)
		ret = btrfs_run_delayed_refs(trans, 0);
	if (!ret && loops == 0) {
		loops++;
		spin_lock(&cur_trans->dirty_bgs_lock);
		list_splice_init(&cur_trans->dirty_bgs, &dirty);
		/*
		 * dirty_bgs_lock protects us from concurrent block group
		 * deletes too (not just cache_write_mutex).
		 */
		if (!list_empty(&dirty)) {
			spin_unlock(&cur_trans->dirty_bgs_lock);
			goto again;
		}
		spin_unlock(&cur_trans->dirty_bgs_lock);
	}
out:
	if (ret < 0) {
		spin_lock(&cur_trans->dirty_bgs_lock);
		list_splice_init(&dirty, &cur_trans->dirty_bgs);
		spin_unlock(&cur_trans->dirty_bgs_lock);
		btrfs_cleanup_dirty_bgs(cur_trans, fs_info);
	}

	btrfs_free_path(path);
	return ret;
}

int btrfs_write_dirty_block_groups(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *cache;
	struct btrfs_transaction *cur_trans = trans->transaction;
	int ret = 0;
	int should_put;
	struct btrfs_path *path;
	struct list_head *io = &cur_trans->io_bgs;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Even though we are in the critical section of the transaction commit,
	 * we can still have concurrent tasks adding elements to this
	 * transaction's list of dirty block groups. These tasks correspond to
	 * endio free space workers started when writeback finishes for a
	 * space cache, which run inode.c:btrfs_finish_ordered_io(), and can
	 * allocate new block groups as a result of COWing nodes of the root
	 * tree when updating the free space inode. The writeback for the space
	 * caches is triggered by an earlier call to
	 * btrfs_start_dirty_block_groups() and iterations of the following
	 * loop.
	 * Also we want to do the cache_save_setup first and then run the
	 * delayed refs to make sure we have the best chance at doing this all
	 * in one shot.
	 */
	spin_lock(&cur_trans->dirty_bgs_lock);
	while (!list_empty(&cur_trans->dirty_bgs)) {
		cache = list_first_entry(&cur_trans->dirty_bgs,
					 struct btrfs_block_group,
					 dirty_list);

		/*
		 * This can happen if cache_save_setup re-dirties a block group
		 * that is already under IO.  Just wait for it to finish and
		 * then do it all again
		 */
		if (!list_empty(&cache->io_list)) {
			spin_unlock(&cur_trans->dirty_bgs_lock);
			list_del_init(&cache->io_list);
			btrfs_wait_cache_io(trans, cache, path);
			btrfs_put_block_group(cache);
			spin_lock(&cur_trans->dirty_bgs_lock);
		}

		/*
		 * Don't remove from the dirty list until after we've waited on
		 * any pending IO
		 */
		list_del_init(&cache->dirty_list);
		spin_unlock(&cur_trans->dirty_bgs_lock);
		should_put = 1;

		cache_save_setup(cache, trans, path);

		if (!ret)
			ret = btrfs_run_delayed_refs(trans, U64_MAX);

		if (!ret && cache->disk_cache_state == BTRFS_DC_SETUP) {
			cache->io_ctl.inode = NULL;
			ret = btrfs_write_out_cache(trans, cache, path);
			if (ret == 0 && cache->io_ctl.inode) {
				should_put = 0;
				list_add_tail(&cache->io_list, io);
			} else {
				/*
				 * If we failed to write the cache, the
				 * generation will be bad and life goes on
				 */
				ret = 0;
			}
		}
		if (!ret) {
			ret = update_block_group_item(trans, path, cache);
			/*
			 * One of the free space endio workers might have
			 * created a new block group while updating a free space
			 * cache's inode (at inode.c:btrfs_finish_ordered_io())
			 * and hasn't released its transaction handle yet, in
			 * which case the new block group is still attached to
			 * its transaction handle and its creation has not
			 * finished yet (no block group item in the extent tree
			 * yet, etc). If this is the case, wait for all free
			 * space endio workers to finish and retry. This is a
			 * very rare case so no need for a more efficient and
			 * complex approach.
			 */
			if (ret == -ENOENT) {
				wait_event(cur_trans->writer_wait,
				   atomic_read(&cur_trans->num_writers) == 1);
				ret = update_block_group_item(trans, path, cache);
			}
			if (ret)
				btrfs_abort_transaction(trans, ret);
		}

		/* If its not on the io list, we need to put the block group */
		if (should_put)
			btrfs_put_block_group(cache);
		btrfs_dec_delayed_refs_rsv_bg_updates(fs_info);
		spin_lock(&cur_trans->dirty_bgs_lock);
	}
	spin_unlock(&cur_trans->dirty_bgs_lock);

	/*
	 * Refer to the definition of io_bgs member for details why it's safe
	 * to use it without any locking
	 */
	while (!list_empty(io)) {
		cache = list_first_entry(io, struct btrfs_block_group,
					 io_list);
		list_del_init(&cache->io_list);
		btrfs_wait_cache_io(trans, cache, path);
		btrfs_put_block_group(cache);
	}

	btrfs_free_path(path);
	return ret;
}

int btrfs_update_block_group(struct btrfs_trans_handle *trans,
			     u64 bytenr, u64 num_bytes, bool alloc)
{
	struct btrfs_fs_info *info = trans->fs_info;
	struct btrfs_space_info *space_info;
	struct btrfs_block_group *cache;
	u64 old_val;
	bool reclaim = false;
	bool bg_already_dirty = true;
	int factor;

	/* Block accounting for super block */
	spin_lock(&info->delalloc_root_lock);
	old_val = btrfs_super_bytes_used(info->super_copy);
	if (alloc)
		old_val += num_bytes;
	else
		old_val -= num_bytes;
	btrfs_set_super_bytes_used(info->super_copy, old_val);
	spin_unlock(&info->delalloc_root_lock);

	cache = btrfs_lookup_block_group(info, bytenr);
	if (!cache)
		return -ENOENT;

	/* An extent can not span multiple block groups. */
	ASSERT(bytenr + num_bytes <= cache->start + cache->length);

	space_info = cache->space_info;
	factor = btrfs_bg_type_to_factor(cache->flags);

	/*
	 * If this block group has free space cache written out, we need to make
	 * sure to load it if we are removing space.  This is because we need
	 * the unpinning stage to actually add the space back to the block group,
	 * otherwise we will leak space.
	 */
	if (!alloc && !btrfs_block_group_done(cache))
		btrfs_cache_block_group(cache, true);

	spin_lock(&space_info->lock);
	spin_lock(&cache->lock);

	if (btrfs_test_opt(info, SPACE_CACHE) &&
	    cache->disk_cache_state < BTRFS_DC_CLEAR)
		cache->disk_cache_state = BTRFS_DC_CLEAR;

	old_val = cache->used;
	if (alloc) {
		old_val += num_bytes;
		cache->used = old_val;
		cache->reserved -= num_bytes;
		space_info->bytes_reserved -= num_bytes;
		space_info->bytes_used += num_bytes;
		space_info->disk_used += num_bytes * factor;
		spin_unlock(&cache->lock);
		spin_unlock(&space_info->lock);
	} else {
		old_val -= num_bytes;
		cache->used = old_val;
		cache->pinned += num_bytes;
		btrfs_space_info_update_bytes_pinned(info, space_info, num_bytes);
		space_info->bytes_used -= num_bytes;
		space_info->disk_used -= num_bytes * factor;

		reclaim = should_reclaim_block_group(cache, num_bytes);

		spin_unlock(&cache->lock);
		spin_unlock(&space_info->lock);

		set_extent_bit(&trans->transaction->pinned_extents, bytenr,
			       bytenr + num_bytes - 1, EXTENT_DIRTY, NULL);
	}

	spin_lock(&trans->transaction->dirty_bgs_lock);
	if (list_empty(&cache->dirty_list)) {
		list_add_tail(&cache->dirty_list, &trans->transaction->dirty_bgs);
		bg_already_dirty = false;
		btrfs_get_block_group(cache);
	}
	spin_unlock(&trans->transaction->dirty_bgs_lock);

	/*
	 * No longer have used bytes in this block group, queue it for deletion.
	 * We do this after adding the block group to the dirty list to avoid
	 * races between cleaner kthread and space cache writeout.
	 */
	if (!alloc && old_val == 0) {
		if (!btrfs_test_opt(info, DISCARD_ASYNC))
			btrfs_mark_bg_unused(cache);
	} else if (!alloc && reclaim) {
		btrfs_mark_bg_to_reclaim(cache);
	}

	btrfs_put_block_group(cache);

	/* Modified block groups are accounted for in the delayed_refs_rsv. */
	if (!bg_already_dirty)
		btrfs_inc_delayed_refs_rsv_bg_updates(info);

	return 0;
}

/*
 * Update the block_group and space info counters.
 *
 * @cache:	The cache we are manipulating
 * @ram_bytes:  The number of bytes of file content, and will be same to
 *              @num_bytes except for the compress path.
 * @num_bytes:	The number of bytes in question
 * @delalloc:   The blocks are allocated for the delalloc write
 *
 * This is called by the allocator when it reserves space. If this is a
 * reservation and the block group has become read only we cannot make the
 * reservation and return -EAGAIN, otherwise this function always succeeds.
 */
int btrfs_add_reserved_bytes(struct btrfs_block_group *cache,
			     u64 ram_bytes, u64 num_bytes, int delalloc,
			     bool force_wrong_size_class)
{
	struct btrfs_space_info *space_info = cache->space_info;
	enum btrfs_block_group_size_class size_class;
	int ret = 0;

	spin_lock(&space_info->lock);
	spin_lock(&cache->lock);
	if (cache->ro) {
		ret = -EAGAIN;
		goto out;
	}

	if (btrfs_block_group_should_use_size_class(cache)) {
		size_class = btrfs_calc_block_group_size_class(num_bytes);
		ret = btrfs_use_block_group_size_class(cache, size_class, force_wrong_size_class);
		if (ret)
			goto out;
	}
	cache->reserved += num_bytes;
	space_info->bytes_reserved += num_bytes;
	trace_btrfs_space_reservation(cache->fs_info, "space_info",
				      space_info->flags, num_bytes, 1);
	btrfs_space_info_update_bytes_may_use(cache->fs_info,
					      space_info, -ram_bytes);
	if (delalloc)
		cache->delalloc_bytes += num_bytes;

	/*
	 * Compression can use less space than we reserved, so wake tickets if
	 * that happens.
	 */
	if (num_bytes < ram_bytes)
		btrfs_try_granting_tickets(cache->fs_info, space_info);
out:
	spin_unlock(&cache->lock);
	spin_unlock(&space_info->lock);
	return ret;
}

/*
 * Update the block_group and space info counters.
 *
 * @cache:      The cache we are manipulating
 * @num_bytes:  The number of bytes in question
 * @delalloc:   The blocks are allocated for the delalloc write
 *
 * This is called by somebody who is freeing space that was never actually used
 * on disk.  For example if you reserve some space for a new leaf in transaction
 * A and before transaction A commits you free that leaf, you call this with
 * reserve set to 0 in order to clear the reservation.
 */
void btrfs_free_reserved_bytes(struct btrfs_block_group *cache,
			       u64 num_bytes, int delalloc)
{
	struct btrfs_space_info *space_info = cache->space_info;

	spin_lock(&space_info->lock);
	spin_lock(&cache->lock);
	if (cache->ro)
		space_info->bytes_readonly += num_bytes;
	cache->reserved -= num_bytes;
	space_info->bytes_reserved -= num_bytes;
	space_info->max_extent_size = 0;

	if (delalloc)
		cache->delalloc_bytes -= num_bytes;
	spin_unlock(&cache->lock);

	btrfs_try_granting_tickets(cache->fs_info, space_info);
	spin_unlock(&space_info->lock);
}

static void force_metadata_allocation(struct btrfs_fs_info *info)
{
	struct list_head *head = &info->space_info;
	struct btrfs_space_info *found;

	list_for_each_entry(found, head, list) {
		if (found->flags & BTRFS_BLOCK_GROUP_METADATA)
			found->force_alloc = CHUNK_ALLOC_FORCE;
	}
}

static int should_alloc_chunk(struct btrfs_fs_info *fs_info,
			      struct btrfs_space_info *sinfo, int force)
{
	u64 bytes_used = btrfs_space_info_used(sinfo, false);
	u64 thresh;

	if (force == CHUNK_ALLOC_FORCE)
		return 1;

	/*
	 * in limited mode, we want to have some free space up to
	 * about 1% of the FS size.
	 */
	if (force == CHUNK_ALLOC_LIMITED) {
		thresh = btrfs_super_total_bytes(fs_info->super_copy);
		thresh = max_t(u64, SZ_64M, mult_perc(thresh, 1));

		if (sinfo->total_bytes - bytes_used < thresh)
			return 1;
	}

	if (bytes_used + SZ_2M < mult_perc(sinfo->total_bytes, 80))
		return 0;
	return 1;
}

int btrfs_force_chunk_alloc(struct btrfs_trans_handle *trans, u64 type)
{
	u64 alloc_flags = btrfs_get_alloc_profile(trans->fs_info, type);

	return btrfs_chunk_alloc(trans, alloc_flags, CHUNK_ALLOC_FORCE);
}

static struct btrfs_block_group *do_chunk_alloc(struct btrfs_trans_handle *trans, u64 flags)
{
	struct btrfs_block_group *bg;
	int ret;

	/*
	 * Check if we have enough space in the system space info because we
	 * will need to update device items in the chunk btree and insert a new
	 * chunk item in the chunk btree as well. This will allocate a new
	 * system block group if needed.
	 */
	check_system_chunk(trans, flags);

	bg = btrfs_create_chunk(trans, flags);
	if (IS_ERR(bg)) {
		ret = PTR_ERR(bg);
		goto out;
	}

	ret = btrfs_chunk_alloc_add_chunk_item(trans, bg);
	/*
	 * Normally we are not expected to fail with -ENOSPC here, since we have
	 * previously reserved space in the system space_info and allocated one
	 * new system chunk if necessary. However there are three exceptions:
	 *
	 * 1) We may have enough free space in the system space_info but all the
	 *    existing system block groups have a profile which can not be used
	 *    for extent allocation.
	 *
	 *    This happens when mounting in degraded mode. For example we have a
	 *    RAID1 filesystem with 2 devices, lose one device and mount the fs
	 *    using the other device in degraded mode. If we then allocate a chunk,
	 *    we may have enough free space in the existing system space_info, but
	 *    none of the block groups can be used for extent allocation since they
	 *    have a RAID1 profile, and because we are in degraded mode with a
	 *    single device, we are forced to allocate a new system chunk with a
	 *    SINGLE profile. Making check_system_chunk() iterate over all system
	 *    block groups and check if they have a usable profile and enough space
	 *    can be slow on very large filesystems, so we tolerate the -ENOSPC and
	 *    try again after forcing allocation of a new system chunk. Like this
	 *    we avoid paying the cost of that search in normal circumstances, when
	 *    we were not mounted in degraded mode;
	 *
	 * 2) We had enough free space info the system space_info, and one suitable
	 *    block group to allocate from when we called check_system_chunk()
	 *    above. However right after we called it, the only system block group
	 *    with enough free space got turned into RO mode by a running scrub,
	 *    and in this case we have to allocate a new one and retry. We only
	 *    need do this allocate and retry once, since we have a transaction
	 *    handle and scrub uses the commit root to search for block groups;
	 *
	 * 3) We had one system block group with enough free space when we called
	 *    check_system_chunk(), but after that, right before we tried to
	 *    allocate the last extent buffer we needed, a discard operation came
	 *    in and it temporarily removed the last free space entry from the
	 *    block group (discard removes a free space entry, discards it, and
	 *    then adds back the entry to the block group cache).
	 */
	if (ret == -ENOSPC) {
		const u64 sys_flags = btrfs_system_alloc_profile(trans->fs_info);
		struct btrfs_block_group *sys_bg;

		sys_bg = btrfs_create_chunk(trans, sys_flags);
		if (IS_ERR(sys_bg)) {
			ret = PTR_ERR(sys_bg);
			btrfs_abort_transaction(trans, ret);
			goto out;
		}

		ret = btrfs_chunk_alloc_add_chunk_item(trans, sys_bg);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		}

		ret = btrfs_chunk_alloc_add_chunk_item(trans, bg);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		}
	} else if (ret) {
		btrfs_abort_transaction(trans, ret);
		goto out;
	}
out:
	btrfs_trans_release_chunk_metadata(trans);

	if (ret)
		return ERR_PTR(ret);

	btrfs_get_block_group(bg);
	return bg;
}

/*
 * Chunk allocation is done in 2 phases:
 *
 * 1) Phase 1 - through btrfs_chunk_alloc() we allocate device extents for
 *    the chunk, the chunk mapping, create its block group and add the items
 *    that belong in the chunk btree to it - more specifically, we need to
 *    update device items in the chunk btree and add a new chunk item to it.
 *
 * 2) Phase 2 - through btrfs_create_pending_block_groups(), we add the block
 *    group item to the extent btree and the device extent items to the devices
 *    btree.
 *
 * This is done to prevent deadlocks. For example when COWing a node from the
 * extent btree we are holding a write lock on the node's parent and if we
 * trigger chunk allocation and attempted to insert the new block group item
 * in the extent btree right way, we could deadlock because the path for the
 * insertion can include that parent node. At first glance it seems impossible
 * to trigger chunk allocation after starting a transaction since tasks should
 * reserve enough transaction units (metadata space), however while that is true
 * most of the time, chunk allocation may still be triggered for several reasons:
 *
 * 1) When reserving metadata, we check if there is enough free space in the
 *    metadata space_info and therefore don't trigger allocation of a new chunk.
 *    However later when the task actually tries to COW an extent buffer from
 *    the extent btree or from the device btree for example, it is forced to
 *    allocate a new block group (chunk) because the only one that had enough
 *    free space was just turned to RO mode by a running scrub for example (or
 *    device replace, block group reclaim thread, etc), so we can not use it
 *    for allocating an extent and end up being forced to allocate a new one;
 *
 * 2) Because we only check that the metadata space_info has enough free bytes,
 *    we end up not allocating a new metadata chunk in that case. However if
 *    the filesystem was mounted in degraded mode, none of the existing block
 *    groups might be suitable for extent allocation due to their incompatible
 *    profile (for e.g. mounting a 2 devices filesystem, where all block groups
 *    use a RAID1 profile, in degraded mode using a single device). In this case
 *    when the task attempts to COW some extent buffer of the extent btree for
 *    example, it will trigger allocation of a new metadata block group with a
 *    suitable profile (SINGLE profile in the example of the degraded mount of
 *    the RAID1 filesystem);
 *
 * 3) The task has reserved enough transaction units / metadata space, but when
 *    it attempts to COW an extent buffer from the extent or device btree for
 *    example, it does not find any free extent in any metadata block group,
 *    therefore forced to try to allocate a new metadata block group.
 *    This is because some other task allocated all available extents in the
 *    meanwhile - this typically happens with tasks that don't reserve space
 *    properly, either intentionally or as a bug. One example where this is
 *    done intentionally is fsync, as it does not reserve any transaction units
 *    and ends up allocating a variable number of metadata extents for log
 *    tree extent buffers;
 *
 * 4) The task has reserved enough transaction units / metadata space, but right
 *    before it tries to allocate the last extent buffer it needs, a discard
 *    operation comes in and, temporarily, removes the last free space entry from
 *    the only metadata block group that had free space (discard starts by
 *    removing a free space entry from a block group, then does the discard
 *    operation and, once it's done, it adds back the free space entry to the
 *    block group).
 *
 * We also need this 2 phases setup when adding a device to a filesystem with
 * a seed device - we must create new metadata and system chunks without adding
 * any of the block group items to the chunk, extent and device btrees. If we
 * did not do it this way, we would get ENOSPC when attempting to update those
 * btrees, since all the chunks from the seed device are read-only.
 *
 * Phase 1 does the updates and insertions to the chunk btree because if we had
 * it done in phase 2 and have a thundering herd of tasks allocating chunks in
 * parallel, we risk having too many system chunks allocated by many tasks if
 * many tasks reach phase 1 without the previous ones completing phase 2. In the
 * extreme case this leads to exhaustion of the system chunk array in the
 * superblock. This is easier to trigger if using a btree node/leaf size of 64K
 * and with RAID filesystems (so we have more device items in the chunk btree).
 * This has happened before and commit eafa4fd0ad0607 ("btrfs: fix exhaustion of
 * the system chunk array due to concurrent allocations") provides more details.
 *
 * Allocation of system chunks does not happen through this function. A task that
 * needs to update the chunk btree (the only btree that uses system chunks), must
 * preallocate chunk space by calling either check_system_chunk() or
 * btrfs_reserve_chunk_metadata() - the former is used when allocating a data or
 * metadata chunk or when removing a chunk, while the later is used before doing
 * a modification to the chunk btree - use cases for the later are adding,
 * removing and resizing a device as well as relocation of a system chunk.
 * See the comment below for more details.
 *
 * The reservation of system space, done through check_system_chunk(), as well
 * as all the updates and insertions into the chunk btree must be done while
 * holding fs_info->chunk_mutex. This is important to guarantee that while COWing
 * an extent buffer from the chunks btree we never trigger allocation of a new
 * system chunk, which would result in a deadlock (trying to lock twice an
 * extent buffer of the chunk btree, first time before triggering the chunk
 * allocation and the second time during chunk allocation while attempting to
 * update the chunks btree). The system chunk array is also updated while holding
 * that mutex. The same logic applies to removing chunks - we must reserve system
 * space, update the chunk btree and the system chunk array in the superblock
 * while holding fs_info->chunk_mutex.
 *
 * This function, btrfs_chunk_alloc(), belongs to phase 1.
 *
 * If @force is CHUNK_ALLOC_FORCE:
 *    - return 1 if it successfully allocates a chunk,
 *    - return errors including -ENOSPC otherwise.
 * If @force is NOT CHUNK_ALLOC_FORCE:
 *    - return 0 if it doesn't need to allocate a new chunk,
 *    - return 1 if it successfully allocates a chunk,
 *    - return errors including -ENOSPC otherwise.
 */
int btrfs_chunk_alloc(struct btrfs_trans_handle *trans, u64 flags,
		      enum btrfs_chunk_alloc_enum force)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_space_info *space_info;
	struct btrfs_block_group *ret_bg;
	bool wait_for_alloc = false;
	bool should_alloc = false;
	bool from_extent_allocation = false;
	int ret = 0;

	if (force == CHUNK_ALLOC_FORCE_FOR_EXTENT) {
		from_extent_allocation = true;
		force = CHUNK_ALLOC_FORCE;
	}

	/* Don't re-enter if we're already allocating a chunk */
	if (trans->allocating_chunk)
		return -ENOSPC;
	/*
	 * Allocation of system chunks can not happen through this path, as we
	 * could end up in a deadlock if we are allocating a data or metadata
	 * chunk and there is another task modifying the chunk btree.
	 *
	 * This is because while we are holding the chunk mutex, we will attempt
	 * to add the new chunk item to the chunk btree or update an existing
	 * device item in the chunk btree, while the other task that is modifying
	 * the chunk btree is attempting to COW an extent buffer while holding a
	 * lock on it and on its parent - if the COW operation triggers a system
	 * chunk allocation, then we can deadlock because we are holding the
	 * chunk mutex and we may need to access that extent buffer or its parent
	 * in order to add the chunk item or update a device item.
	 *
	 * Tasks that want to modify the chunk tree should reserve system space
	 * before updating the chunk btree, by calling either
	 * btrfs_reserve_chunk_metadata() or check_system_chunk().
	 * It's possible that after a task reserves the space, it still ends up
	 * here - this happens in the cases described above at do_chunk_alloc().
	 * The task will have to either retry or fail.
	 */
	if (flags & BTRFS_BLOCK_GROUP_SYSTEM)
		return -ENOSPC;

	space_info = btrfs_find_space_info(fs_info, flags);
	ASSERT(space_info);

	do {
		spin_lock(&space_info->lock);
		if (force < space_info->force_alloc)
			force = space_info->force_alloc;
		should_alloc = should_alloc_chunk(fs_info, space_info, force);
		if (space_info->full) {
			/* No more free physical space */
			if (should_alloc)
				ret = -ENOSPC;
			else
				ret = 0;
			spin_unlock(&space_info->lock);
			return ret;
		} else if (!should_alloc) {
			spin_unlock(&space_info->lock);
			return 0;
		} else if (space_info->chunk_alloc) {
			/*
			 * Someone is already allocating, so we need to block
			 * until this someone is finished and then loop to
			 * recheck if we should continue with our allocation
			 * attempt.
			 */
			wait_for_alloc = true;
			force = CHUNK_ALLOC_NO_FORCE;
			spin_unlock(&space_info->lock);
			mutex_lock(&fs_info->chunk_mutex);
			mutex_unlock(&fs_info->chunk_mutex);
		} else {
			/* Proceed with allocation */
			space_info->chunk_alloc = 1;
			wait_for_alloc = false;
			spin_unlock(&space_info->lock);
		}

		cond_resched();
	} while (wait_for_alloc);

	mutex_lock(&fs_info->chunk_mutex);
	trans->allocating_chunk = true;

	/*
	 * If we have mixed data/metadata chunks we want to make sure we keep
	 * allocating mixed chunks instead of individual chunks.
	 */
	if (btrfs_mixed_space_info(space_info))
		flags |= (BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_METADATA);

	/*
	 * if we're doing a data chunk, go ahead and make sure that
	 * we keep a reasonable number of metadata chunks allocated in the
	 * FS as well.
	 */
	if (flags & BTRFS_BLOCK_GROUP_DATA && fs_info->metadata_ratio) {
		fs_info->data_chunk_allocations++;
		if (!(fs_info->data_chunk_allocations %
		      fs_info->metadata_ratio))
			force_metadata_allocation(fs_info);
	}

	ret_bg = do_chunk_alloc(trans, flags);
	trans->allocating_chunk = false;

	if (IS_ERR(ret_bg)) {
		ret = PTR_ERR(ret_bg);
	} else if (from_extent_allocation && (flags & BTRFS_BLOCK_GROUP_DATA)) {
		/*
		 * New block group is likely to be used soon. Try to activate
		 * it now. Failure is OK for now.
		 */
		btrfs_zone_activate(ret_bg);
	}

	if (!ret)
		btrfs_put_block_group(ret_bg);

	spin_lock(&space_info->lock);
	if (ret < 0) {
		if (ret == -ENOSPC)
			space_info->full = 1;
		else
			goto out;
	} else {
		ret = 1;
		space_info->max_extent_size = 0;
	}

	space_info->force_alloc = CHUNK_ALLOC_NO_FORCE;
out:
	space_info->chunk_alloc = 0;
	spin_unlock(&space_info->lock);
	mutex_unlock(&fs_info->chunk_mutex);

	return ret;
}

static u64 get_profile_num_devs(struct btrfs_fs_info *fs_info, u64 type)
{
	u64 num_dev;

	num_dev = btrfs_raid_array[btrfs_bg_flags_to_raid_index(type)].devs_max;
	if (!num_dev)
		num_dev = fs_info->fs_devices->rw_devices;

	return num_dev;
}

static void reserve_chunk_space(struct btrfs_trans_handle *trans,
				u64 bytes,
				u64 type)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_space_info *info;
	u64 left;
	int ret = 0;

	/*
	 * Needed because we can end up allocating a system chunk and for an
	 * atomic and race free space reservation in the chunk block reserve.
	 */
	lockdep_assert_held(&fs_info->chunk_mutex);

	info = btrfs_find_space_info(fs_info, BTRFS_BLOCK_GROUP_SYSTEM);
	spin_lock(&info->lock);
	left = info->total_bytes - btrfs_space_info_used(info, true);
	spin_unlock(&info->lock);

	if (left < bytes && btrfs_test_opt(fs_info, ENOSPC_DEBUG)) {
		btrfs_info(fs_info, "left=%llu, need=%llu, flags=%llu",
			   left, bytes, type);
		btrfs_dump_space_info(fs_info, info, 0, 0);
	}

	if (left < bytes) {
		u64 flags = btrfs_system_alloc_profile(fs_info);
		struct btrfs_block_group *bg;

		/*
		 * Ignore failure to create system chunk. We might end up not
		 * needing it, as we might not need to COW all nodes/leafs from
		 * the paths we visit in the chunk tree (they were already COWed
		 * or created in the current transaction for example).
		 */
		bg = btrfs_create_chunk(trans, flags);
		if (IS_ERR(bg)) {
			ret = PTR_ERR(bg);
		} else {
			/*
			 * We have a new chunk. We also need to activate it for
			 * zoned filesystem.
			 */
			ret = btrfs_zoned_activate_one_bg(fs_info, info, true);
			if (ret < 0)
				return;

			/*
			 * If we fail to add the chunk item here, we end up
			 * trying again at phase 2 of chunk allocation, at
			 * btrfs_create_pending_block_groups(). So ignore
			 * any error here. An ENOSPC here could happen, due to
			 * the cases described at do_chunk_alloc() - the system
			 * block group we just created was just turned into RO
			 * mode by a scrub for example, or a running discard
			 * temporarily removed its free space entries, etc.
			 */
			btrfs_chunk_alloc_add_chunk_item(trans, bg);
		}
	}

	if (!ret) {
		ret = btrfs_block_rsv_add(fs_info,
					  &fs_info->chunk_block_rsv,
					  bytes, BTRFS_RESERVE_NO_FLUSH);
		if (!ret)
			trans->chunk_bytes_reserved += bytes;
	}
}

/*
 * Reserve space in the system space for allocating or removing a chunk.
 * The caller must be holding fs_info->chunk_mutex.
 */
void check_system_chunk(struct btrfs_trans_handle *trans, u64 type)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	const u64 num_devs = get_profile_num_devs(fs_info, type);
	u64 bytes;

	/* num_devs device items to update and 1 chunk item to add or remove. */
	bytes = btrfs_calc_metadata_size(fs_info, num_devs) +
		btrfs_calc_insert_metadata_size(fs_info, 1);

	reserve_chunk_space(trans, bytes, type);
}

/*
 * Reserve space in the system space, if needed, for doing a modification to the
 * chunk btree.
 *
 * @trans:		A transaction handle.
 * @is_item_insertion:	Indicate if the modification is for inserting a new item
 *			in the chunk btree or if it's for the deletion or update
 *			of an existing item.
 *
 * This is used in a context where we need to update the chunk btree outside
 * block group allocation and removal, to avoid a deadlock with a concurrent
 * task that is allocating a metadata or data block group and therefore needs to
 * update the chunk btree while holding the chunk mutex. After the update to the
 * chunk btree is done, btrfs_trans_release_chunk_metadata() should be called.
 *
 */
void btrfs_reserve_chunk_metadata(struct btrfs_trans_handle *trans,
				  bool is_item_insertion)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	u64 bytes;

	if (is_item_insertion)
		bytes = btrfs_calc_insert_metadata_size(fs_info, 1);
	else
		bytes = btrfs_calc_metadata_size(fs_info, 1);

	mutex_lock(&fs_info->chunk_mutex);
	reserve_chunk_space(trans, bytes, BTRFS_BLOCK_GROUP_SYSTEM);
	mutex_unlock(&fs_info->chunk_mutex);
}

void btrfs_put_block_group_cache(struct btrfs_fs_info *info)
{
	struct btrfs_block_group *block_group;

	block_group = btrfs_lookup_first_block_group(info, 0);
	while (block_group) {
		btrfs_wait_block_group_cache_done(block_group);
		spin_lock(&block_group->lock);
		if (test_and_clear_bit(BLOCK_GROUP_FLAG_IREF,
				       &block_group->runtime_flags)) {
			struct inode *inode = block_group->inode;

			block_group->inode = NULL;
			spin_unlock(&block_group->lock);

			ASSERT(block_group->io_ctl.inode == NULL);
			iput(inode);
		} else {
			spin_unlock(&block_group->lock);
		}
		block_group = btrfs_next_block_group(block_group);
	}
}

/*
 * Must be called only after stopping all workers, since we could have block
 * group caching kthreads running, and therefore they could race with us if we
 * freed the block groups before stopping them.
 */
int btrfs_free_block_groups(struct btrfs_fs_info *info)
{
	struct btrfs_block_group *block_group;
	struct btrfs_space_info *space_info;
	struct btrfs_caching_control *caching_ctl;
	struct rb_node *n;

	if (btrfs_is_zoned(info)) {
		if (info->active_meta_bg) {
			btrfs_put_block_group(info->active_meta_bg);
			info->active_meta_bg = NULL;
		}
		if (info->active_system_bg) {
			btrfs_put_block_group(info->active_system_bg);
			info->active_system_bg = NULL;
		}
	}

	write_lock(&info->block_group_cache_lock);
	while (!list_empty(&info->caching_block_groups)) {
		caching_ctl = list_entry(info->caching_block_groups.next,
					 struct btrfs_caching_control, list);
		list_del(&caching_ctl->list);
		btrfs_put_caching_control(caching_ctl);
	}
	write_unlock(&info->block_group_cache_lock);

	spin_lock(&info->unused_bgs_lock);
	while (!list_empty(&info->unused_bgs)) {
		block_group = list_first_entry(&info->unused_bgs,
					       struct btrfs_block_group,
					       bg_list);
		list_del_init(&block_group->bg_list);
		btrfs_put_block_group(block_group);
	}

	while (!list_empty(&info->reclaim_bgs)) {
		block_group = list_first_entry(&info->reclaim_bgs,
					       struct btrfs_block_group,
					       bg_list);
		list_del_init(&block_group->bg_list);
		btrfs_put_block_group(block_group);
	}
	spin_unlock(&info->unused_bgs_lock);

	spin_lock(&info->zone_active_bgs_lock);
	while (!list_empty(&info->zone_active_bgs)) {
		block_group = list_first_entry(&info->zone_active_bgs,
					       struct btrfs_block_group,
					       active_bg_list);
		list_del_init(&block_group->active_bg_list);
		btrfs_put_block_group(block_group);
	}
	spin_unlock(&info->zone_active_bgs_lock);

	write_lock(&info->block_group_cache_lock);
	while ((n = rb_last(&info->block_group_cache_tree.rb_root)) != NULL) {
		block_group = rb_entry(n, struct btrfs_block_group,
				       cache_node);
		rb_erase_cached(&block_group->cache_node,
				&info->block_group_cache_tree);
		RB_CLEAR_NODE(&block_group->cache_node);
		write_unlock(&info->block_group_cache_lock);

		down_write(&block_group->space_info->groups_sem);
		list_del(&block_group->list);
		up_write(&block_group->space_info->groups_sem);

		/*
		 * We haven't cached this block group, which means we could
		 * possibly have excluded extents on this block group.
		 */
		if (block_group->cached == BTRFS_CACHE_NO ||
		    block_group->cached == BTRFS_CACHE_ERROR)
			btrfs_free_excluded_extents(block_group);

		btrfs_remove_free_space_cache(block_group);
		ASSERT(block_group->cached != BTRFS_CACHE_STARTED);
		ASSERT(list_empty(&block_group->dirty_list));
		ASSERT(list_empty(&block_group->io_list));
		ASSERT(list_empty(&block_group->bg_list));
		ASSERT(refcount_read(&block_group->refs) == 1);
		ASSERT(block_group->swap_extents == 0);
		btrfs_put_block_group(block_group);

		write_lock(&info->block_group_cache_lock);
	}
	write_unlock(&info->block_group_cache_lock);

	btrfs_release_global_block_rsv(info);

	while (!list_empty(&info->space_info)) {
		space_info = list_entry(info->space_info.next,
					struct btrfs_space_info,
					list);

		/*
		 * Do not hide this behind enospc_debug, this is actually
		 * important and indicates a real bug if this happens.
		 */
		if (WARN_ON(space_info->bytes_pinned > 0 ||
			    space_info->bytes_may_use > 0))
			btrfs_dump_space_info(info, space_info, 0, 0);

		/*
		 * If there was a failure to cleanup a log tree, very likely due
		 * to an IO failure on a writeback attempt of one or more of its
		 * extent buffers, we could not do proper (and cheap) unaccounting
		 * of their reserved space, so don't warn on bytes_reserved > 0 in
		 * that case.
		 */
		if (!(space_info->flags & BTRFS_BLOCK_GROUP_METADATA) ||
		    !BTRFS_FS_LOG_CLEANUP_ERROR(info)) {
			if (WARN_ON(space_info->bytes_reserved > 0))
				btrfs_dump_space_info(info, space_info, 0, 0);
		}

		WARN_ON(space_info->reclaim_size > 0);
		list_del(&space_info->list);
		btrfs_sysfs_remove_space_info(space_info);
	}
	return 0;
}

void btrfs_freeze_block_group(struct btrfs_block_group *cache)
{
	atomic_inc(&cache->frozen);
}

void btrfs_unfreeze_block_group(struct btrfs_block_group *block_group)
{
	struct btrfs_fs_info *fs_info = block_group->fs_info;
	bool cleanup;

	spin_lock(&block_group->lock);
	cleanup = (atomic_dec_and_test(&block_group->frozen) &&
		   test_bit(BLOCK_GROUP_FLAG_REMOVED, &block_group->runtime_flags));
	spin_unlock(&block_group->lock);

	if (cleanup) {
		struct btrfs_chunk_map *map;

		map = btrfs_find_chunk_map(fs_info, block_group->start, 1);
		/* Logic error, can't happen. */
		ASSERT(map);

		btrfs_remove_chunk_map(fs_info, map);

		/* Once for our lookup reference. */
		btrfs_free_chunk_map(map);

		/*
		 * We may have left one free space entry and other possible
		 * tasks trimming this block group have left 1 entry each one.
		 * Free them if any.
		 */
		btrfs_remove_free_space_cache(block_group);
	}
}

bool btrfs_inc_block_group_swap_extents(struct btrfs_block_group *bg)
{
	bool ret = true;

	spin_lock(&bg->lock);
	if (bg->ro)
		ret = false;
	else
		bg->swap_extents++;
	spin_unlock(&bg->lock);

	return ret;
}

void btrfs_dec_block_group_swap_extents(struct btrfs_block_group *bg, int amount)
{
	spin_lock(&bg->lock);
	ASSERT(!bg->ro);
	ASSERT(bg->swap_extents >= amount);
	bg->swap_extents -= amount;
	spin_unlock(&bg->lock);
}

enum btrfs_block_group_size_class btrfs_calc_block_group_size_class(u64 size)
{
	if (size <= SZ_128K)
		return BTRFS_BG_SZ_SMALL;
	if (size <= SZ_8M)
		return BTRFS_BG_SZ_MEDIUM;
	return BTRFS_BG_SZ_LARGE;
}

/*
 * Handle a block group allocating an extent in a size class
 *
 * @bg:				The block group we allocated in.
 * @size_class:			The size class of the allocation.
 * @force_wrong_size_class:	Whether we are desperate enough to allow
 *				mismatched size classes.
 *
 * Returns: 0 if the size class was valid for this block_group, -EAGAIN in the
 * case of a race that leads to the wrong size class without
 * force_wrong_size_class set.
 *
 * find_free_extent will skip block groups with a mismatched size class until
 * it really needs to avoid ENOSPC. In that case it will set
 * force_wrong_size_class. However, if a block group is newly allocated and
 * doesn't yet have a size class, then it is possible for two allocations of
 * different sizes to race and both try to use it. The loser is caught here and
 * has to retry.
 */
int btrfs_use_block_group_size_class(struct btrfs_block_group *bg,
				     enum btrfs_block_group_size_class size_class,
				     bool force_wrong_size_class)
{
	ASSERT(size_class != BTRFS_BG_SZ_NONE);

	/* The new allocation is in the right size class, do nothing */
	if (bg->size_class == size_class)
		return 0;
	/*
	 * The new allocation is in a mismatched size class.
	 * This means one of two things:
	 *
	 * 1. Two tasks in find_free_extent for different size_classes raced
	 *    and hit the same empty block_group. Make the loser try again.
	 * 2. A call to find_free_extent got desperate enough to set
	 *    'force_wrong_slab'. Don't change the size_class, but allow the
	 *    allocation.
	 */
	if (bg->size_class != BTRFS_BG_SZ_NONE) {
		if (force_wrong_size_class)
			return 0;
		return -EAGAIN;
	}
	/*
	 * The happy new block group case: the new allocation is the first
	 * one in the block_group so we set size_class.
	 */
	bg->size_class = size_class;

	return 0;
}

bool btrfs_block_group_should_use_size_class(struct btrfs_block_group *bg)
{
	if (btrfs_is_zoned(bg->fs_info))
		return false;
	if (!btrfs_is_block_group_data_only(bg))
		return false;
	return true;
}
