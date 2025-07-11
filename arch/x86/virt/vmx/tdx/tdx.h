/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X86_VIRT_TDX_H
#define _X86_VIRT_TDX_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/compiler_attributes.h>
#include <linux/bits.h>

/*
 * This file contains both macros and data structures defined by the TDX
 * architecture and Linux defined software data structures and functions.
 * The two should not be mixed together for better readability.  The
 * architectural definitions come first.
 */

/*
 * TDX module SEAMCALL leaf functions
 */
#define TDH_PHYMEM_PAGE_RDMD	24
#define TDH_SYS_KEY_CONFIG	31
#define TDH_SYS_INFO		32
#define TDH_SYS_INIT		33
#define TDH_SYS_RD		34
#define TDH_SYS_LP_INIT		35
#define TDH_SYS_TDMR_INIT	36
#define TDH_SYS_CONFIG		45

/* TDX page types */
#define	PT_NDA		0x0
#define	PT_RSVD		0x1

/*
 * Global scope metadata field ID.
 *
 * See Table "Global Scope Metadata", TDX module 1.5 ABI spec.
 */
#define MD_FIELD_ID_MAX_TDMRS			0x9100000100000008ULL
#define MD_FIELD_ID_MAX_RESERVED_PER_TDMR	0x9100000100000009ULL
#define MD_FIELD_ID_PAMT_4K_ENTRY_SIZE		0x9100000100000010ULL
#define MD_FIELD_ID_PAMT_2M_ENTRY_SIZE		0x9100000100000011ULL
#define MD_FIELD_ID_PAMT_1G_ENTRY_SIZE		0x9100000100000012ULL

/*
 * Sub-field definition of metadata field ID.
 *
 * See Table "MD_FIELD_ID (Metadata Field Identifier / Sequence Header)
 * Definition", TDX module 1.5 ABI spec.
 *
 *  - Bit 33:32: ELEMENT_SIZE_CODE -- size of a single element of metadata
 *
 *	0: 8 bits
 *	1: 16 bits
 *	2: 32 bits
 *	3: 64 bits
 */
#define MD_FIELD_ID_ELE_SIZE_CODE(_field_id)	\
		(((_field_id) & GENMASK_ULL(33, 32)) >> 32)

struct tdmr_reserved_area {
	u64 offset;
	u64 size;
} __packed;

#define TDMR_INFO_ALIGNMENT	512
#define TDMR_INFO_PA_ARRAY_ALIGNMENT	512

struct tdmr_info {
	u64 base;
	u64 size;
	u64 pamt_1g_base;
	u64 pamt_1g_size;
	u64 pamt_2m_base;
	u64 pamt_2m_size;
	u64 pamt_4k_base;
	u64 pamt_4k_size;
	/*
	 * The actual number of reserved areas depends on the value of
	 * field MD_FIELD_ID_MAX_RESERVED_PER_TDMR in the TDX module
	 * global metadata.
	 */
	DECLARE_FLEX_ARRAY(struct tdmr_reserved_area, reserved_areas);
} __packed __aligned(TDMR_INFO_ALIGNMENT);

/*
 * Do not put any hardware-defined TDX structure representations below
 * this comment!
 */

/* Kernel defined TDX module status during module initialization. */
enum tdx_module_status_t {
	TDX_MODULE_UNINITIALIZED,
	TDX_MODULE_INITIALIZED,
	TDX_MODULE_ERROR
};

struct tdx_memblock {
	struct list_head list;
	unsigned long start_pfn;
	unsigned long end_pfn;
	int nid;
};

/* "TDMR info" part of "Global Scope Metadata" for constructing TDMRs */
struct tdx_tdmr_sysinfo {
	u16 max_tdmrs;
	u16 max_reserved_per_tdmr;
	u16 pamt_entry_size[TDX_PS_NR];
};

/* Warn if kernel has less than TDMR_NR_WARN TDMRs after allocation */
#define TDMR_NR_WARN 4

struct tdmr_info_list {
	void *tdmrs;	/* Flexible array to hold 'tdmr_info's */
	int nr_consumed_tdmrs;	/* How many 'tdmr_info's are in use */

	/* Metadata for finding target 'tdmr_info' and freeing @tdmrs */
	int tdmr_sz;	/* Size of one 'tdmr_info' */
	int max_tdmrs;	/* How many 'tdmr_info's are allocated */
};

struct cmr_info {
	u64	base;
	u64	size;
} __packed;

#define MAX_CMRS	32
#define CMR_INFO_ARRAY_ALIGNMENT	512

struct cpuid_config {
	u32	leaf;
	u32	sub_leaf;
	u32	eax;
	u32	ebx;
	u32	ecx;
	u32	edx;
} __packed;

#define TDSYSINFO_STRUCT_SIZE		1024
#define TDSYSINFO_STRUCT_ALIGNMENT	1024

/*
 * The size of this structure itself is flexible.  The actual structure
 * passed to TDH.SYS.INFO must be padded to TDSYSINFO_STRUCT_SIZE bytes
 * and TDSYSINFO_STRUCT_ALIGNMENT bytes aligned.
 */
struct tdsysinfo_struct {
	/* TDX-SEAM Module Info */
	u32	attributes;
	u32	vendor_id;
	u32	build_date;
	u16	build_num;
	u16	minor_version;
	u16	major_version;
	u8	reserved0[14];
	/* Memory Info */
	u16	max_tdmrs;
	u16	max_reserved_per_tdmr;
	u16	pamt_entry_size;
	u8	reserved1[10];
	/* Control Struct Info */
	u16	tdcs_base_size;
	u8	reserved2[2];
	u16	tdvps_base_size;
	u8	tdvps_xfam_dependent_size;
	u8	reserved3[9];
	/* TD Capabilities */
	u64	attributes_fixed0;
	u64	attributes_fixed1;
	u64	xfam_fixed0;
	u64	xfam_fixed1;
	u8	reserved4[32];
	u32	num_cpuid_config;
	/*
	 * The actual number of CPUID_CONFIG depends on above
	 * 'num_cpuid_config'.
	 */
	DECLARE_FLEX_ARRAY(struct cpuid_config, cpuid_configs);
} __packed;


#endif
