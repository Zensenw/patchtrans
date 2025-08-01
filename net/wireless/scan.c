// SPDX-License-Identifier: GPL-2.0
/*
 * cfg80211 scan result handling
 *
 * Copyright 2008 Johannes Berg <johannes@sipsolutions.net>
 * Copyright 2013-2014  Intel Mobile Communications GmbH
 * Copyright 2016	Intel Deutschland GmbH
 * Copyright (C) 2018-2023 Intel Corporation
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/wireless.h>
#include <linux/nl80211.h>
#include <linux/etherdevice.h>
#include <linux/crc32.h>
#include <linux/bitfield.h>
#include <net/arp.h>
#include <net/cfg80211.h>
#include <net/cfg80211-wext.h>
#include <net/iw_handler.h>
#include <kunit/visibility.h>
#include "core.h"
#include "nl80211.h"
#include "wext-compat.h"
#include "rdev-ops.h"

/**
 * DOC: BSS tree/list structure
 *
 * At the top level, the BSS list is kept in both a list in each
 * registered device (@bss_list) as well as an RB-tree for faster
 * lookup. In the RB-tree, entries can be looked up using their
 * channel, MESHID, MESHCONF (for MBSSes) or channel, BSSID, SSID
 * for other BSSes.
 *
 * Due to the possibility of hidden SSIDs, there's a second level
 * structure, the "hidden_list" and "hidden_beacon_bss" pointer.
 * The hidden_list connects all BSSes belonging to a single AP
 * that has a hidden SSID, and connects beacon and probe response
 * entries. For a probe response entry for a hidden SSID, the
 * hidden_beacon_bss pointer points to the BSS struct holding the
 * beacon's information.
 *
 * Reference counting is done for all these references except for
 * the hidden_list, so that a beacon BSS struct that is otherwise
 * not referenced has one reference for being on the bss_list and
 * one for each probe response entry that points to it using the
 * hidden_beacon_bss pointer. When a BSS struct that has such a
 * pointer is get/put, the refcount update is also propagated to
 * the referenced struct, this ensure that it cannot get removed
 * while somebody is using the probe response version.
 *
 * Note that the hidden_beacon_bss pointer never changes, due to
 * the reference counting. Therefore, no locking is needed for
 * it.
 *
 * Also note that the hidden_beacon_bss pointer is only relevant
 * if the driver uses something other than the IEs, e.g. private
 * data stored in the BSS struct, since the beacon IEs are
 * also linked into the probe response struct.
 */

/*
 * Limit the number of BSS entries stored in mac80211. Each one is
 * a bit over 4k at most, so this limits to roughly 4-5M of memory.
 * If somebody wants to really attack this though, they'd likely
 * use small beacons, and only one type of frame, limiting each of
 * the entries to a much smaller size (in order to generate more
 * entries in total, so overhead is bigger.)
 */
static int bss_entries_limit = 1000;
module_param(bss_entries_limit, int, 0644);
MODULE_PARM_DESC(bss_entries_limit,
                 "limit to number of scan BSS entries (per wiphy, default 1000)");

#define IEEE80211_SCAN_RESULT_EXPIRE	(30 * HZ)

/**
 * struct cfg80211_colocated_ap - colocated AP information
 *
 * @list: linked list to all colocated aPS
 * @bssid: BSSID of the reported AP
 * @ssid: SSID of the reported AP
 * @ssid_len: length of the ssid
 * @center_freq: frequency the reported AP is on
 * @unsolicited_probe: the reported AP is part of an ESS, where all the APs
 *	that operate in the same channel as the reported AP and that might be
 *	detected by a STA receiving this frame, are transmitting unsolicited
 *	Probe Response frames every 20 TUs
 * @oct_recommended: OCT is recommended to exchange MMPDUs with the reported AP
 * @same_ssid: the reported AP has the same SSID as the reporting AP
 * @multi_bss: the reported AP is part of a multiple BSSID set
 * @transmitted_bssid: the reported AP is the transmitting BSSID
 * @colocated_ess: all the APs that share the same ESS as the reported AP are
 *	colocated and can be discovered via legacy bands.
 * @short_ssid_valid: short_ssid is valid and can be used
 * @short_ssid: the short SSID for this SSID
 * @psd_20: The 20MHz PSD EIRP of the primary 20MHz channel for the reported AP
 */
struct cfg80211_colocated_ap {
	struct list_head list;
	u8 bssid[ETH_ALEN];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	size_t ssid_len;
	u32 short_ssid;
	u32 center_freq;
	u8 unsolicited_probe:1,
	   oct_recommended:1,
	   same_ssid:1,
	   multi_bss:1,
	   transmitted_bssid:1,
	   colocated_ess:1,
	   short_ssid_valid:1;
	s8 psd_20;
};

static void bss_free(struct cfg80211_internal_bss *bss)
{
	struct cfg80211_bss_ies *ies;

	if (WARN_ON(atomic_read(&bss->hold)))
		return;

	ies = (void *)rcu_access_pointer(bss->pub.beacon_ies);
	if (ies && !bss->pub.hidden_beacon_bss)
		kfree_rcu(ies, rcu_head);
	ies = (void *)rcu_access_pointer(bss->pub.proberesp_ies);
	if (ies)
		kfree_rcu(ies, rcu_head);

	/*
	 * This happens when the module is removed, it doesn't
	 * really matter any more save for completeness
	 */
	if (!list_empty(&bss->hidden_list))
		list_del(&bss->hidden_list);

	kfree(bss);
}

static inline void bss_ref_get(struct cfg80211_registered_device *rdev,
			       struct cfg80211_internal_bss *bss)
{
	lockdep_assert_held(&rdev->bss_lock);

	bss->refcount++;

	if (bss->pub.hidden_beacon_bss)
		bss_from_pub(bss->pub.hidden_beacon_bss)->refcount++;

	if (bss->pub.transmitted_bss)
		bss_from_pub(bss->pub.transmitted_bss)->refcount++;
}

static inline void bss_ref_put(struct cfg80211_registered_device *rdev,
			       struct cfg80211_internal_bss *bss)
{
	lockdep_assert_held(&rdev->bss_lock);

	if (bss->pub.hidden_beacon_bss) {
		struct cfg80211_internal_bss *hbss;

		hbss = bss_from_pub(bss->pub.hidden_beacon_bss);
		hbss->refcount--;
		if (hbss->refcount == 0)
			bss_free(hbss);
	}

	if (bss->pub.transmitted_bss) {
		struct cfg80211_internal_bss *tbss;

		tbss = bss_from_pub(bss->pub.transmitted_bss);
		tbss->refcount--;
		if (tbss->refcount == 0)
			bss_free(tbss);
	}

	bss->refcount--;
	if (bss->refcount == 0)
		bss_free(bss);
}

static bool __cfg80211_unlink_bss(struct cfg80211_registered_device *rdev,
				  struct cfg80211_internal_bss *bss)
{
	lockdep_assert_held(&rdev->bss_lock);

	if (!list_empty(&bss->hidden_list)) {
		/*
		 * don't remove the beacon entry if it has
		 * probe responses associated with it
		 */
		if (!bss->pub.hidden_beacon_bss)
			return false;
		/*
		 * if it's a probe response entry break its
		 * link to the other entries in the group
		 */
		list_del_init(&bss->hidden_list);
	}

	list_del_init(&bss->list);
	list_del_init(&bss->pub.nontrans_list);
	rb_erase(&bss->rbn, &rdev->bss_tree);
	rdev->bss_entries--;
	WARN_ONCE((rdev->bss_entries == 0) ^ list_empty(&rdev->bss_list),
		  "rdev bss entries[%d]/list[empty:%d] corruption\n",
		  rdev->bss_entries, list_empty(&rdev->bss_list));
	bss_ref_put(rdev, bss);
	return true;
}

bool cfg80211_is_element_inherited(const struct element *elem,
				   const struct element *non_inherit_elem)
{
	u8 id_len, ext_id_len, i, loop_len, id;
	const u8 *list;

	if (elem->id == WLAN_EID_MULTIPLE_BSSID)
		return false;

	if (elem->id == WLAN_EID_EXTENSION && elem->datalen > 1 &&
	    elem->data[0] == WLAN_EID_EXT_EHT_MULTI_LINK)
		return false;

	if (!non_inherit_elem || non_inherit_elem->datalen < 2)
		return true;

	/*
	 * non inheritance element format is:
	 * ext ID (56) | IDs list len | list | extension IDs list len | list
	 * Both lists are optional. Both lengths are mandatory.
	 * This means valid length is:
	 * elem_len = 1 (extension ID) + 2 (list len fields) + list lengths
	 */
	id_len = non_inherit_elem->data[1];
	if (non_inherit_elem->datalen < 3 + id_len)
		return true;

	ext_id_len = non_inherit_elem->data[2 + id_len];
	if (non_inherit_elem->datalen < 3 + id_len + ext_id_len)
		return true;

	if (elem->id == WLAN_EID_EXTENSION) {
		if (!ext_id_len)
			return true;
		loop_len = ext_id_len;
		list = &non_inherit_elem->data[3 + id_len];
		id = elem->data[0];
	} else {
		if (!id_len)
			return true;
		loop_len = id_len;
		list = &non_inherit_elem->data[2];
		id = elem->id;
	}

	for (i = 0; i < loop_len; i++) {
		if (list[i] == id)
			return false;
	}

	return true;
}
EXPORT_SYMBOL(cfg80211_is_element_inherited);

static size_t cfg80211_copy_elem_with_frags(const struct element *elem,
					    const u8 *ie, size_t ie_len,
					    u8 **pos, u8 *buf, size_t buf_len)
{
	if (WARN_ON((u8 *)elem < ie || elem->data > ie + ie_len ||
		    elem->data + elem->datalen > ie + ie_len))
		return 0;

	if (elem->datalen + 2 > buf + buf_len - *pos)
		return 0;

	memcpy(*pos, elem, elem->datalen + 2);
	*pos += elem->datalen + 2;

	/* Finish if it is not fragmented  */
	if (elem->datalen != 255)
		return *pos - buf;

	ie_len = ie + ie_len - elem->data - elem->datalen;
	ie = (const u8 *)elem->data + elem->datalen;

	for_each_element(elem, ie, ie_len) {
		if (elem->id != WLAN_EID_FRAGMENT)
			break;

		if (elem->datalen + 2 > buf + buf_len - *pos)
			return 0;

		memcpy(*pos, elem, elem->datalen + 2);
		*pos += elem->datalen + 2;

		if (elem->datalen != 255)
			break;
	}

	return *pos - buf;
}

VISIBLE_IF_CFG80211_KUNIT size_t
cfg80211_gen_new_ie(const u8 *ie, size_t ielen,
		    const u8 *subie, size_t subie_len,
		    u8 *new_ie, size_t new_ie_len)
{
	const struct element *non_inherit_elem, *parent, *sub;
	u8 *pos = new_ie;
	u8 id, ext_id;
	unsigned int match_len;

	non_inherit_elem = cfg80211_find_ext_elem(WLAN_EID_EXT_NON_INHERITANCE,
						  subie, subie_len);

	/* We copy the elements one by one from the parent to the generated
	 * elements.
	 * If they are not inherited (included in subie or in the non
	 * inheritance element), then we copy all occurrences the first time
	 * we see this element type.
	 */
	for_each_element(parent, ie, ielen) {
		if (parent->id == WLAN_EID_FRAGMENT)
			continue;

		if (parent->id == WLAN_EID_EXTENSION) {
			if (parent->datalen < 1)
				continue;

			id = WLAN_EID_EXTENSION;
			ext_id = parent->data[0];
			match_len = 1;
		} else {
			id = parent->id;
			match_len = 0;
		}

		/* Find first occurrence in subie */
		sub = cfg80211_find_elem_match(id, subie, subie_len,
					       &ext_id, match_len, 0);

		/* Copy from parent if not in subie and inherited */
		if (!sub &&
		    cfg80211_is_element_inherited(parent, non_inherit_elem)) {
			if (!cfg80211_copy_elem_with_frags(parent,
							   ie, ielen,
							   &pos, new_ie,
							   new_ie_len))
				return 0;

			continue;
		}

		/* Already copied if an earlier element had the same type */
		if (cfg80211_find_elem_match(id, ie, (u8 *)parent - ie,
					     &ext_id, match_len, 0))
			continue;

		/* Not inheriting, copy all similar elements from subie */
		while (sub) {
			if (!cfg80211_copy_elem_with_frags(sub,
							   subie, subie_len,
							   &pos, new_ie,
							   new_ie_len))
				return 0;

			sub = cfg80211_find_elem_match(id,
						       sub->data + sub->datalen,
						       subie_len + subie -
						       (sub->data +
							sub->datalen),
						       &ext_id, match_len, 0);
		}
	}

	/* The above misses elements that are included in subie but not in the
	 * parent, so do a pass over subie and append those.
	 * Skip the non-tx BSSID caps and non-inheritance element.
	 */
	for_each_element(sub, subie, subie_len) {
		if (sub->id == WLAN_EID_NON_TX_BSSID_CAP)
			continue;

		if (sub->id == WLAN_EID_FRAGMENT)
			continue;

		if (sub->id == WLAN_EID_EXTENSION) {
			if (sub->datalen < 1)
				continue;

			id = WLAN_EID_EXTENSION;
			ext_id = sub->data[0];
			match_len = 1;

			if (ext_id == WLAN_EID_EXT_NON_INHERITANCE)
				continue;
		} else {
			id = sub->id;
			match_len = 0;
		}

		/* Processed if one was included in the parent */
		if (cfg80211_find_elem_match(id, ie, ielen,
					     &ext_id, match_len, 0))
			continue;

		if (!cfg80211_copy_elem_with_frags(sub, subie, subie_len,
						   &pos, new_ie, new_ie_len))
			return 0;
	}

	return pos - new_ie;
}
EXPORT_SYMBOL_IF_CFG80211_KUNIT(cfg80211_gen_new_ie);

static bool is_bss(struct cfg80211_bss *a, const u8 *bssid,
		   const u8 *ssid, size_t ssid_len)
{
	const struct cfg80211_bss_ies *ies;
	const struct element *ssid_elem;

	if (bssid && !ether_addr_equal(a->bssid, bssid))
		return false;

	if (!ssid)
		return true;

	ies = rcu_access_pointer(a->ies);
	if (!ies)
		return false;
	ssid_elem = cfg80211_find_elem(WLAN_EID_SSID, ies->data, ies->len);
	if (!ssid_elem)
		return false;
	if (ssid_elem->datalen != ssid_len)
		return false;
	return memcmp(ssid_elem->data, ssid, ssid_len) == 0;
}

static int
cfg80211_add_nontrans_list(struct cfg80211_bss *trans_bss,
			   struct cfg80211_bss *nontrans_bss)
{
	const struct element *ssid_elem;
	struct cfg80211_bss *bss = NULL;

	rcu_read_lock();
	ssid_elem = ieee80211_bss_get_elem(nontrans_bss, WLAN_EID_SSID);
	if (!ssid_elem) {
		rcu_read_unlock();
		return -EINVAL;
	}

	/* check if nontrans_bss is in the list */
	list_for_each_entry(bss, &trans_bss->nontrans_list, nontrans_list) {
		if (is_bss(bss, nontrans_bss->bssid, ssid_elem->data,
			   ssid_elem->datalen)) {
			rcu_read_unlock();
			return 0;
		}
	}

	rcu_read_unlock();

	/*
	 * This is a bit weird - it's not on the list, but already on another
	 * one! The only way that could happen is if there's some BSSID/SSID
	 * shared by multiple APs in their multi-BSSID profiles, potentially
	 * with hidden SSID mixed in ... ignore it.
	 */
	if (!list_empty(&nontrans_bss->nontrans_list))
		return -EINVAL;

	/* add to the list */
	list_add_tail(&nontrans_bss->nontrans_list, &trans_bss->nontrans_list);
	return 0;
}

static void __cfg80211_bss_expire(struct cfg80211_registered_device *rdev,
				  unsigned long expire_time)
{
	struct cfg80211_internal_bss *bss, *tmp;
	bool expired = false;

	lockdep_assert_held(&rdev->bss_lock);

	list_for_each_entry_safe(bss, tmp, &rdev->bss_list, list) {
		if (atomic_read(&bss->hold))
			continue;
		if (!time_after(expire_time, bss->ts))
			continue;

		if (__cfg80211_unlink_bss(rdev, bss))
			expired = true;
	}

	if (expired)
		rdev->bss_generation++;
}

