/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2021-2022 Intel Corporation */
#ifndef _ASM_X86_TDX_H
#define _ASM_X86_TDX_H

#include <linux/init.h>
#include <linux/bits.h>

#include <asm/errno.h>
#include <asm/ptrace.h>
#include <asm/trapnr.h>
#include <asm/shared/tdx.h>

/*
 * SW-defined error codes.
 *
 * Bits 47:40 == 0xFF indicate Reserved status code class that never used by
 * TDX module.
 */
#define TDX_ERROR			_BITULL(63)
#define TDX_SW_ERROR			(TDX_ERROR | GENMASK_ULL(47, 40))
#define TDX_SEAMCALL_VMFAILINVALID	(TDX_SW_ERROR | _ULL(0xFFFF0000))

#define TDX_SEAMCALL_GP			(TDX_SW_ERROR | X86_TRAP_GP)
#define TDX_SEAMCALL_UD			(TDX_SW_ERROR | X86_TRAP_UD)

/*
 * TDX module SEAMCALL leaf function error codes
 */
#define TDX_SUCCESS		0ULL
#define TDX_RND_NO_ENTROPY	0x8000020300000000ULL

#ifndef __ASSEMBLY__

#include <uapi/asm/mce.h>

/*
 * Used by the #VE exception handler to gather the #VE exception
 * info from the TDX module. This is a software only structure
 * and not part of the TDX module/VMM ABI.
 */
struct ve_info {
	u64 exit_reason;
	u64 exit_qual;
	/* Guest Linear (virtual) Address */
	u64 gla;
	/* Guest Physical Address */
	u64 gpa;
	u32 instr_len;
	u32 instr_info;
};

#ifdef CONFIG_INTEL_TDX_GUEST

void __init tdx_early_init(void);

void tdx_get_ve_info(struct ve_info *ve);

bool tdx_handle_virt_exception(struct pt_regs *regs, struct ve_info *ve);

void tdx_safe_halt(void);

bool tdx_early_handle_ve(struct pt_regs *regs);

int tdx_mcall_get_report0(u8 *reportdata, u8 *tdreport);

u64 tdx_hcall_get_quote(u8 *buf, size_t size);

#else

static inline void tdx_early_init(void) { };
static inline void tdx_safe_halt(void) { };

static inline bool tdx_early_handle_ve(struct pt_regs *regs) { return false; }

#endif /* CONFIG_INTEL_TDX_GUEST */

#if defined(CONFIG_KVM_GUEST) && defined(CONFIG_INTEL_TDX_GUEST)
long tdx_kvm_hypercall(unsigned int nr, unsigned long p1, unsigned long p2,
		       unsigned long p3, unsigned long p4);
#else
static inline long tdx_kvm_hypercall(unsigned int nr, unsigned long p1,
				     unsigned long p2, unsigned long p3,
				     unsigned long p4)
{
	return -ENODEV;
}
#endif /* CONFIG_INTEL_TDX_GUEST && CONFIG_KVM_GUEST */

#ifdef CONFIG_INTEL_TDX_HOST

extern u32 tdx_global_keyid;
extern u32 tdx_guest_keyid_start;
extern u32 tdx_nr_guest_keyids;

u64 __seamcall(u64 fn, struct tdx_module_args *args);
u64 __seamcall_ret(u64 fn, struct tdx_module_args *args);
u64 __seamcall_saved_ret(u64 fn, struct tdx_module_args *args);
void tdx_init(void);

#include <asm/archrandom.h>

typedef u64 (*sc_func_t)(u64 fn, struct tdx_module_args *args);

static inline u64 sc_retry(sc_func_t func, u64 fn,
			   struct tdx_module_args *args)
{
	int retry = RDRAND_RETRY_LOOPS;
	u64 ret;

	do {
		ret = func(fn, args);
	} while (ret == TDX_RND_NO_ENTROPY && --retry);

	return ret;
}

#define seamcall(_fn, _args)		sc_retry(__seamcall, (_fn), (_args))
#define seamcall_ret(_fn, _args)	sc_retry(__seamcall_ret, (_fn), (_args))
#define seamcall_saved_ret(_fn, _args)	sc_retry(__seamcall_saved_ret, (_fn), (_args))
int tdx_cpu_enable(void);
int tdx_enable(void);
const char *tdx_dump_mce_info(struct mce *m);
void tdx_reset_memory(void);

struct notifier_block;

int tdx_register_memory_reset_notifier(struct notifier_block *nb);
void tdx_unregister_memory_reset_notifier(struct notifier_block *nb);

struct tdx_metadata_field_mapping {
	u64 field_id;
	int offset;
	int size;
};

#define TD_SYSINFO_MAP(_field_id, _struct, _member)	\
	{ .field_id = MD_FIELD_ID_##_field_id,		\
	  .offset   = offsetof(_struct, _member),	\
	  .size     = sizeof(typeof(((_struct *)0)->_member)) }

/*
 * Read multiple global metadata fields to a buffer of a structure
 * based on the "field ID -> structure member" mapping table.
 */
int tdx_sys_metadata_read(const struct tdx_metadata_field_mapping *fields,
			  int nr_fields, void *stbuf);

/* Read a single global metadata field */
int tdx_sys_metadata_field_read(u64 field_id, u64 *data);

#else
static inline void tdx_init(void) { }
static inline int tdx_cpu_enable(void) { return -ENODEV; }
static inline int tdx_enable(void)  { return -ENODEV; }
static inline const char *tdx_dump_mce_info(struct mce *m) { return NULL; }
static inline void tdx_reset_memory(void) { }

struct notifier_block;

static inline int tdx_register_memory_reset_notifier(struct notifier_block *nb)
{
	return -EOPNOTSUPP;
}
static inline void tdx_unregister_memory_reset_notifier(
		struct notifier_block *nb) { }
#endif	/* CONFIG_INTEL_TDX_HOST */

#endif /* !__ASSEMBLY__ */
#endif /* _ASM_X86_TDX_H */
