/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PAGE_64_H
#define _ASM_X86_PAGE_64_H

#include <asm/page_64_types.h>

#ifndef __ASSEMBLY__
#include <asm/cpufeatures.h>
#include <asm/alternative.h>

#include <linux/kmsan-checks.h>

/* duplicated to the one in bootmem.h */
extern unsigned long max_pfn;
extern unsigned long phys_base;

extern unsigned long page_offset_base;
extern unsigned long vmalloc_base;
extern unsigned long vmemmap_base;
extern unsigned long direct_mapping_size;
extern unsigned long direct_mapping_mask;

static __always_inline unsigned long __phys_addr_nodebug(unsigned long x)
{
	unsigned long y = x - __START_KERNEL_map;

	/* use the carry flag to determine if x was < __START_KERNEL_map */
	x = y + ((x > y) ? phys_base : (__START_KERNEL_map - PAGE_OFFSET));

	return x & direct_mapping_mask;
}

#ifdef CONFIG_DEBUG_VIRTUAL
extern unsigned long __phys_addr(unsigned long);
extern unsigned long __phys_addr_symbol(unsigned long);
#else
#define __phys_addr(x)		__phys_addr_nodebug(x)
#define __phys_addr_symbol(x) \
	((unsigned long)(x) - __START_KERNEL_map + phys_base)
#endif

#define __phys_reloc_hide(x)	(x)

void clear_page_orig(void *page);
void clear_page_rep(void *page);
void clear_page_erms(void *page);


#ifdef CONFIG_X86_INTEL_MKTME
static inline void _movdir64b(void *dst, const void *src)
{
	// assert((uintptr_t)src == (uintptr_t)((uintptr_t)src & ~(64-1)));
	// assert((uintptr_t)dst == (uintptr_t)((uintptr_t)dst & ~(64-1)));
	const struct { char _[64]; } *__src = src;
	struct { char _[64]; } *__dst = dst;

	asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02"
		     : "+m" (*__dst)
		     :  "m" (*__src), "a" (__dst), "d" (__src));
}
static inline void clear_page(void *page)
{
	uint8_t* dst = (uint8_t*)page;
	uint32_t block = 0;
	__attribute__((aligned(64))) const uint8_t ZeroBlock[64] = { 0 };
	asm volatile("mfence");
	for (block = 0; block < (4096 / 64); block++) {
		_movdir64b(dst, ZeroBlock);
		dst += 64;
	}
	asm volatile("mfence");
}
#else

static inline void clear_page(void *page)
{
	/*
	 * Clean up KMSAN metadata for the page being cleared. The assembly call
	 * below clobbers @page, so we perform unpoisoning before it.
	 */
	kmsan_unpoison_memory(page, PAGE_SIZE);
	alternative_call_2(clear_page_orig,
			   clear_page_rep, X86_FEATURE_REP_GOOD,
			   clear_page_erms, X86_FEATURE_ERMS,
			   "=D" (page),
			   "0" (page)
			   : "cc", "memory", "rax", "rcx");
}
#endif /* CONFIG_X86_INTEL_MKTME */

// void copy_page(void *to, void *from);
#if defined(CONFIG_X86_INTEL_MKTME)
static inline void copy_page(void *to, void* from)
{
	uint8_t* dst = (uint8_t*)to;
	uint8_t* src = (uint8_t*)from;
	uint32_t block = 0;
	asm volatile("mfence");
	for (block = 0; block < (4096 / 64); block++) {
		_movdir64b(dst, src);
		dst += 64;
		src += 64;
	}
	asm volatile("mfence");
}
#else
void copy_page_orig(void *to, void *from);
#endif

#ifdef CONFIG_X86_5LEVEL
/*
 * User space process size.  This is the first address outside the user range.
 * There are a few constraints that determine this:
 *
 * On Intel CPUs, if a SYSCALL instruction is at the highest canonical
 * address, then that syscall will enter the kernel with a
 * non-canonical return address, and SYSRET will explode dangerously.
 * We avoid this particular problem by preventing anything
 * from being mapped at the maximum canonical address.
 *
 * On AMD CPUs in the Ryzen family, there's a nasty bug in which the
 * CPUs malfunction if they execute code from the highest canonical page.
 * They'll speculate right off the end of the canonical space, and
 * bad things happen.  This is worked around in the same way as the
 * Intel problem.
 *
 * With page table isolation enabled, we map the LDT in ... [stay tuned]
 */
static __always_inline unsigned long task_size_max(void)
{
	unsigned long ret;

	alternative_io("movq %[small],%0","movq %[large],%0",
			X86_FEATURE_LA57,
			"=r" (ret),
			[small] "i" ((1ul << 47)-PAGE_SIZE),
			[large] "i" ((1ul << 56)-PAGE_SIZE));

	return ret;
}
#endif	/* CONFIG_X86_5LEVEL */

#endif	/* !__ASSEMBLY__ */

#ifdef CONFIG_X86_VSYSCALL_EMULATION
# define __HAVE_ARCH_GATE_AREA 1
#endif

#endif /* _ASM_X86_PAGE_64_H */