static bool cfg80211_bss_expire_oldest(struct cfg80211_registered_device *rdev)
{
	struct cfg80211_internal_bss *bss, *oldest = NULL;
	bool ret;

	lockdep_assert_held(&rdev->bss_lock);

	list_for_each_entry(bss, &rdev->bss_list, list) {
		if (atomic_read(&bss->hold))
			continue;

		if (!list_empty(&bss->hidden_list) &&
		    !bss->pub.hidden_beacon_bss)
			continue;

		if (oldest && time_before(oldest->ts, bss->ts))
			continue;
		oldest = bss;
	}

	if (WARN_ON(!oldest))
		return false;

	/*
	 * The callers make sure to increase rdev->bss_generation if anything
	 * gets removed (and a new entry added), so there's no need to also do
	 * it here.
	 */

	ret = __cfg80211_unlink_bss(rdev, oldest);
	WARN_ON(!ret);
	return ret;
}

static u8 cfg80211_parse_bss_param(u8 data,
				   struct cfg80211_colocated_ap *coloc_ap)
{
	coloc_ap->oct_recommended =
		u8_get_bits(data, IEEE80211_RNR_TBTT_PARAMS_OCT_RECOMMENDED);
	coloc_ap->same_ssid =
		u8_get_bits(data, IEEE80211_RNR_TBTT_PARAMS_SAME_SSID);
	coloc_ap->multi_bss =
		u8_get_bits(data, IEEE80211_RNR_TBTT_PARAMS_MULTI_BSSID);
	coloc_ap->transmitted_bssid =
		u8_get_bits(data, IEEE80211_RNR_TBTT_PARAMS_TRANSMITTED_BSSID);
	coloc_ap->unsolicited_probe =
		u8_get_bits(data, IEEE80211_RNR_TBTT_PARAMS_PROBE_ACTIVE);
	coloc_ap->colocated_ess =
		u8_get_bits(data, IEEE80211_RNR_TBTT_PARAMS_COLOC_ESS);

	return u8_get_bits(data, IEEE80211_RNR_TBTT_PARAMS_COLOC_AP);
}

static int cfg80211_calc_short_ssid(const struct cfg80211_bss_ies *ies,
				    const struct element **elem, u32 *s_ssid)
{

	*elem = cfg80211_find_elem(WLAN_EID_SSID, ies->data, ies->len);
	if (!*elem || (*elem)->datalen > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;

	*s_ssid = ~crc32_le(~0, (*elem)->data, (*elem)->datalen);
	return 0;
}

static void cfg80211_free_coloc_ap_list(struct list_head *coloc_ap_list)
{
	struct cfg80211_colocated_ap *ap, *tmp_ap;

	list_for_each_entry_safe(ap, tmp_ap, coloc_ap_list, list) {
		list_del(&ap->list);
		kfree(ap);
	}
}

static int cfg80211_parse_ap_info(struct cfg80211_colocated_ap *entry,
				  const u8 *pos, u8 length,
				  const struct element *ssid_elem,
				  u32 s_ssid_tmp)
{
	u8 bss_params;

	entry->psd_20 = IEEE80211_RNR_TBTT_PARAMS_PSD_RESERVED;

	/* The length is already verified by the caller to contain bss_params */
	if (length > sizeof(struct ieee80211_tbtt_info_7_8_9)) {
		struct ieee80211_tbtt_info_ge_11 *tbtt_info = (void *)pos;

		memcpy(entry->bssid, tbtt_info->bssid, ETH_ALEN);
		entry->short_ssid = le32_to_cpu(tbtt_info->short_ssid);
		entry->short_ssid_valid = true;

		bss_params = tbtt_info->bss_params;

		/* Ignore disabled links */
		if (length >= offsetofend(typeof(*tbtt_info), mld_params)) {
			if (le16_get_bits(tbtt_info->mld_params.params,
					  IEEE80211_RNR_MLD_PARAMS_DISABLED_LINK))
				return -EINVAL;
		}

		if (length >= offsetofend(struct ieee80211_tbtt_info_ge_11,
					  psd_20))
			entry->psd_20 = tbtt_info->psd_20;
	} else {
		struct ieee80211_tbtt_info_7_8_9 *tbtt_info = (void *)pos;

		memcpy(entry->bssid, tbtt_info->bssid, ETH_ALEN);

		bss_params = tbtt_info->bss_params;

		if (length == offsetofend(struct ieee80211_tbtt_info_7_8_9,
					  psd_20))
			entry->psd_20 = tbtt_info->psd_20;
	}

	/* ignore entries with invalid BSSID */
	if (!is_valid_ether_addr(entry->bssid))
		return -EINVAL;

	/* skip non colocated APs */
	if (!cfg80211_parse_bss_param(bss_params, entry))
		return -EINVAL;

	/* no information about the short ssid. Consider the entry valid
	 * for now. It would later be dropped in case there are explicit
	 * SSIDs that need to be matched
	 */
	if (!entry->same_ssid && !entry->short_ssid_valid)
		return 0;

	if (entry->same_ssid) {
		entry->short_ssid = s_ssid_tmp;
		entry->short_ssid_valid = true;

		/*
		 * This is safe because we validate datalen in
		 * cfg80211_parse_colocated_ap(), before calling this
		 * function.
		 */
		memcpy(&entry->ssid, &ssid_elem->data, ssid_elem->datalen);
		entry->ssid_len = ssid_elem->datalen;
	}

	return 0;
}

static int cfg80211_parse_colocated_ap(const struct cfg80211_bss_ies *ies,
				       struct list_head *list)
{
	struct ieee80211_neighbor_ap_info *ap_info;
	const struct element *elem, *ssid_elem;
	const u8 *pos, *end;
	u32 s_ssid_tmp;
	int n_coloc = 0, ret;
	LIST_HEAD(ap_list);

	ret = cfg80211_calc_short_ssid(ies, &ssid_elem, &s_ssid_tmp);
	if (ret)
		return 0;

	for_each_element_id(elem, WLAN_EID_REDUCED_NEIGHBOR_REPORT,
			    ies->data, ies->len) {
		pos = elem->data;
		end = elem->data + elem->datalen;

		/* RNR IE may contain more than one NEIGHBOR_AP_INFO */
		while (pos + sizeof(*ap_info) <= end) {
			enum nl80211_band band;
			int freq;
			u8 length, i, count;

			ap_info = (void *)pos;
			count = u8_get_bits(ap_info->tbtt_info_hdr,
					    IEEE80211_AP_INFO_TBTT_HDR_COUNT) + 1;
			length = ap_info->tbtt_info_len;

			pos += sizeof(*ap_info);

			if (!ieee80211_operating_class_to_band(ap_info->op_class,
							       &band))
				break;

			freq = ieee80211_channel_to_frequency(ap_info->channel,
							      band);

			if (end - pos < count * length)
				break;

			if (u8_get_bits(ap_info->tbtt_info_hdr,
					IEEE80211_AP_INFO_TBTT_HDR_TYPE) !=
			    IEEE80211_TBTT_INFO_TYPE_TBTT) {
				pos += count * length;
				continue;
			}

			/* TBTT info must include bss param + BSSID +
			 * (short SSID or same_ssid bit to be set).
			 * ignore other options, and move to the
			 * next AP info
			 */
			if (band != NL80211_BAND_6GHZ ||
			    !(length == offsetofend(struct ieee80211_tbtt_info_7_8_9,
						    bss_params) ||
			      length == sizeof(struct ieee80211_tbtt_info_7_8_9) ||
			      length >= offsetofend(struct ieee80211_tbtt_info_ge_11,
						    bss_params))) {
				pos += count * length;
				continue;
			}

			for (i = 0; i < count; i++) {
				struct cfg80211_colocated_ap *entry;

				entry = kzalloc(sizeof(*entry) + IEEE80211_MAX_SSID_LEN,
						GFP_ATOMIC);

				if (!entry)
					goto error;

				entry->center_freq = freq;

				if (!cfg80211_parse_ap_info(entry, pos, length,
							    ssid_elem,
							    s_ssid_tmp)) {
					n_coloc++;
					list_add_tail(&entry->list, &ap_list);
				} else {
					kfree(entry);
				}

				pos += length;
			}
		}

error:
		if (pos != end) {
			cfg80211_free_coloc_ap_list(&ap_list);
			return 0;
		}
	}

	list_splice_tail(&ap_list, list);
	return n_coloc;
}

static  void cfg80211_scan_req_add_chan(struct cfg80211_scan_request *request,
					struct ieee80211_channel *chan,
					bool add_to_6ghz)
{
	int i;
	u32 n_channels = request->n_channels;
	struct cfg80211_scan_6ghz_params *params =
		&request->scan_6ghz_params[request->n_6ghz_params];

	for (i = 0; i < n_channels; i++) {
		if (request->channels[i] == chan) {
			if (add_to_6ghz)
				params->channel_idx = i;
			return;
		}
	}

	request->channels[n_channels] = chan;
	if (add_to_6ghz)
		request->scan_6ghz_params[request->n_6ghz_params].channel_idx =
			n_channels;

	request->n_channels++;
}

static bool cfg80211_find_ssid_match(struct cfg80211_colocated_ap *ap,
				     struct cfg80211_scan_request *request)
{
	int i;
	u32 s_ssid;

	for (i = 0; i < request->n_ssids; i++) {
		/* wildcard ssid in the scan request */
		if (!request->ssids[i].ssid_len) {
			if (ap->multi_bss && !ap->transmitted_bssid)
				continue;

			return true;
		}

		if (ap->ssid_len &&
		    ap->ssid_len == request->ssids[i].ssid_len) {
			if (!memcmp(request->ssids[i].ssid, ap->ssid,
				    ap->ssid_len))
				return true;
		} else if (ap->short_ssid_valid) {
			s_ssid = ~crc32_le(~0, request->ssids[i].ssid,
					   request->ssids[i].ssid_len);

			if (ap->short_ssid == s_ssid)
				return true;
		}
	}

	return false;
}

static int cfg80211_scan_6ghz(struct cfg80211_registered_device *rdev)
{
	u8 i;
	struct cfg80211_colocated_ap *ap;
	int n_channels, count = 0, err;
	struct cfg80211_scan_request *request, *rdev_req = rdev->scan_req;
	LIST_HEAD(coloc_ap_list);
	bool need_scan_psc = true;
	const struct ieee80211_sband_iftype_data *iftd;

	rdev_req->scan_6ghz = true;

	if (!rdev->wiphy.bands[NL80211_BAND_6GHZ])
		return -EOPNOTSUPP;

	iftd = ieee80211_get_sband_iftype_data(rdev->wiphy.bands[NL80211_BAND_6GHZ],
					       rdev_req->wdev->iftype);
	if (!iftd || !iftd->he_cap.has_he)
		return -EOPNOTSUPP;

	n_channels = rdev->wiphy.bands[NL80211_BAND_6GHZ]->n_channels;

	if (rdev_req->flags & NL80211_SCAN_FLAG_COLOCATED_6GHZ) {
		struct cfg80211_internal_bss *intbss;

		spin_lock_bh(&rdev->bss_lock);
		list_for_each_entry(intbss, &rdev->bss_list, list) {
			struct cfg80211_bss *res = &intbss->pub;
			const struct cfg80211_bss_ies *ies;
			const struct element *ssid_elem;
			struct cfg80211_colocated_ap *entry;
			u32 s_ssid_tmp;
			int ret;

			ies = rcu_access_pointer(res->ies);
			count += cfg80211_parse_colocated_ap(ies,
							     &coloc_ap_list);

			/* In case the scan request specified a specific BSSID
			 * and the BSS is found and operating on 6GHz band then
			 * add this AP to the collocated APs list.
			 * This is relevant for ML probe requests when the lower
			 * band APs have not been discovered.
			 */
			if (is_broadcast_ether_addr(rdev_req->bssid) ||
			    !ether_addr_equal(rdev_req->bssid, res->bssid) ||
			    res->channel->band != NL80211_BAND_6GHZ)
				continue;

			ret = cfg80211_calc_short_ssid(ies, &ssid_elem,
						       &s_ssid_tmp);
			if (ret)
				continue;

			entry = kzalloc(sizeof(*entry) + IEEE80211_MAX_SSID_LEN,
					GFP_ATOMIC);

			if (!entry)
				continue;

			memcpy(entry->bssid, res->bssid, ETH_ALEN);
			entry->short_ssid = s_ssid_tmp;
			memcpy(entry->ssid, ssid_elem->data,
			       ssid_elem->datalen);
			entry->ssid_len = ssid_elem->datalen;
			entry->short_ssid_valid = true;
			entry->center_freq = res->channel->center_freq;

			list_add_tail(&entry->list, &coloc_ap_list);
			count++;
		}
		spin_unlock_bh(&rdev->bss_lock);
	}

	request = kzalloc(struct_size(request, channels, n_channels) +
			  sizeof(*request->scan_6ghz_params) * count +
			  sizeof(*request->ssids) * rdev_req->n_ssids,
			  GFP_KERNEL);
	if (!request) {
		cfg80211_free_coloc_ap_list(&coloc_ap_list);
		return -ENOMEM;
	}

	*request = *rdev_req;
	request->n_channels = 0;
	request->scan_6ghz_params =
		(void *)&request->channels[n_channels];

	/*
	 * PSC channels should not be scanned in case of direct scan with 1 SSID
	 * and at least one of the reported co-located APs with same SSID
	 * indicating that all APs in the same ESS are co-located
	 */
	if (count && request->n_ssids == 1 && request->ssids[0].ssid_len) {
		list_for_each_entry(ap, &coloc_ap_list, list) {
			if (ap->colocated_ess &&
			    cfg80211_find_ssid_match(ap, request)) {
				need_scan_psc = false;
				break;
			}
		}
	}

	/*
	 * add to the scan request the channels that need to be scanned
	 * regardless of the collocated APs (PSC channels or all channels
	 * in case that NL80211_SCAN_FLAG_COLOCATED_6GHZ is not set)
	 */
	for (i = 0; i < rdev_req->n_channels; i++) {
		if (rdev_req->channels[i]->band == NL80211_BAND_6GHZ &&
		    ((need_scan_psc &&
		      cfg80211_channel_is_psc(rdev_req->channels[i])) ||
		     !(rdev_req->flags & NL80211_SCAN_FLAG_COLOCATED_6GHZ))) {
			cfg80211_scan_req_add_chan(request,
						   rdev_req->channels[i],
						   false);
		}
	}

	if (!(rdev_req->flags & NL80211_SCAN_FLAG_COLOCATED_6GHZ))
		goto skip;

	list_for_each_entry(ap, &coloc_ap_list, list) {
		bool found = false;
		struct cfg80211_scan_6ghz_params *scan_6ghz_params =
			&request->scan_6ghz_params[request->n_6ghz_params];
		struct ieee80211_channel *chan =
			ieee80211_get_channel(&rdev->wiphy, ap->center_freq);

		if (!chan || chan->flags & IEEE80211_CHAN_DISABLED)
			continue;

		for (i = 0; i < rdev_req->n_channels; i++) {
			if (rdev_req->channels[i] == chan)
				found = true;
		}

		if (!found)
			continue;

		if (request->n_ssids > 0 &&
		    !cfg80211_find_ssid_match(ap, request))
			continue;

		if (!is_broadcast_ether_addr(request->bssid) &&
		    !ether_addr_equal(request->bssid, ap->bssid))
			continue;

		if (!request->n_ssids && ap->multi_bss && !ap->transmitted_bssid)
			continue;

		cfg80211_scan_req_add_chan(request, chan, true);
		memcpy(scan_6ghz_params->bssid, ap->bssid, ETH_ALEN);
		scan_6ghz_params->short_ssid = ap->short_ssid;
		scan_6ghz_params->short_ssid_valid = ap->short_ssid_valid;
		scan_6ghz_params->unsolicited_probe = ap->unsolicited_probe;
		scan_6ghz_params->psd_20 = ap->psd_20;

		/*
		 * If a PSC channel is added to the scan and 'need_scan_psc' is
		 * set to false, then all the APs that the scan logic is
		 * interested with on the channel are collocated and thus there
		 * is no need to perform the initial PSC channel listen.
		 */
		if (cfg80211_channel_is_psc(chan) && !need_scan_psc)
			scan_6ghz_params->psc_no_listen = true;

		request->n_6ghz_params++;
	}

skip:
	cfg80211_free_coloc_ap_list(&coloc_ap_list);

	if (request->n_channels) {
		struct cfg80211_scan_request *old = rdev->int_scan_req;
		rdev->int_scan_req = request;

		/*
		 * Add the ssids from the parent scan request to the new scan
		 * request, so the driver would be able to use them in its
		 * probe requests to discover hidden APs on PSC channels.
		 */
		request->ssids = (void *)&request->channels[request->n_channels];
		request->n_ssids = rdev_req->n_ssids;
		memcpy(request->ssids, rdev_req->ssids, sizeof(*request->ssids) *
		       request->n_ssids);

		/*
		 * If this scan follows a previous scan, save the scan start
		 * info from the first part of the scan
		 */
		if (old)
			rdev->int_scan_req->info = old->info;

		err = rdev_scan(rdev, request);
		if (err) {
			rdev->int_scan_req = old;
			kfree(request);
		} else {
			kfree(old);
		}

		return err;
	}

	kfree(request);
	return -EINVAL;
}

int cfg80211_scan(struct cfg80211_registered_device *rdev)
{
	struct cfg80211_scan_request *request;
	struct cfg80211_scan_request *rdev_req = rdev->scan_req;
	u32 n_channels = 0, idx, i;

	if (!(rdev->wiphy.flags & WIPHY_FLAG_SPLIT_SCAN_6GHZ))
		return rdev_scan(rdev, rdev_req);

	for (i = 0; i < rdev_req->n_channels; i++) {
		if (rdev_req->channels[i]->band != NL80211_BAND_6GHZ)
			n_channels++;
	}

	if (!n_channels)
		return cfg80211_scan_6ghz(rdev);

	request = kzalloc(struct_size(request, channels, n_channels),
			  GFP_KERNEL);
	if (!request)
		return -ENOMEM;

	*request = *rdev_req;
	request->n_channels = n_channels;

	for (i = idx = 0; i < rdev_req->n_channels; i++) {
		if (rdev_req->channels[i]->band != NL80211_BAND_6GHZ)
			request->channels[idx++] = rdev_req->channels[i];
	}

	rdev_req->scan_6ghz = false;
	rdev->int_scan_req = request;
	return rdev_scan(rdev, request);
}

void ___cfg80211_scan_done(struct cfg80211_registered_device *rdev,
			   bool send_message)
{
	struct cfg80211_scan_request *request, *rdev_req;
	struct wireless_dev *wdev;
	struct sk_buff *msg;
#ifdef CONFIG_CFG80211_WEXT
	union iwreq_data wrqu;
#endif

	lockdep_assert_held(&rdev->wiphy.mtx);

	if (rdev->scan_msg) {
		nl80211_send_scan_msg(rdev, rdev->scan_msg);
		rdev->scan_msg = NULL;
		return;
	}

	rdev_req = rdev->scan_req;
	if (!rdev_req)
		return;

	wdev = rdev_req->wdev;
	request = rdev->int_scan_req ? rdev->int_scan_req : rdev_req;

	if (wdev_running(wdev) &&
	    (rdev->wiphy.flags & WIPHY_FLAG_SPLIT_SCAN_6GHZ) &&
	    !rdev_req->scan_6ghz && !request->info.aborted &&
	    !cfg80211_scan_6ghz(rdev))
		return;

	/*
	 * This must be before sending the other events!
	 * Otherwise, wpa_supplicant gets completely confused with
	 * wext events.
	 */
	if (wdev->netdev)
		cfg80211_sme_scan_done(wdev->netdev);

	if (!request->info.aborted &&
	    request->flags & NL80211_SCAN_FLAG_FLUSH) {
		/* flush entries from previous scans */
		spin_lock_bh(&rdev->bss_lock);
		__cfg80211_bss_expire(rdev, request->scan_start);
		spin_unlock_bh(&rdev->bss_lock);
	}

	msg = nl80211_build_scan_msg(rdev, wdev, request->info.aborted);

#ifdef CONFIG_CFG80211_WEXT
	if (wdev->netdev && !request->info.aborted) {
		memset(&wrqu, 0, sizeof(wrqu));

		wireless_send_event(wdev->netdev, SIOCGIWSCAN, &wrqu, NULL);
	}
#endif

	dev_put(wdev->netdev);

	kfree(rdev->int_scan_req);
	rdev->int_scan_req = NULL;

	kfree(rdev->scan_req);
	rdev->scan_req = NULL;

	if (!send_message)
		rdev->scan_msg = msg;
	else
		nl80211_send_scan_msg(rdev, msg);
}

void __cfg80211_scan_done(struct wiphy *wiphy, struct wiphy_work *wk)
{
	___cfg80211_scan_done(wiphy_to_rdev(wiphy), true);
}

void cfg80211_scan_done(struct cfg80211_scan_request *request,
			struct cfg80211_scan_info *info)
{
	struct cfg80211_scan_info old_info = request->info;

	trace_cfg80211_scan_done(request, info);
	WARN_ON(request != wiphy_to_rdev(request->wiphy)->scan_req &&
		request != wiphy_to_rdev(request->wiphy)->int_scan_req);

	request->info = *info;

	/*
	 * In case the scan is split, the scan_start_tsf and tsf_bssid should
	 * be of the first part. In such a case old_info.scan_start_tsf should
	 * be non zero.
	 */
	if (request->scan_6ghz && old_info.scan_start_tsf) {
		request->info.scan_start_tsf = old_info.scan_start_tsf;
		memcpy(request->info.tsf_bssid, old_info.tsf_bssid,
		       sizeof(request->info.tsf_bssid));
	}

	request->notified = true;
	wiphy_work_queue(request->wiphy,
			 &wiphy_to_rdev(request->wiphy)->scan_done_wk);
}
EXPORT_SYMBOL(cfg80211_scan_done);

void cfg80211_add_sched_scan_req(struct cfg80211_registered_device *rdev,
				 struct cfg80211_sched_scan_request *req)
{
	lockdep_assert_held(&rdev->wiphy.mtx);

	list_add_rcu(&req->list, &rdev->sched_scan_req_list);
}

static void cfg80211_del_sched_scan_req(struct cfg80211_registered_device *rdev,
					struct cfg80211_sched_scan_request *req)
{
	lockdep_assert_held(&rdev->wiphy.mtx);

	list_del_rcu(&req->list);
	kfree_rcu(req, rcu_head);
}

static struct cfg80211_sched_scan_request *
cfg80211_find_sched_scan_req(struct cfg80211_registered_device *rdev, u64 reqid)
{
	struct cfg80211_sched_scan_request *pos;

	list_for_each_entry_rcu(pos, &rdev->sched_scan_req_list, list,
				lockdep_is_held(&rdev->wiphy.mtx)) {
		if (pos->reqid == reqid)
			return pos;
	}
	return NULL;
}

/*
 * Determines if a scheduled scan request can be handled. When a legacy
 * scheduled scan is running no other scheduled scan is allowed regardless
 * whether the request is for legacy or multi-support scan. When a multi-support
 * scheduled scan is running a request for legacy scan is not allowed. In this
 * case a request for multi-support scan can be handled if resources are
 * available, ie. struct wiphy::max_sched_scan_reqs limit is not yet reached.
 */
int cfg80211_sched_scan_req_possible(struct cfg80211_registered_device *rdev,
				     bool want_multi)
{
	struct cfg80211_sched_scan_request *pos;
	int i = 0;

	list_for_each_entry(pos, &rdev->sched_scan_req_list, list) {
		/* request id zero means legacy in progress */
		if (!i && !pos->reqid)
			return -EINPROGRESS;
		i++;
	}

	if (i) {
		/* no legacy allowed when multi request(s) are active */
		if (!want_multi)
			return -EINPROGRESS;

		/* resource limit reached */
		if (i == rdev->wiphy.max_sched_scan_reqs)
			return -ENOSPC;
	}
	return 0;
}

void cfg80211_sched_scan_results_wk(struct work_struct *work)
{
	struct cfg80211_registered_device *rdev;
	struct cfg80211_sched_scan_request *req, *tmp;

	rdev = container_of(work, struct cfg80211_registered_device,
			   sched_scan_res_wk);

	wiphy_lock(&rdev->wiphy);
	list_for_each_entry_safe(req, tmp, &rdev->sched_scan_req_list, list) {
		if (req->report_results) {
			req->report_results = false;
			if (req->flags & NL80211_SCAN_FLAG_FLUSH) {
				/* flush entries from previous scans */
				spin_lock_bh(&rdev->bss_lock);
				__cfg80211_bss_expire(rdev, req->scan_start);
				spin_unlock_bh(&rdev->bss_lock);
				req->scan_start = jiffies;
			}
			nl80211_send_sched_scan(req,
						NL80211_CMD_SCHED_SCAN_RESULTS);
		}
	}
	wiphy_unlock(&rdev->wiphy);
}

void cfg80211_sched_scan_results(struct wiphy *wiphy, u64 reqid)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);
	struct cfg80211_sched_scan_request *request;

	trace_cfg80211_sched_scan_results(wiphy, reqid);
	/* ignore if we're not scanning */

	rcu_read_lock();
	request = cfg80211_find_sched_scan_req(rdev, reqid);
	if (request) {
		request->report_results = true;
		queue_work(cfg80211_wq, &rdev->sched_scan_res_wk);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(cfg80211_sched_scan_results);

void cfg80211_sched_scan_stopped_locked(struct wiphy *wiphy, u64 reqid)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);

	lockdep_assert_held(&wiphy->mtx);

	trace_cfg80211_sched_scan_stopped(wiphy, reqid);

	__cfg80211_stop_sched_scan(rdev, reqid, true);
}
EXPORT_SYMBOL(cfg80211_sched_scan_stopped_locked);

void cfg80211_sched_scan_stopped(struct wiphy *wiphy, u64 reqid)
{
	wiphy_lock(wiphy);
	cfg80211_sched_scan_stopped_locked(wiphy, reqid);
	wiphy_unlock(wiphy);
}
EXPORT_SYMBOL(cfg80211_sched_scan_stopped);

int cfg80211_stop_sched_scan_req(struct cfg80211_registered_device *rdev,
				 struct cfg80211_sched_scan_request *req,
				 bool driver_initiated)
{
	lockdep_assert_held(&rdev->wiphy.mtx);

	if (!driver_initiated) {
		int err = rdev_sched_scan_stop(rdev, req->dev, req->reqid);
		if (err)
			return err;
	}

	nl80211_send_sched_scan(req, NL80211_CMD_SCHED_SCAN_STOPPED);

	cfg80211_del_sched_scan_req(rdev, req);

	return 0;
}

int __cfg80211_stop_sched_scan(struct cfg80211_registered_device *rdev,
			       u64 reqid, bool driver_initiated)
{
	struct cfg80211_sched_scan_request *sched_scan_req;

	lockdep_assert_held(&rdev->wiphy.mtx);

	sched_scan_req = cfg80211_find_sched_scan_req(rdev, reqid);
	if (!sched_scan_req)
		return -ENOENT;

	return cfg80211_stop_sched_scan_req(rdev, sched_scan_req,
					    driver_initiated);
}

void cfg80211_bss_age(struct cfg80211_registered_device *rdev,
                      unsigned long age_secs)
{
	struct cfg80211_internal_bss *bss;
	unsigned long age_jiffies = msecs_to_jiffies(age_secs * MSEC_PER_SEC);

	spin_lock_bh(&rdev->bss_lock);
	list_for_each_entry(bss, &rdev->bss_list, list)
		bss->ts -= age_jiffies;
	spin_unlock_bh(&rdev->bss_lock);
}

void cfg80211_bss_expire(struct cfg80211_registered_device *rdev)
{
	__cfg80211_bss_expire(rdev, jiffies - IEEE80211_SCAN_RESULT_EXPIRE);
}

void cfg80211_bss_flush(struct wiphy *wiphy)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);

	spin_lock_bh(&rdev->bss_lock);
	__cfg80211_bss_expire(rdev, jiffies);
	spin_unlock_bh(&rdev->bss_lock);
}
EXPORT_SYMBOL(cfg80211_bss_flush);

const struct element *
cfg80211_find_elem_match(u8 eid, const u8 *ies, unsigned int len,
			 const u8 *match, unsigned int match_len,
			 unsigned int match_offset)
{
	const struct element *elem;

	for_each_element_id(elem, eid, ies, len) {
		if (elem->datalen >= match_offset + match_len &&
		    !memcmp(elem->data + match_offset, match, match_len))
			return elem;
	}

	return NULL;
}
EXPORT_SYMBOL(cfg80211_find_elem_match);

const struct element *cfg80211_find_vendor_elem(unsigned int oui, int oui_type,
						const u8 *ies,
						unsigned int len)
{
	const struct element *elem;
	u8 match[] = { oui >> 16, oui >> 8, oui, oui_type };
	int match_len = (oui_type < 0) ? 3 : sizeof(match);

	if (WARN_ON(oui_type > 0xff))
		return NULL;

	elem = cfg80211_find_elem_match(WLAN_EID_VENDOR_SPECIFIC, ies, len,
					match, match_len, 0);

	if (!elem || elem->datalen < 4)
		return NULL;

	return elem;
}
EXPORT_SYMBOL(cfg80211_find_vendor_elem);

/**
 * enum bss_compare_mode - BSS compare mode
 * @BSS_CMP_REGULAR: regular compare mode (for insertion and normal find)
 * @BSS_CMP_HIDE_ZLEN: find hidden SSID with zero-length mode
 * @BSS_CMP_HIDE_NUL: find hidden SSID with NUL-ed out mode
 */
enum bss_compare_mode {
	BSS_CMP_REGULAR,
	BSS_CMP_HIDE_ZLEN,
	BSS_CMP_HIDE_NUL,
};

static int cmp_bss(struct cfg80211_bss *a,
		   struct cfg80211_bss *b,
		   enum bss_compare_mode mode)
{
	const struct cfg80211_bss_ies *a_ies, *b_ies;
	const u8 *ie1 = NULL;
	const u8 *ie2 = NULL;
	int i, r;

	if (a->channel != b->channel)
		return (b->channel->center_freq * 1000 + b->channel->freq_offset) -
		       (a->channel->center_freq * 1000 + a->channel->freq_offset);

	a_ies = rcu_access_pointer(a->ies);
	if (!a_ies)
		return -1;
	b_ies = rcu_access_pointer(b->ies);
	if (!b_ies)
		return 1;

	if (WLAN_CAPABILITY_IS_STA_BSS(a->capability))
		ie1 = cfg80211_find_ie(WLAN_EID_MESH_ID,
				       a_ies->data, a_ies->len);
	if (WLAN_CAPABILITY_IS_STA_BSS(b->capability))
		ie2 = cfg80211_find_ie(WLAN_EID_MESH_ID,
				       b_ies->data, b_ies->len);
	if (ie1 && ie2) {
		int mesh_id_cmp;

		if (ie1[1] == ie2[1])
			mesh_id_cmp = memcmp(ie1 + 2, ie2 + 2, ie1[1]);
		else
			mesh_id_cmp = ie2[1] - ie1[1];

		ie1 = cfg80211_find_ie(WLAN_EID_MESH_CONFIG,
				       a_ies->data, a_ies->len);
		ie2 = cfg80211_find_ie(WLAN_EID_MESH_CONFIG,
				       b_ies->data, b_ies->len);
		if (ie1 && ie2) {
			if (mesh_id_cmp)
				return mesh_id_cmp;
			if (ie1[1] != ie2[1])
				return ie2[1] - ie1[1];
			return memcmp(ie1 + 2, ie2 + 2, ie1[1]);
		}
	}

	r = memcmp(a->bssid, b->bssid, sizeof(a->bssid));
	if (r)
		return r;

	ie1 = cfg80211_find_ie(WLAN_EID_SSID, a_ies->data, a_ies->len);
	ie2 = cfg80211_find_ie(WLAN_EID_SSID, b_ies->data, b_ies->len);

	if (!ie1 && !ie2)
		return 0;

	/*
	 * Note that with "hide_ssid", the function returns a match if
	 * the already-present BSS ("b") is a hidden SSID beacon for
	 * the new BSS ("a").
	 */

	/* sort missing IE before (left of) present IE */
	if (!ie1)
		return -1;
	if (!ie2)
		return 1;

	switch (mode) {
	case BSS_CMP_HIDE_ZLEN:
		/*
		 * In ZLEN mode we assume the BSS entry we're
		 * looking for has a zero-length SSID. So if
		 * the one we're looking at right now has that,
		 * return 0. Otherwise, return the difference
		 * in length, but since we're looking for the
		 * 0-length it's really equivalent to returning
		 * the length of the one we're looking at.
		 *
		 * No content comparison is needed as we assume
		 * the content length is zero.
		 */
		return ie2[1];
	case BSS_CMP_REGULAR:
	default:
		/* sort by length first, then by contents */
		if (ie1[1] != ie2[1])
			return ie2[1] - ie1[1];
		return memcmp(ie1 + 2, ie2 + 2, ie1[1]);
	case BSS_CMP_HIDE_NUL:
		if (ie1[1] != ie2[1])
			return ie2[1] - ie1[1];
		/* this is equivalent to memcmp(zeroes, ie2 + 2, len) */
		for (i = 0; i < ie2[1]; i++)
			if (ie2[i + 2])
				return -1;
		return 0;
	}
}

static bool cfg80211_bss_type_match(u16 capability,
				    enum nl80211_band band,
				    enum ieee80211_bss_type bss_type)
{
	bool ret = true;
	u16 mask, val;

	if (bss_type == IEEE80211_BSS_TYPE_ANY)
		return ret;

	if (band == NL80211_BAND_60GHZ) {
		mask = WLAN_CAPABILITY_DMG_TYPE_MASK;
		switch (bss_type) {
		case IEEE80211_BSS_TYPE_ESS:
			val = WLAN_CAPABILITY_DMG_TYPE_AP;
			break;
		case IEEE80211_BSS_TYPE_PBSS:
			val = WLAN_CAPABILITY_DMG_TYPE_PBSS;
			break;
		case IEEE80211_BSS_TYPE_IBSS:
			val = WLAN_CAPABILITY_DMG_TYPE_IBSS;
			break;
		default:
			return false;
		}
	} else {
		mask = WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_IBSS;
		switch (bss_type) {
		case IEEE80211_BSS_TYPE_ESS:
			val = WLAN_CAPABILITY_ESS;
			break;
		case IEEE80211_BSS_TYPE_IBSS:
			val = WLAN_CAPABILITY_IBSS;
			break;
		case IEEE80211_BSS_TYPE_MBSS:
			val = 0;
			break;
		default:
			return false;
		}
	}

	ret = ((capability & mask) == val);
	return ret;
}

/* Returned bss is reference counted and must be cleaned up appropriately. */
struct cfg80211_bss *__cfg80211_get_bss(struct wiphy *wiphy,
					struct ieee80211_channel *channel,
					const u8 *bssid,
					const u8 *ssid, size_t ssid_len,
					enum ieee80211_bss_type bss_type,
					enum ieee80211_privacy privacy,
					u32 use_for)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);
	struct cfg80211_internal_bss *bss, *res = NULL;
	unsigned long now = jiffies;
	int bss_privacy;

	trace_cfg80211_get_bss(wiphy, channel, bssid, ssid, ssid_len, bss_type,
			       privacy);

	spin_lock_bh(&rdev->bss_lock);

	list_for_each_entry(bss, &rdev->bss_list, list) {
		if (!cfg80211_bss_type_match(bss->pub.capability,
					     bss->pub.channel->band, bss_type))
			continue;

		bss_privacy = (bss->pub.capability & WLAN_CAPABILITY_PRIVACY);
		if ((privacy == IEEE80211_PRIVACY_ON && !bss_privacy) ||
		    (privacy == IEEE80211_PRIVACY_OFF && bss_privacy))
			continue;
		if (channel && bss->pub.channel != channel)
			continue;
		if (!is_valid_ether_addr(bss->pub.bssid))
			continue;
		if ((bss->pub.use_for & use_for) != use_for)
			continue;
		/* Don't get expired BSS structs */
		if (time_after(now, bss->ts + IEEE80211_SCAN_RESULT_EXPIRE) &&
		    !atomic_read(&bss->hold))
			continue;
		if (is_bss(&bss->pub, bssid, ssid, ssid_len)) {
			res = bss;
			bss_ref_get(rdev, res);
			break;
		}
	}

	spin_unlock_bh(&rdev->bss_lock);
	if (!res)
		return NULL;
	trace_cfg80211_return_bss(&res->pub);
	return &res->pub;
}
EXPORT_SYMBOL(__cfg80211_get_bss);

static void rb_insert_bss(struct cfg80211_registered_device *rdev,
			  struct cfg80211_internal_bss *bss)
{
	struct rb_node **p = &rdev->bss_tree.rb_node;
	struct rb_node *parent = NULL;
	struct cfg80211_internal_bss *tbss;
	int cmp;

	while (*p) {
		parent = *p;
		tbss = rb_entry(parent, struct cfg80211_internal_bss, rbn);

		cmp = cmp_bss(&bss->pub, &tbss->pub, BSS_CMP_REGULAR);

		if (WARN_ON(!cmp)) {
			/* will sort of leak this BSS */
			return;
		}

		if (cmp < 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&bss->rbn, parent, p);
	rb_insert_color(&bss->rbn, &rdev->bss_tree);
}

static struct cfg80211_internal_bss *
rb_find_bss(struct cfg80211_registered_device *rdev,
	    struct cfg80211_internal_bss *res,
	    enum bss_compare_mode mode)
{
	struct rb_node *n = rdev->bss_tree.rb_node;
	struct cfg80211_internal_bss *bss;
	int r;

	while (n) {
		bss = rb_entry(n, struct cfg80211_internal_bss, rbn);
		r = cmp_bss(&res->pub, &bss->pub, mode);

		if (r == 0)
			return bss;
		else if (r < 0)
			n = n->rb_left;
		else
			n = n->rb_right;
	}

	return NULL;
}

static bool cfg80211_combine_bsses(struct cfg80211_registered_device *rdev,
				   struct cfg80211_internal_bss *new)
{
	const struct cfg80211_bss_ies *ies;
	struct cfg80211_internal_bss *bss;
	const u8 *ie;
	int i, ssidlen;
	u8 fold = 0;
	u32 n_entries = 0;

	ies = rcu_access_pointer(new->pub.beacon_ies);
	if (WARN_ON(!ies))
		return false;

	ie = cfg80211_find_ie(WLAN_EID_SSID, ies->data, ies->len);
	if (!ie) {
		/* nothing to do */
		return true;
	}

	ssidlen = ie[1];
	for (i = 0; i < ssidlen; i++)
		fold |= ie[2 + i];

	if (fold) {
		/* not a hidden SSID */
		return true;
	}

	/* This is the bad part ... */

	list_for_each_entry(bss, &rdev->bss_list, list) {
		/*
		 * we're iterating all the entries anyway, so take the
		 * opportunity to validate the list length accounting
		 */
		n_entries++;

		if (!ether_addr_equal(bss->pub.bssid, new->pub.bssid))
			continue;
		if (bss->pub.channel != new->pub.channel)
			continue;
		if (rcu_access_pointer(bss->pub.beacon_ies))
			continue;
		ies = rcu_access_pointer(bss->pub.ies);
		if (!ies)
			continue;
		ie = cfg80211_find_ie(WLAN_EID_SSID, ies->data, ies->len);
		if (!ie)
			continue;
		if (ssidlen && ie[1] != ssidlen)
			continue;
		if (WARN_ON_ONCE(bss->pub.hidden_beacon_bss))
			continue;
		if (WARN_ON_ONCE(!list_empty(&bss->hidden_list)))
			list_del(&bss->hidden_list);
		/* combine them */
		list_add(&bss->hidden_list, &new->hidden_list);
		bss->pub.hidden_beacon_bss = &new->pub;
		new->refcount += bss->refcount;
		rcu_assign_pointer(bss->pub.beacon_ies,
				   new->pub.beacon_ies);
	}

	WARN_ONCE(n_entries != rdev->bss_entries,
		  "rdev bss entries[%d]/list[len:%d] corruption\n",
		  rdev->bss_entries, n_entries);

	return true;
}

static void cfg80211_update_hidden_bsses(struct cfg80211_internal_bss *known,
					 const struct cfg80211_bss_ies *new_ies,
					 const struct cfg80211_bss_ies *old_ies)
{
	struct cfg80211_internal_bss *bss;

	/* Assign beacon IEs to all sub entries */
	list_for_each_entry(bss, &known->hidden_list, hidden_list) {
		const struct cfg80211_bss_ies *ies;

		ies = rcu_access_pointer(bss->pub.beacon_ies);
		WARN_ON(ies != old_ies);

		rcu_assign_pointer(bss->pub.beacon_ies, new_ies);
	}
}

static void cfg80211_check_stuck_ecsa(struct cfg80211_registered_device *rdev,
				      struct cfg80211_internal_bss *known,
				      const struct cfg80211_bss_ies *old)
{
	const struct ieee80211_ext_chansw_ie *ecsa;
	const struct element *elem_new, *elem_old;
	const struct cfg80211_bss_ies *new, *bcn;

	if (known->pub.proberesp_ecsa_stuck)
		return;

	new = rcu_dereference_protected(known->pub.proberesp_ies,
					lockdep_is_held(&rdev->bss_lock));
	if (WARN_ON(!new))
		return;

	if (new->tsf - old->tsf < USEC_PER_SEC)
		return;

	elem_old = cfg80211_find_elem(WLAN_EID_EXT_CHANSWITCH_ANN,
				      old->data, old->len);
	if (!elem_old)
		return;

	elem_new = cfg80211_find_elem(WLAN_EID_EXT_CHANSWITCH_ANN,
				      new->data, new->len);
	if (!elem_new)
		return;

	bcn = rcu_dereference_protected(known->pub.beacon_ies,
					lockdep_is_held(&rdev->bss_lock));
	if (bcn &&
	    cfg80211_find_elem(WLAN_EID_EXT_CHANSWITCH_ANN,
			       bcn->data, bcn->len))
		return;

	if (elem_new->datalen != elem_old->datalen)
		return;
	if (elem_new->datalen < sizeof(struct ieee80211_ext_chansw_ie))
		return;
	if (memcmp(elem_new->data, elem_old->data, elem_new->datalen))
		return;

	ecsa = (void *)elem_new->data;

	if (!ecsa->mode)
		return;

	if (ecsa->new_ch_num !=
	    ieee80211_frequency_to_channel(known->pub.channel->center_freq))
		return;

	known->pub.proberesp_ecsa_stuck = 1;
}

static bool
cfg80211_update_known_bss(struct cfg80211_registered_device *rdev,
			  struct cfg80211_internal_bss *known,
			  struct cfg80211_internal_bss *new,
			  bool signal_valid)
{
	lockdep_assert_held(&rdev->bss_lock);

	/* Update IEs */
	if (rcu_access_pointer(new->pub.proberesp_ies)) {
		const struct cfg80211_bss_ies *old;

		old = rcu_access_pointer(known->pub.proberesp_ies);

		rcu_assign_pointer(known->pub.proberesp_ies,
				   new->pub.proberesp_ies);
		/* Override possible earlier Beacon frame IEs */
		rcu_assign_pointer(known->pub.ies,
				   new->pub.proberesp_ies);
		if (old) {
			cfg80211_check_stuck_ecsa(rdev, known, old);
			kfree_rcu((struct cfg80211_bss_ies *)old, rcu_head);
		}
	}

	if (rcu_access_pointer(new->pub.beacon_ies)) {
		const struct cfg80211_bss_ies *old;

		if (known->pub.hidden_beacon_bss &&
		    !list_empty(&known->hidden_list)) {
			const struct cfg80211_bss_ies *f;

			/* The known BSS struct is one of the probe
			 * response members of a group, but we're
			 * receiving a beacon (beacon_ies in the new
			 * bss is used). This can only mean that the
			 * AP changed its beacon from not having an
			 * SSID to showing it, which is confusing so
			 * drop this information.
			 */

			f = rcu_access_pointer(new->pub.beacon_ies);
			kfree_rcu((struct cfg80211_bss_ies *)f, rcu_head);
			return false;
		}

		old = rcu_access_pointer(known->pub.beacon_ies);

		rcu_assign_pointer(known->pub.beacon_ies, new->pub.beacon_ies);

		/* Override IEs if they were from a beacon before */
		if (old == rcu_access_pointer(known->pub.ies))
			rcu_assign_pointer(known->pub.ies, new->pub.beacon_ies);

		cfg80211_update_hidden_bsses(known,
					     rcu_access_pointer(new->pub.beacon_ies),
					     old);

		if (old)
			kfree_rcu((struct cfg80211_bss_ies *)old, rcu_head);
	}

	known->pub.beacon_interval = new->pub.beacon_interval;

	/* don't update the signal if beacon was heard on
	 * adjacent channel.
	 */
	if (signal_valid)
		known->pub.signal = new->pub.signal;
	known->pub.capability = new->pub.capability;
	known->ts = new->ts;
	known->ts_boottime = new->ts_boottime;
	known->parent_tsf = new->parent_tsf;
	known->pub.chains = new->pub.chains;
	memcpy(known->pub.chain_signal, new->pub.chain_signal,
	       IEEE80211_MAX_CHAINS);
	ether_addr_copy(known->parent_bssid, new->parent_bssid);
	known->pub.max_bssid_indicator = new->pub.max_bssid_indicator;
	known->pub.bssid_index = new->pub.bssid_index;
	known->pub.use_for &= new->pub.use_for;
	known->pub.cannot_use_reasons = new->pub.cannot_use_reasons;

	return true;
}

/* Returned bss is reference counted and must be cleaned up appropriately. */
static struct cfg80211_internal_bss *
__cfg80211_bss_update(struct cfg80211_registered_device *rdev,
		      struct cfg80211_internal_bss *tmp,
		      bool signal_valid, unsigned long ts)
{
	struct cfg80211_internal_bss *found = NULL;
	struct cfg80211_bss_ies *ies;

	if (WARN_ON(!tmp->pub.channel))
		goto free_ies;

	tmp->ts = ts;

	if (WARN_ON(!rcu_access_pointer(tmp->pub.ies)))
		goto free_ies;

	found = rb_find_bss(rdev, tmp, BSS_CMP_REGULAR);

	if (found) {
		if (!cfg80211_update_known_bss(rdev, found, tmp, signal_valid))
			return NULL;
	} else {
		struct cfg80211_internal_bss *new;
		struct cfg80211_internal_bss *hidden;

		/*
		 * create a copy -- the "res" variable that is passed in
		 * is allocated on the stack since it's not needed in the
		 * more common case of an update
		 */
		new = kzalloc(sizeof(*new) + rdev->wiphy.bss_priv_size,
			      GFP_ATOMIC);
		if (!new)
			goto free_ies;
		memcpy(new, tmp, sizeof(*new));
		new->refcount = 1;
		INIT_LIST_HEAD(&new->hidden_list);
		INIT_LIST_HEAD(&new->pub.nontrans_list);
		/* we'll set this later if it was non-NULL */
		new->pub.transmitted_bss = NULL;

		if (rcu_access_pointer(tmp->pub.proberesp_ies)) {
			hidden = rb_find_bss(rdev, tmp, BSS_CMP_HIDE_ZLEN);
			if (!hidden)
				hidden = rb_find_bss(rdev, tmp,
						     BSS_CMP_HIDE_NUL);
			if (hidden) {
				new->pub.hidden_beacon_bss = &hidden->pub;
				list_add(&new->hidden_list,
					 &hidden->hidden_list);
				hidden->refcount++;

				ies = (void *)rcu_access_pointer(new->pub.beacon_ies);
				rcu_assign_pointer(new->pub.beacon_ies,
						   hidden->pub.beacon_ies);
				if (ies)
					kfree_rcu(ies, rcu_head);
			}
		} else {
			/*
			 * Ok so we found a beacon, and don't have an entry. If
			 * it's a beacon with hidden SSID, we might be in for an
			 * expensive search for any probe responses that should
			 * be grouped with this beacon for updates ...
			 */
			if (!cfg80211_combine_bsses(rdev, new)) {
				bss_ref_put(rdev, new);
				return NULL;
			}
		}

		if (rdev->bss_entries >= bss_entries_limit &&
		    !cfg80211_bss_expire_oldest(rdev)) {
			bss_ref_put(rdev, new);
			return NULL;
		}

		/* This must be before the call to bss_ref_get */
		if (tmp->pub.transmitted_bss) {
			new->pub.transmitted_bss = tmp->pub.transmitted_bss;
			bss_ref_get(rdev, bss_from_pub(tmp->pub.transmitted_bss));
		}

		list_add_tail(&new->list, &rdev->bss_list);
		rdev->bss_entries++;
		rb_insert_bss(rdev, new);
		found = new;
	}

	rdev->bss_generation++;
	bss_ref_get(rdev, found);

	return found;

free_ies:
	ies = (void *)rcu_dereference(tmp->pub.beacon_ies);
	if (ies)
		kfree_rcu(ies, rcu_head);
	ies = (void *)rcu_dereference(tmp->pub.proberesp_ies);
	if (ies)
		kfree_rcu(ies, rcu_head);

	return NULL;
}

struct cfg80211_internal_bss *
cfg80211_bss_update(struct cfg80211_registered_device *rdev,
		    struct cfg80211_internal_bss *tmp,
		    bool signal_valid, unsigned long ts)
{
	struct cfg80211_internal_bss *res;

	spin_lock_bh(&rdev->bss_lock);
	res = __cfg80211_bss_update(rdev, tmp, signal_valid, ts);
	spin_unlock_bh(&rdev->bss_lock);

	return res;
}

int cfg80211_get_ies_channel_number(const u8 *ie, size_t ielen,
				    enum nl80211_band band)
{
	const struct element *tmp;

	if (band == NL80211_BAND_6GHZ) {
		struct ieee80211_he_operation *he_oper;

		tmp = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_OPERATION, ie,
					     ielen);
		if (tmp && tmp->datalen >= sizeof(*he_oper) &&
		    tmp->datalen >= ieee80211_he_oper_size(&tmp->data[1])) {
			const struct ieee80211_he_6ghz_oper *he_6ghz_oper;

			he_oper = (void *)&tmp->data[1];

			he_6ghz_oper = ieee80211_he_6ghz_oper(he_oper);
			if (!he_6ghz_oper)
				return -1;

			return he_6ghz_oper->primary;
		}
	} else if (band == NL80211_BAND_S1GHZ) {
		tmp = cfg80211_find_elem(WLAN_EID_S1G_OPERATION, ie, ielen);
		if (tmp && tmp->datalen >= sizeof(struct ieee80211_s1g_oper_ie)) {
			struct ieee80211_s1g_oper_ie *s1gop = (void *)tmp->data;

			return s1gop->oper_ch;
		}
	} else {
		tmp = cfg80211_find_elem(WLAN_EID_DS_PARAMS, ie, ielen);
		if (tmp && tmp->datalen == 1)
			return tmp->data[0];

		tmp = cfg80211_find_elem(WLAN_EID_HT_OPERATION, ie, ielen);
		if (tmp &&
		    tmp->datalen >= sizeof(struct ieee80211_ht_operation)) {
			struct ieee80211_ht_operation *htop = (void *)tmp->data;

			return htop->primary_chan;
		}
	}

	return -1;
}
EXPORT_SYMBOL(cfg80211_get_ies_channel_number);

/*
 * Update RX channel information based on the available frame payload
 * information. This is mainly for the 2.4 GHz band where frames can be received
 * from neighboring channels and the Beacon frames use the DSSS Parameter Set
 * element to indicate the current (transmitting) channel, but this might also
 * be needed on other bands if RX frequency does not match with the actual
 * operating channel of a BSS, or if the AP reports a different primary channel.
 */
static struct ieee80211_channel *
cfg80211_get_bss_channel(struct wiphy *wiphy, const u8 *ie, size_t ielen,
			 struct ieee80211_channel *channel)
{
	u32 freq;
	int channel_number;
	struct ieee80211_channel *alt_channel;

	channel_number = cfg80211_get_ies_channel_number(ie, ielen,
							 channel->band);

	if (channel_number < 0) {
		/* No channel information in frame payload */
		return channel;
	}

	freq = ieee80211_channel_to_freq_khz(channel_number, channel->band);

	/*
	 * Frame info (beacon/prob res) is the same as received channel,
	 * no need for further processing.
	 */
	if (freq == ieee80211_channel_to_khz(channel))
		return channel;

	alt_channel = ieee80211_get_channel_khz(wiphy, freq);
	if (!alt_channel) {
		if (channel->band == NL80211_BAND_2GHZ ||
		    channel->band == NL80211_BAND_6GHZ) {
			/*
			 * Better not allow unexpected channels when that could
			 * be going beyond the 1-11 range (e.g., discovering
			 * BSS on channel 12 when radio is configured for
			 * channel 11) or beyond the 6 GHz channel range.
			 */
			return NULL;
		}

		/* No match for the payload channel number - ignore it */
		return channel;
	}

	/*
	 * Use the channel determined through the payload channel number
	 * instead of the RX channel reported by the driver.
	 */
	if (alt_channel->flags & IEEE80211_CHAN_DISABLED)
		return NULL;
	return alt_channel;
}

struct cfg80211_inform_single_bss_data {
	struct cfg80211_inform_bss *drv_data;
	enum cfg80211_bss_frame_type ftype;
	struct ieee80211_channel *channel;
	u8 bssid[ETH_ALEN];
	u64 tsf;
	u16 capability;
	u16 beacon_interval;
	const u8 *ie;
	size_t ielen;

	enum {
		BSS_SOURCE_DIRECT = 0,
		BSS_SOURCE_MBSSID,
		BSS_SOURCE_STA_PROFILE,
	} bss_source;
	/* Set if reporting bss_source != BSS_SOURCE_DIRECT */
	struct cfg80211_bss *source_bss;
	u8 max_bssid_indicator;
	u8 bssid_index;

	u8 use_for;
	u64 cannot_use_reasons;
};

/* Returned bss is reference counted and must be cleaned up appropriately. */
static struct cfg80211_bss *
cfg80211_inform_single_bss_data(struct wiphy *wiphy,
				struct cfg80211_inform_single_bss_data *data,
				gfp_t gfp)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);
	struct cfg80211_inform_bss *drv_data = data->drv_data;
	struct cfg80211_bss_ies *ies;
	struct ieee80211_channel *channel;
	struct cfg80211_internal_bss tmp = {}, *res;
	int bss_type;
	bool signal_valid;
	unsigned long ts;

	if (WARN_ON(!wiphy))
		return NULL;

	if (WARN_ON(wiphy->signal_type == CFG80211_SIGNAL_TYPE_UNSPEC &&
		    (drv_data->signal < 0 || drv_data->signal > 100)))
		return NULL;

	if (WARN_ON(data->bss_source != BSS_SOURCE_DIRECT && !data->source_bss))
		return NULL;

	channel = data->channel;
	if (!channel)
		channel = cfg80211_get_bss_channel(wiphy, data->ie, data->ielen,
						   drv_data->chan);
	if (!channel)
		return NULL;

	memcpy(tmp.pub.bssid, data->bssid, ETH_ALEN);
	tmp.pub.channel = channel;
	if (data->bss_source != BSS_SOURCE_STA_PROFILE)
		tmp.pub.signal = drv_data->signal;
	else
		tmp.pub.signal = 0;
	tmp.pub.beacon_interval = data->beacon_interval;
	tmp.pub.capability = data->capability;
	tmp.ts_boottime = drv_data->boottime_ns;
	tmp.parent_tsf = drv_data->parent_tsf;
	ether_addr_copy(tmp.parent_bssid, drv_data->parent_bssid);
	tmp.pub.use_for = data->use_for;
	tmp.pub.cannot_use_reasons = data->cannot_use_reasons;

	if (data->bss_source != BSS_SOURCE_DIRECT) {
		tmp.pub.transmitted_bss = data->source_bss;
		ts = bss_from_pub(data->source_bss)->ts;
		tmp.pub.bssid_index = data->bssid_index;
		tmp.pub.max_bssid_indicator = data->max_bssid_indicator;
	} else {
		ts = jiffies;

		if (channel->band == NL80211_BAND_60GHZ) {
			bss_type = data->capability &
				   WLAN_CAPABILITY_DMG_TYPE_MASK;
			if (bss_type == WLAN_CAPABILITY_DMG_TYPE_AP ||
			    bss_type == WLAN_CAPABILITY_DMG_TYPE_PBSS)
				regulatory_hint_found_beacon(wiphy, channel,
							     gfp);
		} else {
			if (data->capability & WLAN_CAPABILITY_ESS)
				regulatory_hint_found_beacon(wiphy, channel,
							     gfp);
		}
	}

	/*
	 * If we do not know here whether the IEs are from a Beacon or Probe
	 * Response frame, we need to pick one of the options and only use it
	 * with the driver that does not provide the full Beacon/Probe Response
	 * frame. Use Beacon frame pointer to avoid indicating that this should
	 * override the IEs pointer should we have received an earlier
	 * indication of Probe Response data.
	 */
	ies = kzalloc(sizeof(*ies) + data->ielen, gfp);
	if (!ies)
		return NULL;
	ies->len = data->ielen;
	ies->tsf = data->tsf;
	ies->from_beacon = false;
	memcpy(ies->data, data->ie, data->ielen);

	switch (data->ftype) {
	case CFG80211_BSS_FTYPE_BEACON:
		ies->from_beacon = true;
		fallthrough;
	case CFG80211_BSS_FTYPE_UNKNOWN:
		rcu_assign_pointer(tmp.pub.beacon_ies, ies);
		break;
	case CFG80211_BSS_FTYPE_PRESP:
		rcu_assign_pointer(tmp.pub.proberesp_ies, ies);
		break;
	}
	rcu_assign_pointer(tmp.pub.ies, ies);

	signal_valid = drv_data->chan == channel;
	spin_lock_bh(&rdev->bss_lock);
	res = __cfg80211_bss_update(rdev, &tmp, signal_valid, ts);
	if (!res)
		goto drop;

	rdev_inform_bss(rdev, &res->pub, ies, drv_data->drv_data);

	if (data->bss_source == BSS_SOURCE_MBSSID) {
		/* this is a nontransmitting bss, we need to add it to
		 * transmitting bss' list if it is not there
		 */
		if (cfg80211_add_nontrans_list(data->source_bss, &res->pub)) {
			if (__cfg80211_unlink_bss(rdev, res)) {
				rdev->bss_generation++;
				res = NULL;
			}
		}

		if (!res)
			goto drop;
	}
	spin_unlock_bh(&rdev->bss_lock);

	trace_cfg80211_return_bss(&res->pub);
	/* __cfg80211_bss_update gives us a referenced result */
	return &res->pub;

drop:
	spin_unlock_bh(&rdev->bss_lock);
	return NULL;
}

static const struct element
*cfg80211_get_profile_continuation(const u8 *ie, size_t ielen,
				   const struct element *mbssid_elem,
				   const struct element *sub_elem)
{
	const u8 *mbssid_end = mbssid_elem->data + mbssid_elem->datalen;
	const struct element *next_mbssid;
	const struct element *next_sub;

	next_mbssid = cfg80211_find_elem(WLAN_EID_MULTIPLE_BSSID,
					 mbssid_end,
					 ielen - (mbssid_end - ie));

	/*
	 * If it is not the last subelement in current MBSSID IE or there isn't
	 * a next MBSSID IE - profile is complete.
	*/
	if ((sub_elem->data + sub_elem->datalen < mbssid_end - 1) ||
	    !next_mbssid)
		return NULL;

	/* For any length error, just return NULL */

	if (next_mbssid->datalen < 4)
		return NULL;

	next_sub = (void *)&next_mbssid->data[1];

	if (next_mbssid->data + next_mbssid->datalen <
	    next_sub->data + next_sub->datalen)
		return NULL;

	if (next_sub->id != 0 || next_sub->datalen < 2)
		return NULL;

	/*
	 * Check if the first element in the next sub element is a start
	 * of a new profile
	 */
	return next_sub->data[0] == WLAN_EID_NON_TX_BSSID_CAP ?
	       NULL : next_mbssid;
}

size_t cfg80211_merge_profile(const u8 *ie, size_t ielen,
			      const struct element *mbssid_elem,
			      const struct element *sub_elem,
			      u8 *merged_ie, size_t max_copy_len)
{
	size_t copied_len = sub_elem->datalen;
	const struct element *next_mbssid;

	if (sub_elem->datalen > max_copy_len)
		return 0;

	memcpy(merged_ie, sub_elem->data, sub_elem->datalen);

	while ((next_mbssid = cfg80211_get_profile_continuation(ie, ielen,
								mbssid_elem,
								sub_elem))) {
		const struct element *next_sub = (void *)&next_mbssid->data[1];

		if (copied_len + next_sub->datalen > max_copy_len)
			break;
		memcpy(merged_ie + copied_len, next_sub->data,
		       next_sub->datalen);
		copied_len += next_sub->datalen;
	}

	return copied_len;
}
EXPORT_SYMBOL(cfg80211_merge_profile);

static void
cfg80211_parse_mbssid_data(struct wiphy *wiphy,
			   struct cfg80211_inform_single_bss_data *tx_data,
			   struct cfg80211_bss *source_bss,
			   gfp_t gfp)
{
	struct cfg80211_inform_single_bss_data data = {
		.drv_data = tx_data->drv_data,
		.ftype = tx_data->ftype,
		.tsf = tx_data->tsf,
		.beacon_interval = tx_data->beacon_interval,
		.source_bss = source_bss,
		.bss_source = BSS_SOURCE_MBSSID,
		.use_for = tx_data->use_for,
		.cannot_use_reasons = tx_data->cannot_use_reasons,
	};
	const u8 *mbssid_index_ie;
	const struct element *elem, *sub;
	u8 *new_ie, *profile;
	u64 seen_indices = 0;
	struct cfg80211_bss *bss;

	if (!source_bss)
		return;
	if (!cfg80211_find_elem(WLAN_EID_MULTIPLE_BSSID,
				tx_data->ie, tx_data->ielen))
		return;
	if (!wiphy->support_mbssid)
		return;
	if (wiphy->support_only_he_mbssid &&
	    !cfg80211_find_ext_elem(WLAN_EID_EXT_HE_CAPABILITY,
				    tx_data->ie, tx_data->ielen))
		return;

	new_ie = kmalloc(IEEE80211_MAX_DATA_LEN, gfp);
	if (!new_ie)
		return;

	profile = kmalloc(tx_data->ielen, gfp);
	if (!profile)
		goto out;

	for_each_element_id(elem, WLAN_EID_MULTIPLE_BSSID,
			    tx_data->ie, tx_data->ielen) {
		if (elem->datalen < 4)
			continue;
		if (elem->data[0] < 1 || (int)elem->data[0] > 8)
			continue;
		for_each_element(sub, elem->data + 1, elem->datalen - 1) {
			u8 profile_len;

			if (sub->id != 0 || sub->datalen < 4) {
				/* not a valid BSS profile */
				continue;
			}

			if (sub->data[0] != WLAN_EID_NON_TX_BSSID_CAP ||
			    sub->data[1] != 2) {
				/* The first element within the Nontransmitted
				 * BSSID Profile is not the Nontransmitted
				 * BSSID Capability element.
				 */
				continue;
			}

			memset(profile, 0, tx_data->ielen);
			profile_len = cfg80211_merge_profile(tx_data->ie,
							     tx_data->ielen,
							     elem,
							     sub,
							     profile,
							     tx_data->ielen);

			/* found a Nontransmitted BSSID Profile */
			mbssid_index_ie = cfg80211_find_ie
				(WLAN_EID_MULTI_BSSID_IDX,
				 profile, profile_len);
			if (!mbssid_index_ie || mbssid_index_ie[1] < 1 ||
			    mbssid_index_ie[2] == 0 ||
			    mbssid_index_ie[2] > 46) {
				/* No valid Multiple BSSID-Index element */
				continue;
			}

			if (seen_indices & BIT_ULL(mbssid_index_ie[2]))
				/* We don't support legacy split of a profile */
				net_dbg_ratelimited("Partial info for BSSID index %d\n",
						    mbssid_index_ie[2]);

			seen_indices |= BIT_ULL(mbssid_index_ie[2]);

			data.bssid_index = mbssid_index_ie[2];
			data.max_bssid_indicator = elem->data[0];

			cfg80211_gen_new_bssid(tx_data->bssid,
					       data.max_bssid_indicator,
					       data.bssid_index,
					       data.bssid);

			memset(new_ie, 0, IEEE80211_MAX_DATA_LEN);
			data.ie = new_ie;
			data.ielen = cfg80211_gen_new_ie(tx_data->ie,
							 tx_data->ielen,
							 profile,
							 profile_len,
							 new_ie,
							 IEEE80211_MAX_DATA_LEN);
			if (!data.ielen)
				continue;

			data.capability = get_unaligned_le16(profile + 2);
			bss = cfg80211_inform_single_bss_data(wiphy, &data, gfp);
			if (!bss)
				break;
			cfg80211_put_bss(wiphy, bss);
		}
	}

out:
	kfree(new_ie);
	kfree(profile);
}

ssize_t cfg80211_defragment_element(const struct element *elem, const u8 *ies,
				    size_t ieslen, u8 *data, size_t data_len,
				    u8 frag_id)
{
	const struct element *next;
	ssize_t copied;
	u8 elem_datalen;

	if (!elem)
		return -EINVAL;

	/* elem might be invalid after the memmove */
	next = (void *)(elem->data + elem->datalen);
	elem_datalen = elem->datalen;

	if (elem->id == WLAN_EID_EXTENSION) {
		copied = elem->datalen - 1;
		if (copied > data_len)
			return -ENOSPC;

		memmove(data, elem->data + 1, copied);
	} else {
		copied = elem->datalen;
		if (copied > data_len)
			return -ENOSPC;

		memmove(data, elem->data, copied);
	}

	/* Fragmented elements must have 255 bytes */
	if (elem_datalen < 255)
		return copied;

	for (elem = next;
	     elem->data < ies + ieslen &&
		elem->data + elem->datalen <= ies + ieslen;
	     elem = next) {
		/* elem might be invalid after the memmove */
		next = (void *)(elem->data + elem->datalen);

		if (elem->id != frag_id)
			break;

		elem_datalen = elem->datalen;

		if (copied + elem_datalen > data_len)
			return -ENOSPC;

		memmove(data + copied, elem->data, elem_datalen);
		copied += elem_datalen;

		/* Only the last fragment may be short */
		if (elem_datalen != 255)
			break;
	}

	return copied;
}
EXPORT_SYMBOL(cfg80211_defragment_element);

struct cfg80211_mle {
	struct ieee80211_multi_link_elem *mle;
	struct ieee80211_mle_per_sta_profile
		*sta_prof[IEEE80211_MLD_MAX_NUM_LINKS];
	ssize_t sta_prof_len[IEEE80211_MLD_MAX_NUM_LINKS];

	u8 data[];
};

static struct cfg80211_mle *
cfg80211_defrag_mle(const struct element *mle, const u8 *ie, size_t ielen,
		    gfp_t gfp)
{
	const struct element *elem;
	struct cfg80211_mle *res;
	size_t buf_len;
	ssize_t mle_len;
	u8 common_size, idx;

	if (!mle || !ieee80211_mle_size_ok(mle->data + 1, mle->datalen - 1))
		return NULL;

	/* Required length for first defragmentation */
	buf_len = mle->datalen - 1;
	for_each_element(elem, mle->data + mle->datalen,
			 ielen - sizeof(*mle) + mle->datalen) {
		if (elem->id != WLAN_EID_FRAGMENT)
			break;

		buf_len += elem->datalen;
	}

	res = kzalloc(struct_size(res, data, buf_len), gfp);
	if (!res)
		return NULL;

	mle_len = cfg80211_defragment_element(mle, ie, ielen,
					      res->data, buf_len,
					      WLAN_EID_FRAGMENT);
	if (mle_len < 0)
		goto error;

	res->mle = (void *)res->data;

	/* Find the sub-element area in the buffer */
	common_size = ieee80211_mle_common_size((u8 *)res->mle);
	ie = res->data + common_size;
	ielen = mle_len - common_size;

	idx = 0;
	for_each_element_id(elem, IEEE80211_MLE_SUBELEM_PER_STA_PROFILE,
			    ie, ielen) {
		res->sta_prof[idx] = (void *)elem->data;
		res->sta_prof_len[idx] = elem->datalen;

		idx++;
		if (idx >= IEEE80211_MLD_MAX_NUM_LINKS)
			break;
	}
	if (!for_each_element_completed(elem, ie, ielen))
		goto error;

	/* Defragment sta_info in-place */
	for (idx = 0; idx < IEEE80211_MLD_MAX_NUM_LINKS && res->sta_prof[idx];
	     idx++) {
		if (res->sta_prof_len[idx] < 255)
			continue;

		elem = (void *)res->sta_prof[idx] - 2;

		if (idx + 1 < ARRAY_SIZE(res->sta_prof) &&
		    res->sta_prof[idx + 1])
			buf_len = (u8 *)res->sta_prof[idx + 1] -
				  (u8 *)res->sta_prof[idx];
		else
			buf_len = ielen + ie - (u8 *)elem;

		res->sta_prof_len[idx] =
			cfg80211_defragment_element(elem,
						    (u8 *)elem, buf_len,
						    (u8 *)res->sta_prof[idx],
						    buf_len,
						    IEEE80211_MLE_SUBELEM_FRAGMENT);
		if (res->sta_prof_len[idx] < 0)
			goto error;
	}

	return res;

error:
	kfree(res);
	return NULL;
}

static u8
cfg80211_rnr_info_for_mld_ap(const u8 *ie, size_t ielen, u8 mld_id, u8 link_id,
			     const struct ieee80211_neighbor_ap_info **ap_info,
			     u8 *param_ch_count)
{
	const struct ieee80211_neighbor_ap_info *info;
	const struct element *rnr;
	const u8 *pos, *end;

	for_each_element_id(rnr, WLAN_EID_REDUCED_NEIGHBOR_REPORT, ie, ielen) {
		pos = rnr->data;
		end = rnr->data + rnr->datalen;

		/* RNR IE may contain more than one NEIGHBOR_AP_INFO */
		while (sizeof(*info) <= end - pos) {
			const struct ieee80211_rnr_mld_params *mld_params;
			u16 params;
			u8 length, i, count, mld_params_offset;
			u8 type, lid;
			u32 use_for;

			info = (void *)pos;
			count = u8_get_bits(info->tbtt_info_hdr,
					    IEEE80211_AP_INFO_TBTT_HDR_COUNT) + 1;
			length = info->tbtt_info_len;

			pos += sizeof(*info);

			if (count * length > end - pos)
				return 0;

			type = u8_get_bits(info->tbtt_info_hdr,
					   IEEE80211_AP_INFO_TBTT_HDR_TYPE);

			if (type == IEEE80211_TBTT_INFO_TYPE_TBTT &&
			    length >=
			    offsetofend(struct ieee80211_tbtt_info_ge_11,
					mld_params)) {
				mld_params_offset =
					offsetof(struct ieee80211_tbtt_info_ge_11, mld_params);
				use_for = NL80211_BSS_USE_FOR_ALL;
			} else if (type == IEEE80211_TBTT_INFO_TYPE_MLD &&
				   length >= sizeof(struct ieee80211_rnr_mld_params)) {
				mld_params_offset = 0;
				use_for = NL80211_BSS_USE_FOR_MLD_LINK;
			} else {
				pos += count * length;
				continue;
			}

			for (i = 0; i < count; i++) {
				mld_params = (void *)pos + mld_params_offset;
				params = le16_to_cpu(mld_params->params);

				lid = u16_get_bits(params,
						   IEEE80211_RNR_MLD_PARAMS_LINK_ID);

				if (mld_id == mld_params->mld_id &&
				    link_id == lid) {
					*ap_info = info;
					*param_ch_count =
						le16_get_bits(mld_params->params,
							      IEEE80211_RNR_MLD_PARAMS_BSS_CHANGE_COUNT);

					return use_for;
				}

				pos += length;
			}
		}
	}

	return 0;
}

static struct element *
cfg80211_gen_reporter_rnr(struct cfg80211_bss *source_bss, bool is_mbssid,
			  bool same_mld, u8 link_id, u8 bss_change_count,
			  gfp_t gfp)
{
	const struct cfg80211_bss_ies *ies;
	struct ieee80211_neighbor_ap_info ap_info;
	struct ieee80211_tbtt_info_ge_11 tbtt_info;
	u32 short_ssid;
	const struct element *elem;
	struct element *res;

	/*
	 * We only generate the RNR to permit ML lookups. For that we do not
	 * need an entry for the corresponding transmitting BSS, lets just skip
	 * it even though it would be easy to add.
	 */
	if (!same_mld)
		return NULL;

	/* We could use tx_data->ies if we change cfg80211_calc_short_ssid */
	rcu_read_lock();
	ies = rcu_dereference(source_bss->ies);

	ap_info.tbtt_info_len = offsetofend(typeof(tbtt_info), mld_params);
	ap_info.tbtt_info_hdr =
			u8_encode_bits(IEEE80211_TBTT_INFO_TYPE_TBTT,
				       IEEE80211_AP_INFO_TBTT_HDR_TYPE) |
			u8_encode_bits(0, IEEE80211_AP_INFO_TBTT_HDR_COUNT);

	ap_info.channel = ieee80211_frequency_to_channel(source_bss->channel->center_freq);

	/* operating class */
	elem = cfg80211_find_elem(WLAN_EID_SUPPORTED_REGULATORY_CLASSES,
				  ies->data, ies->len);
	if (elem && elem->datalen >= 1) {
		ap_info.op_class = elem->data[0];
	} else {
		struct cfg80211_chan_def chandef;

		/* The AP is not providing us with anything to work with. So
		 * make up a somewhat reasonable operating class, but don't
		 * bother with it too much as no one will ever use the
		 * information.
		 */
		cfg80211_chandef_create(&chandef, source_bss->channel,
					NL80211_CHAN_NO_HT);

		if (!ieee80211_chandef_to_operating_class(&chandef,
							  &ap_info.op_class))
			goto out_unlock;
	}

	/* Just set TBTT offset and PSD 20 to invalid/unknown */
	tbtt_info.tbtt_offset = 255;
	tbtt_info.psd_20 = IEEE80211_RNR_TBTT_PARAMS_PSD_RESERVED;

	memcpy(tbtt_info.bssid, source_bss->bssid, ETH_ALEN);
	if (cfg80211_calc_short_ssid(ies, &elem, &short_ssid))
		goto out_unlock;

	rcu_read_unlock();

	tbtt_info.short_ssid = cpu_to_le32(short_ssid);

	tbtt_info.bss_params = IEEE80211_RNR_TBTT_PARAMS_SAME_SSID;

	if (is_mbssid) {
		tbtt_info.bss_params |= IEEE80211_RNR_TBTT_PARAMS_MULTI_BSSID;
		tbtt_info.bss_params |= IEEE80211_RNR_TBTT_PARAMS_TRANSMITTED_BSSID;
	}

	tbtt_info.mld_params.mld_id = 0;
	tbtt_info.mld_params.params =
		le16_encode_bits(link_id, IEEE80211_RNR_MLD_PARAMS_LINK_ID) |
		le16_encode_bits(bss_change_count,
				 IEEE80211_RNR_MLD_PARAMS_BSS_CHANGE_COUNT);

	res = kzalloc(struct_size(res, data,
				  sizeof(ap_info) + ap_info.tbtt_info_len),
		      gfp);
	if (!res)
		return NULL;

	/* Copy the data */
	res->id = WLAN_EID_REDUCED_NEIGHBOR_REPORT;
	res->datalen = sizeof(ap_info) + ap_info.tbtt_info_len;
	memcpy(res->data, &ap_info, sizeof(ap_info));
	memcpy(res->data + sizeof(ap_info), &tbtt_info, ap_info.tbtt_info_len);

	return res;

out_unlock:
	rcu_read_unlock();
	return NULL;
}

static void
cfg80211_parse_ml_elem_sta_data(struct wiphy *wiphy,
				struct cfg80211_inform_single_bss_data *tx_data,
				struct cfg80211_bss *source_bss,
				const struct element *elem,
				gfp_t gfp)
{
	struct cfg80211_inform_single_bss_data data = {
		.drv_data = tx_data->drv_data,
		.ftype = tx_data->ftype,
		.source_bss = source_bss,
		.bss_source = BSS_SOURCE_STA_PROFILE,
	};
	struct element *reporter_rnr = NULL;
	struct ieee80211_multi_link_elem *ml_elem;
	struct cfg80211_mle *mle;
	u16 control;
	u8 ml_common_len;
	u8 *new_ie = NULL;
	struct cfg80211_bss *bss;
	u8 mld_id, reporter_link_id, bss_change_count;
	u16 seen_links = 0;
	const u8 *pos;
	u8 i;

	if (!ieee80211_mle_size_ok(elem->data + 1, elem->datalen - 1))
		return;

	ml_elem = (void *)elem->data + 1;
	control = le16_to_cpu(ml_elem->control);
	if (u16_get_bits(control, IEEE80211_ML_CONTROL_TYPE) !=
	    IEEE80211_ML_CONTROL_TYPE_BASIC)
		return;

	/* Must be present when transmitted by an AP (in a probe response) */
	if (!(control & IEEE80211_MLC_BASIC_PRES_BSS_PARAM_CH_CNT) ||
	    !(control & IEEE80211_MLC_BASIC_PRES_LINK_ID) ||
	    !(control & IEEE80211_MLC_BASIC_PRES_MLD_CAPA_OP))
		return;

	ml_common_len = ml_elem->variable[0];

	/* length + MLD MAC address */
	pos = ml_elem->variable + 1 + 6;

	reporter_link_id = pos[0];
	pos += 1;

	bss_change_count = pos[0];
	pos += 1;

	if (u16_get_bits(control, IEEE80211_MLC_BASIC_PRES_MED_SYNC_DELAY))
		pos += 2;
	if (u16_get_bits(control, IEEE80211_MLC_BASIC_PRES_EML_CAPA))
		pos += 2;

	/* MLD capabilities and operations */
	pos += 2;

	/*
	 * The MLD ID of the reporting AP is always zero. It is set if the AP
	 * is part of an MBSSID set and will be non-zero for ML Elements
	 * relating to a nontransmitted BSS (matching the Multi-BSSID Index,
	 * Draft P802.11be_D3.2, 35.3.4.2)
	 */
	if (u16_get_bits(control, IEEE80211_MLC_BASIC_PRES_MLD_ID)) {
		mld_id = *pos;
		pos += 1;
	} else {
		mld_id = 0;
	}

	/* Extended MLD capabilities and operations */
	pos += 2;

	/* Fully defrag the ML element for sta information/profile iteration */
	mle = cfg80211_defrag_mle(elem, tx_data->ie, tx_data->ielen, gfp);
	if (!mle)
		return;

	/* No point in doing anything if there is no per-STA profile */
	if (!mle->sta_prof[0])
		goto out;

	new_ie = kmalloc(IEEE80211_MAX_DATA_LEN, gfp);
	if (!new_ie)
		goto out;

	reporter_rnr = cfg80211_gen_reporter_rnr(source_bss,
						 u16_get_bits(control,
							      IEEE80211_MLC_BASIC_PRES_MLD_ID),
						 mld_id == 0, reporter_link_id,
						 bss_change_count,
						 gfp);

	for (i = 0; i < ARRAY_SIZE(mle->sta_prof) && mle->sta_prof[i]; i++) {
		const struct ieee80211_neighbor_ap_info *ap_info;
		enum nl80211_band band;
		u32 freq;
		const u8 *profile;
		ssize_t profile_len;
		u8 param_ch_count;
		u8 link_id, use_for;

		if (!ieee80211_mle_basic_sta_prof_size_ok((u8 *)mle->sta_prof[i],
							  mle->sta_prof_len[i]))
			continue;

		control = le16_to_cpu(mle->sta_prof[i]->control);

		if (!(control & IEEE80211_MLE_STA_CONTROL_COMPLETE_PROFILE))
			continue;

		link_id = u16_get_bits(control,
				       IEEE80211_MLE_STA_CONTROL_LINK_ID);
		if (seen_links & BIT(link_id))
			break;
		seen_links |= BIT(link_id);

		if (!(control & IEEE80211_MLE_STA_CONTROL_BEACON_INT_PRESENT) ||
		    !(control & IEEE80211_MLE_STA_CONTROL_TSF_OFFS_PRESENT) ||
		    !(control & IEEE80211_MLE_STA_CONTROL_STA_MAC_ADDR_PRESENT))
			continue;

		memcpy(data.bssid, mle->sta_prof[i]->variable, ETH_ALEN);
		data.beacon_interval =
			get_unaligned_le16(mle->sta_prof[i]->variable + 6);
		data.tsf = tx_data->tsf +
			   get_unaligned_le64(mle->sta_prof[i]->variable + 8);

		/* sta_info_len counts itself */
		profile = mle->sta_prof[i]->variable +
			  mle->sta_prof[i]->sta_info_len - 1;
		profile_len = (u8 *)mle->sta_prof[i] + mle->sta_prof_len[i] -
			      profile;

		if (profile_len < 2)
			continue;

		data.capability = get_unaligned_le16(profile);
		profile += 2;
		profile_len -= 2;

		/* Find in RNR to look up channel information */
		use_for = cfg80211_rnr_info_for_mld_ap(tx_data->ie,
						       tx_data->ielen,
						       mld_id, link_id,
						       &ap_info,
						       &param_ch_count);
		if (!use_for)
			continue;

		/* We could sanity check the BSSID is included */

		if (!ieee80211_operating_class_to_band(ap_info->op_class,
						       &band))
			continue;

		freq = ieee80211_channel_to_freq_khz(ap_info->channel, band);
		data.channel = ieee80211_get_channel_khz(wiphy, freq);

		if (use_for == NL80211_BSS_USE_FOR_MLD_LINK &&
		    !(wiphy->flags & WIPHY_FLAG_SUPPORTS_NSTR_NONPRIMARY)) {
			use_for = 0;
			data.cannot_use_reasons =
				NL80211_BSS_CANNOT_USE_NSTR_NONPRIMARY;
		}
		data.use_for = use_for;

		/* Generate new elements */
		memset(new_ie, 0, IEEE80211_MAX_DATA_LEN);
		data.ie = new_ie;
		data.ielen = cfg80211_gen_new_ie(tx_data->ie, tx_data->ielen,
						 profile, profile_len,
						 new_ie,
						 IEEE80211_MAX_DATA_LEN);
		if (!data.ielen)
			continue;

		/* The generated elements do not contain:
		 *  - Basic ML element
		 *  - A TBTT entry in the RNR for the transmitting AP
		 *
		 * This information is needed both internally and in userspace
		 * as such, we should append it here.
		 */
		if (data.ielen + 3 + sizeof(*ml_elem) + ml_common_len >
		    IEEE80211_MAX_DATA_LEN)
			continue;

		/* Copy the Basic Multi-Link element including the common
		 * information, and then fix up the link ID and BSS param
		 * change count.
		 * Note that the ML element length has been verified and we
		 * also checked that it contains the link ID.
		 */
		new_ie[data.ielen++] = WLAN_EID_EXTENSION;
		new_ie[data.ielen++] = 1 + sizeof(*ml_elem) + ml_common_len;
		new_ie[data.ielen++] = WLAN_EID_EXT_EHT_MULTI_LINK;
		memcpy(new_ie + data.ielen, ml_elem,
		       sizeof(*ml_elem) + ml_common_len);

		new_ie[data.ielen + sizeof(*ml_elem) + 1 + ETH_ALEN] = link_id;
		new_ie[data.ielen + sizeof(*ml_elem) + 1 + ETH_ALEN + 1] =
			param_ch_count;

		data.ielen += sizeof(*ml_elem) + ml_common_len;

		if (reporter_rnr && (use_for & NL80211_BSS_USE_FOR_NORMAL)) {
			if (data.ielen + sizeof(struct element) +
			    reporter_rnr->datalen > IEEE80211_MAX_DATA_LEN)
				continue;

			memcpy(new_ie + data.ielen, reporter_rnr,
			       sizeof(struct element) + reporter_rnr->datalen);
			data.ielen += sizeof(struct element) +
				      reporter_rnr->datalen;
		}

		bss = cfg80211_inform_single_bss_data(wiphy, &data, gfp);
		if (!bss)
			break;
		cfg80211_put_bss(wiphy, bss);
	}

out:
	kfree(reporter_rnr);
	kfree(new_ie);
	kfree(mle);
}

static void cfg80211_parse_ml_sta_data(struct wiphy *wiphy,
				       struct cfg80211_inform_single_bss_data *tx_data,
				       struct cfg80211_bss *source_bss,
				       gfp_t gfp)
{
	const struct element *elem;

	if (!source_bss)
		return;

	if (tx_data->ftype != CFG80211_BSS_FTYPE_PRESP)
		return;

	for_each_element_extid(elem, WLAN_EID_EXT_EHT_MULTI_LINK,
			       tx_data->ie, tx_data->ielen)
		cfg80211_parse_ml_elem_sta_data(wiphy, tx_data, source_bss,
						elem, gfp);
}

struct cfg80211_bss *
cfg80211_inform_bss_data(struct wiphy *wiphy,
			 struct cfg80211_inform_bss *data,
			 enum cfg80211_bss_frame_type ftype,
			 const u8 *bssid, u64 tsf, u16 capability,
			 u16 beacon_interval, const u8 *ie, size_t ielen,
			 gfp_t gfp)
{
	struct cfg80211_inform_single_bss_data inform_data = {
		.drv_data = data,
		.ftype = ftype,
		.tsf = tsf,
		.capability = capability,
		.beacon_interval = beacon_interval,
		.ie = ie,
		.ielen = ielen,
		.use_for = data->restrict_use ?
				data->use_for :
				NL80211_BSS_USE_FOR_ALL,
		.cannot_use_reasons = data->cannot_use_reasons,
	};
	struct cfg80211_bss *res;

	memcpy(inform_data.bssid, bssid, ETH_ALEN);

	res = cfg80211_inform_single_bss_data(wiphy, &inform_data, gfp);
	if (!res)
		return NULL;

	cfg80211_parse_mbssid_data(wiphy, &inform_data, res, gfp);

	cfg80211_parse_ml_sta_data(wiphy, &inform_data, res, gfp);

	return res;
}
EXPORT_SYMBOL(cfg80211_inform_bss_data);

static bool cfg80211_uhb_power_type_valid(const u8 *ie,
					  size_t ielen,
					  const u32 flags)
{
	const struct element *tmp;
	struct ieee80211_he_operation *he_oper;

	tmp = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_OPERATION, ie, ielen);
	if (tmp && tmp->datalen >= sizeof(*he_oper) + 1 &&
	    tmp->datalen >= ieee80211_he_oper_size(tmp->data + 1)) {
		const struct ieee80211_he_6ghz_oper *he_6ghz_oper;

		he_oper = (void *)&tmp->data[1];
		he_6ghz_oper = ieee80211_he_6ghz_oper(he_oper);

		if (!he_6ghz_oper)
			return false;

		switch (u8_get_bits(he_6ghz_oper->control,
				    IEEE80211_HE_6GHZ_OPER_CTRL_REG_INFO)) {
		case IEEE80211_6GHZ_CTRL_REG_LPI_AP:
			return true;
		case IEEE80211_6GHZ_CTRL_REG_SP_AP:
			return !(flags & IEEE80211_CHAN_NO_UHB_AFC_CLIENT);
		case IEEE80211_6GHZ_CTRL_REG_VLP_AP:
			return !(flags & IEEE80211_CHAN_NO_UHB_VLP_CLIENT);
		}
	}
	return false;
}

/* cfg80211_inform_bss_width_frame helper */
static struct cfg80211_bss *
cfg80211_inform_single_bss_frame_data(struct wiphy *wiphy,
				      struct cfg80211_inform_bss *data,
				      struct ieee80211_mgmt *mgmt, size_t len,
				      gfp_t gfp)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);
	struct cfg80211_internal_bss tmp = {}, *res;
	struct cfg80211_bss_ies *ies;
	struct ieee80211_channel *channel;
	bool signal_valid;
	struct ieee80211_ext *ext = NULL;
	u8 *bssid, *variable;
	u16 capability, beacon_int;
	size_t ielen, min_hdr_len = offsetof(struct ieee80211_mgmt,
					     u.probe_resp.variable);
	int bss_type;

	BUILD_BUG_ON(offsetof(struct ieee80211_mgmt, u.probe_resp.variable) !=
			offsetof(struct ieee80211_mgmt, u.beacon.variable));

	trace_cfg80211_inform_bss_frame(wiphy, data, mgmt, len);

	if (WARN_ON(!mgmt))
		return NULL;

	if (WARN_ON(!wiphy))
		return NULL;

	if (WARN_ON(wiphy->signal_type == CFG80211_SIGNAL_TYPE_UNSPEC &&
		    (data->signal < 0 || data->signal > 100)))
		return NULL;

	if (ieee80211_is_s1g_beacon(mgmt->frame_control)) {
		ext = (void *) mgmt;
		min_hdr_len = offsetof(struct ieee80211_ext, u.s1g_beacon);
		if (ieee80211_is_s1g_short_beacon(mgmt->frame_control))
			min_hdr_len = offsetof(struct ieee80211_ext,
					       u.s1g_short_beacon.variable);
	}

	if (WARN_ON(len < min_hdr_len))
		return NULL;

	ielen = len - min_hdr_len;
	variable = mgmt->u.probe_resp.variable;
	if (ext) {
		if (ieee80211_is_s1g_short_beacon(mgmt->frame_control))
			variable = ext->u.s1g_short_beacon.variable;
		else
			variable = ext->u.s1g_beacon.variable;
	}

	channel = cfg80211_get_bss_channel(wiphy, variable, ielen, data->chan);
	if (!channel)
		return NULL;

	if (channel->band == NL80211_BAND_6GHZ &&
	    !cfg80211_uhb_power_type_valid(variable, ielen, channel->flags)) {
		data->restrict_use = 1;
		data->use_for = 0;
		data->cannot_use_reasons =
			NL80211_BSS_CANNOT_USE_UHB_PWR_MISMATCH;
	}

	if (ext) {
		const struct ieee80211_s1g_bcn_compat_ie *compat;
		const struct element *elem;

		elem = cfg80211_find_elem(WLAN_EID_S1G_BCN_COMPAT,
					  variable, ielen);
		if (!elem)
			return NULL;
		if (elem->datalen < sizeof(*compat))
			return NULL;
		compat = (void *)elem->data;
		bssid = ext->u.s1g_beacon.sa;
		capability = le16_to_cpu(compat->compat_info);
		beacon_int = le16_to_cpu(compat->beacon_int);
	} else {
		bssid = mgmt->bssid;
		beacon_int = le16_to_cpu(mgmt->u.probe_resp.beacon_int);
		capability = le16_to_cpu(mgmt->u.probe_resp.capab_info);
	}

	if (channel->band == NL80211_BAND_60GHZ) {
		bss_type = capability & WLAN_CAPABILITY_DMG_TYPE_MASK;
		if (bss_type == WLAN_CAPABILITY_DMG_TYPE_AP ||
		    bss_type == WLAN_CAPABILITY_DMG_TYPE_PBSS)
			regulatory_hint_found_beacon(wiphy, channel, gfp);
	} else {
		if (capability & WLAN_CAPABILITY_ESS)
			regulatory_hint_found_beacon(wiphy, channel, gfp);
	}

	ies = kzalloc(sizeof(*ies) + ielen, gfp);
	if (!ies)
		return NULL;
	ies->len = ielen;
	ies->tsf = le64_to_cpu(mgmt->u.probe_resp.timestamp);
	ies->from_beacon = ieee80211_is_beacon(mgmt->frame_control) ||
			   ieee80211_is_s1g_beacon(mgmt->frame_control);
	memcpy(ies->data, variable, ielen);

	if (ieee80211_is_probe_resp(mgmt->frame_control))
		rcu_assign_pointer(tmp.pub.proberesp_ies, ies);
	else
		rcu_assign_pointer(tmp.pub.beacon_ies, ies);
	rcu_assign_pointer(tmp.pub.ies, ies);

	memcpy(tmp.pub.bssid, bssid, ETH_ALEN);
	tmp.pub.beacon_interval = beacon_int;
	tmp.pub.capability = capability;
	tmp.pub.channel = channel;
	tmp.pub.signal = data->signal;
	tmp.ts_boottime = data->boottime_ns;
	tmp.parent_tsf = data->parent_tsf;
	tmp.pub.chains = data->chains;
	memcpy(tmp.pub.chain_signal, data->chain_signal, IEEE80211_MAX_CHAINS);
	ether_addr_copy(tmp.parent_bssid, data->parent_bssid);
	tmp.pub.use_for = data->restrict_use ?
				data->use_for :
				NL80211_BSS_USE_FOR_ALL;
	tmp.pub.cannot_use_reasons = data->cannot_use_reasons;

	signal_valid = data->chan == channel;
	spin_lock_bh(&rdev->bss_lock);
	res = __cfg80211_bss_update(rdev, &tmp, signal_valid, jiffies);
	if (!res)
		goto drop;

	rdev_inform_bss(rdev, &res->pub, ies, data->drv_data);

	spin_unlock_bh(&rdev->bss_lock);

	trace_cfg80211_return_bss(&res->pub);
	/* __cfg80211_bss_update gives us a referenced result */
	return &res->pub;

drop:
	spin_unlock_bh(&rdev->bss_lock);
	return NULL;
}

struct cfg80211_bss *
cfg80211_inform_bss_frame_data(struct wiphy *wiphy,
			       struct cfg80211_inform_bss *data,
			       struct ieee80211_mgmt *mgmt, size_t len,
			       gfp_t gfp)
{
	struct cfg80211_inform_single_bss_data inform_data = {
		.drv_data = data,
		.ie = mgmt->u.probe_resp.variable,
		.ielen = len - offsetof(struct ieee80211_mgmt,
					u.probe_resp.variable),
		.use_for = data->restrict_use ?
				data->use_for :
				NL80211_BSS_USE_FOR_ALL,
		.cannot_use_reasons = data->cannot_use_reasons,
	};
	struct cfg80211_bss *res;

	res = cfg80211_inform_single_bss_frame_data(wiphy, data, mgmt,
						    len, gfp);
	if (!res)
		return NULL;

	/* don't do any further MBSSID/ML handling for S1G */
	if (ieee80211_is_s1g_beacon(mgmt->frame_control))
		return res;

	inform_data.ftype = ieee80211_is_beacon(mgmt->frame_control) ?
		CFG80211_BSS_FTYPE_BEACON : CFG80211_BSS_FTYPE_PRESP;
	memcpy(inform_data.bssid, mgmt->bssid, ETH_ALEN);
	inform_data.tsf = le64_to_cpu(mgmt->u.probe_resp.timestamp);
	inform_data.beacon_interval =
		le16_to_cpu(mgmt->u.probe_resp.beacon_int);

	/* process each non-transmitting bss */
	cfg80211_parse_mbssid_data(wiphy, &inform_data, res, gfp);

	cfg80211_parse_ml_sta_data(wiphy, &inform_data, res, gfp);

	return res;
}
EXPORT_SYMBOL(cfg80211_inform_bss_frame_data);

void cfg80211_ref_bss(struct wiphy *wiphy, struct cfg80211_bss *pub)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);

	if (!pub)
		return;

	spin_lock_bh(&rdev->bss_lock);
	bss_ref_get(rdev, bss_from_pub(pub));
	spin_unlock_bh(&rdev->bss_lock);
}
EXPORT_SYMBOL(cfg80211_ref_bss);

void cfg80211_put_bss(struct wiphy *wiphy, struct cfg80211_bss *pub)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);

	if (!pub)
		return;

	spin_lock_bh(&rdev->bss_lock);
	bss_ref_put(rdev, bss_from_pub(pub));
	spin_unlock_bh(&rdev->bss_lock);
}
EXPORT_SYMBOL(cfg80211_put_bss);

void cfg80211_unlink_bss(struct wiphy *wiphy, struct cfg80211_bss *pub)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);
	struct cfg80211_internal_bss *bss, *tmp1;
	struct cfg80211_bss *nontrans_bss, *tmp;

	if (WARN_ON(!pub))
		return;

	bss = bss_from_pub(pub);

	spin_lock_bh(&rdev->bss_lock);
	if (list_empty(&bss->list))
		goto out;

	list_for_each_entry_safe(nontrans_bss, tmp,
				 &pub->nontrans_list,
				 nontrans_list) {
		tmp1 = bss_from_pub(nontrans_bss);
		if (__cfg80211_unlink_bss(rdev, tmp1))
			rdev->bss_generation++;
	}

	if (__cfg80211_unlink_bss(rdev, bss))
		rdev->bss_generation++;
out:
	spin_unlock_bh(&rdev->bss_lock);
}
EXPORT_SYMBOL(cfg80211_unlink_bss);

void cfg80211_bss_iter(struct wiphy *wiphy,
		       struct cfg80211_chan_def *chandef,
		       void (*iter)(struct wiphy *wiphy,
				    struct cfg80211_bss *bss,
				    void *data),
		       void *iter_data)
{
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);
	struct cfg80211_internal_bss *bss;

	spin_lock_bh(&rdev->bss_lock);

	list_for_each_entry(bss, &rdev->bss_list, list) {
		if (!chandef || cfg80211_is_sub_chan(chandef, bss->pub.channel,
						     false))
			iter(wiphy, &bss->pub, iter_data);
	}

	spin_unlock_bh(&rdev->bss_lock);
}
EXPORT_SYMBOL(cfg80211_bss_iter);

void cfg80211_update_assoc_bss_entry(struct wireless_dev *wdev,
				     unsigned int link_id,
				     struct ieee80211_channel *chan)
{
	struct wiphy *wiphy = wdev->wiphy;
	struct cfg80211_registered_device *rdev = wiphy_to_rdev(wiphy);
	struct cfg80211_internal_bss *cbss = wdev->links[link_id].client.current_bss;
	struct cfg80211_internal_bss *new = NULL;
	struct cfg80211_internal_bss *bss;
	struct cfg80211_bss *nontrans_bss;
	struct cfg80211_bss *tmp;

	spin_lock_bh(&rdev->bss_lock);

	/*
	 * Some APs use CSA also for bandwidth changes, i.e., without actually
	 * changing the control channel, so no need to update in such a case.
	 */
	if (cbss->pub.channel == chan)
		goto done;

	/* use transmitting bss */
	if (cbss->pub.transmitted_bss)
		cbss = bss_from_pub(cbss->pub.transmitted_bss);

	cbss->pub.channel = chan;

	list_for_each_entry(bss, &rdev->bss_list, list) {
		if (!cfg80211_bss_type_match(bss->pub.capability,
					     bss->pub.channel->band,
					     wdev->conn_bss_type))
			continue;

		if (bss == cbss)
			continue;

		if (!cmp_bss(&bss->pub, &cbss->pub, BSS_CMP_REGULAR)) {
			new = bss;
			break;
		}
	}

	if (new) {
		/* to save time, update IEs for transmitting bss only */
		cfg80211_update_known_bss(rdev, cbss, new, false);
		new->pub.proberesp_ies = NULL;
		new->pub.beacon_ies = NULL;

		list_for_each_entry_safe(nontrans_bss, tmp,
					 &new->pub.nontrans_list,
					 nontrans_list) {
			bss = bss_from_pub(nontrans_bss);
			if (__cfg80211_unlink_bss(rdev, bss))
				rdev->bss_generation++;
		}

		WARN_ON(atomic_read(&new->hold));
		if (!WARN_ON(!__cfg80211_unlink_bss(rdev, new)))
			rdev->bss_generation++;
	}

	rb_erase(&cbss->rbn, &rdev->bss_tree);
	rb_insert_bss(rdev, cbss);
	rdev->bss_generation++;

	list_for_each_entry_safe(nontrans_bss, tmp,
				 &cbss->pub.nontrans_list,
				 nontrans_list) {
		bss = bss_from_pub(nontrans_bss);
		bss->pub.channel = chan;
		rb_erase(&bss->rbn, &rdev->bss_tree);
		rb_insert_bss(rdev, bss);
		rdev->bss_generation++;
	}

done:
	spin_unlock_bh(&rdev->bss_lock);
}

#ifdef CONFIG_CFG80211_WEXT
static struct cfg80211_registered_device *
cfg80211_get_dev_from_ifindex(struct net *net, int ifindex)
{
	struct cfg80211_registered_device *rdev;
	struct net_device *dev;

	ASSERT_RTNL();

	dev = dev_get_by_index(net, ifindex);
	if (!dev)
		return ERR_PTR(-ENODEV);
	if (dev->ieee80211_ptr)
		rdev = wiphy_to_rdev(dev->ieee80211_ptr->wiphy);
	else
		rdev = ERR_PTR(-ENODEV);
	dev_put(dev);
	return rdev;
}

int cfg80211_wext_siwscan(struct net_device *dev,
			  struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{
	struct cfg80211_registered_device *rdev;
	struct wiphy *wiphy;
	struct iw_scan_req *wreq = NULL;
	struct cfg80211_scan_request *creq;
	int i, err, n_channels = 0;
	enum nl80211_band band;

	if (!netif_running(dev))
		return -ENETDOWN;

	if (wrqu->data.length == sizeof(struct iw_scan_req))
		wreq = (struct iw_scan_req *)extra;

	rdev = cfg80211_get_dev_from_ifindex(dev_net(dev), dev->ifindex);

	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	if (rdev->scan_req || rdev->scan_msg)
		return -EBUSY;

	wiphy = &rdev->wiphy;

	/* Determine number of channels, needed to allocate creq */
	if (wreq && wreq->num_channels)
		n_channels = wreq->num_channels;
	else
		n_channels = ieee80211_get_num_supported_channels(wiphy);

	creq = kzalloc(sizeof(*creq) + sizeof(struct cfg80211_ssid) +
		       n_channels * sizeof(void *),
		       GFP_ATOMIC);
	if (!creq)
		return -ENOMEM;

	creq->wiphy = wiphy;
	creq->wdev = dev->ieee80211_ptr;
	/* SSIDs come after channels */
	creq->ssids = (void *)&creq->channels[n_channels];
	creq->n_channels = n_channels;
	creq->n_ssids = 1;
	creq->scan_start = jiffies;

	/* translate "Scan on frequencies" request */
	i = 0;
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		int j;

		if (!wiphy->bands[band])
			continue;

		for (j = 0; j < wiphy->bands[band]->n_channels; j++) {
			/* ignore disabled channels */
			if (wiphy->bands[band]->channels[j].flags &
						IEEE80211_CHAN_DISABLED)
				continue;

			/* If we have a wireless request structure and the
			 * wireless request specifies frequencies, then search
			 * for the matching hardware channel.
			 */
			if (wreq && wreq->num_channels) {
				int k;
				int wiphy_freq = wiphy->bands[band]->channels[j].center_freq;
				for (k = 0; k < wreq->num_channels; k++) {
					struct iw_freq *freq =
						&wreq->channel_list[k];
					int wext_freq =
						cfg80211_wext_freq(freq);

					if (wext_freq == wiphy_freq)
						goto wext_freq_found;
				}
				goto wext_freq_not_found;
			}

		wext_freq_found:
			creq->channels[i] = &wiphy->bands[band]->channels[j];
			i++;
		wext_freq_not_found: ;
		}
	}
	/* No channels found? */
	if (!i) {
		err = -EINVAL;
		goto out;
	}

	/* Set real number of channels specified in creq->channels[] */
	creq->n_channels = i;

	/* translate "Scan for SSID" request */
	if (wreq) {
		if (wrqu->data.flags & IW_SCAN_THIS_ESSID) {
			if (wreq->essid_len > IEEE80211_MAX_SSID_LEN) {
				err = -EINVAL;
				goto out;
			}
			memcpy(creq->ssids[0].ssid, wreq->essid, wreq->essid_len);
			creq->ssids[0].ssid_len = wreq->essid_len;
		}
		if (wreq->scan_type == IW_SCAN_TYPE_PASSIVE)
			creq->n_ssids = 0;
	}

	for (i = 0; i < NUM_NL80211_BANDS; i++)
		if (wiphy->bands[i])
			creq->rates[i] = (1 << wiphy->bands[i]->n_bitrates) - 1;

	eth_broadcast_addr(creq->bssid);

	wiphy_lock(&rdev->wiphy);

	rdev->scan_req = creq;
	err = rdev_scan(rdev, creq);
	if (err) {
		rdev->scan_req = NULL;
		/* creq will be freed below */
	} else {
		nl80211_send_scan_start(rdev, dev->ieee80211_ptr);
		/* creq now owned by driver */
		creq = NULL;
		dev_hold(dev);
	}
	wiphy_unlock(&rdev->wiphy);
 out:
	kfree(creq);
	return err;
}
EXPORT_WEXT_HANDLER(cfg80211_wext_siwscan);

static char *ieee80211_scan_add_ies(struct iw_request_info *info,
				    const struct cfg80211_bss_ies *ies,
				    char *current_ev, char *end_buf)
{
	const u8 *pos, *end, *next;
	struct iw_event iwe;

	if (!ies)
		return current_ev;

	/*
	 * If needed, fragment the IEs buffer (at IE boundaries) into short
	 * enough fragments to fit into IW_GENERIC_IE_MAX octet messages.
	 */
	pos = ies->data;
	end = pos + ies->len;

	while (end - pos > IW_GENERIC_IE_MAX) {
		next = pos + 2 + pos[1];
		while (next + 2 + next[1] - pos < IW_GENERIC_IE_MAX)
			next = next + 2 + next[1];

		memset(&iwe, 0, sizeof(iwe));
		iwe.cmd = IWEVGENIE;
		iwe.u.data.length = next - pos;
		current_ev = iwe_stream_add_point_check(info, current_ev,
							end_buf, &iwe,
							(void *)pos);
		if (IS_ERR(current_ev))
			return current_ev;
		pos = next;
	}

	if (end > pos) {
		memset(&iwe, 0, sizeof(iwe));
		iwe.cmd = IWEVGENIE;
		iwe.u.data.length = end - pos;
		current_ev = iwe_stream_add_point_check(info, current_ev,
							end_buf, &iwe,
							(void *)pos);
		if (IS_ERR(current_ev))
			return current_ev;
	}

	return current_ev;
}

static char *
ieee80211_bss(struct wiphy *wiphy, struct iw_request_info *info,
	      struct cfg80211_internal_bss *bss, char *current_ev,
	      char *end_buf)
{
	const struct cfg80211_bss_ies *ies;
	struct iw_event iwe;
	const u8 *ie;
	u8 buf[50];
	u8 *cfg, *p, *tmp;
	int rem, i, sig;
	bool ismesh = false;

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWAP;
	iwe.u.ap_addr.sa_family = ARPHRD_ETHER;
	memcpy(iwe.u.ap_addr.sa_data, bss->pub.bssid, ETH_ALEN);
	current_ev = iwe_stream_add_event_check(info, current_ev, end_buf, &iwe,
						IW_EV_ADDR_LEN);
	if (IS_ERR(current_ev))
		return current_ev;

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWFREQ;
	iwe.u.freq.m = ieee80211_frequency_to_channel(bss->pub.channel->center_freq);
	iwe.u.freq.e = 0;
	current_ev = iwe_stream_add_event_check(info, current_ev, end_buf, &iwe,
						IW_EV_FREQ_LEN);
	if (IS_ERR(current_ev))
		return current_ev;

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWFREQ;
	iwe.u.freq.m = bss->pub.channel->center_freq;
	iwe.u.freq.e = 6;
	current_ev = iwe_stream_add_event_check(info, current_ev, end_buf, &iwe,
						IW_EV_FREQ_LEN);
	if (IS_ERR(current_ev))
		return current_ev;

	if (wiphy->signal_type != CFG80211_SIGNAL_TYPE_NONE) {
		memset(&iwe, 0, sizeof(iwe));
		iwe.cmd = IWEVQUAL;
		iwe.u.qual.updated = IW_QUAL_LEVEL_UPDATED |
				     IW_QUAL_NOISE_INVALID |
				     IW_QUAL_QUAL_UPDATED;
		switch (wiphy->signal_type) {
		case CFG80211_SIGNAL_TYPE_MBM:
			sig = bss->pub.signal / 100;
			iwe.u.qual.level = sig;
			iwe.u.qual.updated |= IW_QUAL_DBM;
			if (sig < -110)		/* rather bad */
				sig = -110;
			else if (sig > -40)	/* perfect */
				sig = -40;
			/* will give a range of 0 .. 70 */
			iwe.u.qual.qual = sig + 110;
			break;
		case CFG80211_SIGNAL_TYPE_UNSPEC:
			iwe.u.qual.level = bss->pub.signal;
			/* will give range 0 .. 100 */
			iwe.u.qual.qual = bss->pub.signal;
			break;
		default:
			/* not reached */
			break;
		}
		current_ev = iwe_stream_add_event_check(info, current_ev,
							end_buf, &iwe,
							IW_EV_QUAL_LEN);
		if (IS_ERR(current_ev))
			return current_ev;
	}

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWENCODE;
	if (bss->pub.capability & WLAN_CAPABILITY_PRIVACY)
		iwe.u.data.flags = IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
	else
		iwe.u.data.flags = IW_ENCODE_DISABLED;
	iwe.u.data.length = 0;
	current_ev = iwe_stream_add_point_check(info, current_ev, end_buf,
						&iwe, "");
	if (IS_ERR(current_ev))
		return current_ev;

	rcu_read_lock();
	ies = rcu_dereference(bss->pub.ies);
	rem = ies->len;
	ie = ies->data;

	while (rem >= 2) {
		/* invalid data */
		if (ie[1] > rem - 2)
			break;

		switch (ie[0]) {
		case WLAN_EID_SSID:
			memset(&iwe, 0, sizeof(iwe));
			iwe.cmd = SIOCGIWESSID;
			iwe.u.data.length = ie[1];
			iwe.u.data.flags = 1;
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf, &iwe,
								(u8 *)ie + 2);
			if (IS_ERR(current_ev))
				goto unlock;
			break;
		case WLAN_EID_MESH_ID:
			memset(&iwe, 0, sizeof(iwe));
			iwe.cmd = SIOCGIWESSID;
			iwe.u.data.length = ie[1];
			iwe.u.data.flags = 1;
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf, &iwe,
								(u8 *)ie + 2);
			if (IS_ERR(current_ev))
				goto unlock;
			break;
		case WLAN_EID_MESH_CONFIG:
			ismesh = true;
			if (ie[1] != sizeof(struct ieee80211_meshconf_ie))
				break;
			cfg = (u8 *)ie + 2;
			memset(&iwe, 0, sizeof(iwe));
			iwe.cmd = IWEVCUSTOM;
			iwe.u.data.length = sprintf(buf,
						    "Mesh Network Path Selection Protocol ID: 0x%02X",
						    cfg[0]);
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf,
								&iwe, buf);
			if (IS_ERR(current_ev))
				goto unlock;
			iwe.u.data.length = sprintf(buf,
						    "Path Selection Metric ID: 0x%02X",
						    cfg[1]);
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf,
								&iwe, buf);
			if (IS_ERR(current_ev))
				goto unlock;
			iwe.u.data.length = sprintf(buf,
						    "Congestion Control Mode ID: 0x%02X",
						    cfg[2]);
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf,
								&iwe, buf);
			if (IS_ERR(current_ev))
				goto unlock;
			iwe.u.data.length = sprintf(buf,
						    "Synchronization ID: 0x%02X",
						    cfg[3]);
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf,
								&iwe, buf);
			if (IS_ERR(current_ev))
				goto unlock;
			iwe.u.data.length = sprintf(buf,
						    "Authentication ID: 0x%02X",
						    cfg[4]);
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf,
								&iwe, buf);
			if (IS_ERR(current_ev))
				goto unlock;
			iwe.u.data.length = sprintf(buf,
						    "Formation Info: 0x%02X",
						    cfg[5]);
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf,
								&iwe, buf);
			if (IS_ERR(current_ev))
				goto unlock;
			iwe.u.data.length = sprintf(buf,
						    "Capabilities: 0x%02X",
						    cfg[6]);
			current_ev = iwe_stream_add_point_check(info,
								current_ev,
								end_buf,
								&iwe, buf);
			if (IS_ERR(current_ev))
				goto unlock;
			break;
		case WLAN_EID_SUPP_RATES:
		case WLAN_EID_EXT_SUPP_RATES:
			/* display all supported rates in readable format */
			p = current_ev + iwe_stream_lcp_len(info);

			memset(&iwe, 0, sizeof(iwe));
			iwe.cmd = SIOCGIWRATE;
			/* Those two flags are ignored... */
			iwe.u.bitrate.fixed = iwe.u.bitrate.disabled = 0;

			for (i = 0; i < ie[1]; i++) {
				iwe.u.bitrate.value =
					((ie[i + 2] & 0x7f) * 500000);
				tmp = p;
				p = iwe_stream_add_value(info, current_ev, p,
							 end_buf, &iwe,
							 IW_EV_PARAM_LEN);
				if (p == tmp) {
					current_ev = ERR_PTR(-E2BIG);
					goto unlock;
				}
			}
			current_ev = p;
			break;
		}
		rem -= ie[1] + 2;
		ie += ie[1] + 2;
	}

	if (bss->pub.capability & (WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_IBSS) ||
	    ismesh) {
		memset(&iwe, 0, sizeof(iwe));
		iwe.cmd = SIOCGIWMODE;
		if (ismesh)
			iwe.u.mode = IW_MODE_MESH;
		else if (bss->pub.capability & WLAN_CAPABILITY_ESS)
			iwe.u.mode = IW_MODE_MASTER;
		else
			iwe.u.mode = IW_MODE_ADHOC;
		current_ev = iwe_stream_add_event_check(info, current_ev,
							end_buf, &iwe,
							IW_EV_UINT_LEN);
		if (IS_ERR(current_ev))
			goto unlock;
	}

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length = sprintf(buf, "tsf=%016llx",
				    (unsigned long long)(ies->tsf));
	current_ev = iwe_stream_add_point_check(info, current_ev, end_buf,
						&iwe, buf);
	if (IS_ERR(current_ev))
		goto unlock;
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = IWEVCUSTOM;
	iwe.u.data.length = sprintf(buf, " Last beacon: %ums ago",
				    elapsed_jiffies_msecs(bss->ts));
	current_ev = iwe_stream_add_point_check(info, current_ev,
						end_buf, &iwe, buf);
	if (IS_ERR(current_ev))
		goto unlock;

	current_ev = ieee80211_scan_add_ies(info, ies, current_ev, end_buf);

 unlock:
	rcu_read_unlock();
	return current_ev;
}


static int ieee80211_scan_results(struct cfg80211_registered_device *rdev,
				  struct iw_request_info *info,
				  char *buf, size_t len)
{
	char *current_ev = buf;
	char *end_buf = buf + len;
	struct cfg80211_internal_bss *bss;
	int err = 0;

	spin_lock_bh(&rdev->bss_lock);
	cfg80211_bss_expire(rdev);

	list_for_each_entry(bss, &rdev->bss_list, list) {
		if (buf + len - current_ev <= IW_EV_ADDR_LEN) {
			err = -E2BIG;
			break;
		}
		current_ev = ieee80211_bss(&rdev->wiphy, info, bss,
					   current_ev, end_buf);
		if (IS_ERR(current_ev)) {
			err = PTR_ERR(current_ev);
			break;
		}
	}
	spin_unlock_bh(&rdev->bss_lock);

	if (err)
		return err;
	return current_ev - buf;
}


int cfg80211_wext_giwscan(struct net_device *dev,
			  struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{
	struct iw_point *data = &wrqu->data;
	struct cfg80211_registered_device *rdev;
	int res;

	if (!netif_running(dev))
		return -ENETDOWN;

	rdev = cfg80211_get_dev_from_ifindex(dev_net(dev), dev->ifindex);

	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	if (rdev->scan_req || rdev->scan_msg)
		return -EAGAIN;

	res = ieee80211_scan_results(rdev, info, extra, data->length);
	data->length = 0;
	if (res >= 0) {
		data->length = res;
		res = 0;
	}

	return res;
}
EXPORT_WEXT_HANDLER(cfg80211_wext_giwscan);
#endif
