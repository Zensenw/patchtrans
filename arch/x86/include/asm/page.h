/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PAGE_H
#define _ASM_X86_PAGE_H

#include <linux/types.h>

#ifdef __KERNEL__

#include <asm/page_types.h>

#ifdef CONFIG_X86_64
#include <asm/page_64.h>
#else
#include <asm/page_32.h>
#endif	/* CONFIG_X86_64 */

#ifndef __ASSEMBLY__

struct page;

#include <linux/range.h>
#include <asm/mktme.h>
extern struct range pfn_mapped[];
extern int nr_pfn_mapped;

static inline void clear_user_page(void *page, unsigned long vaddr,
				   struct page *pg)
{
	clear_page(page);
}

void copy_page(void *to, void *from);

static inline void copy_user_page(void *to, void *from, unsigned long vaddr,
				  struct page *topage)
{
	copy_page(to, from);
}

#define vma_alloc_zeroed_movable_folio(vma, vaddr) \
	vma_alloc_folio(GFP_HIGHUSER_MOVABLE | __GFP_ZERO, 0, vma, vaddr, false)

#ifndef __pa
#define __pa(x)		__phys_addr((unsigned long)(x))
#endif

#define __pa_nodebug(x)	__phys_addr_nodebug((unsigned long)(x))
/* __pa_symbol should be used for C visible symbols.
   This seems to be the official gcc blessed way to do such arithmetic. */
/*
 * We need __phys_reloc_hide() here because gcc may assume that there is no
 * overflow during __pa() calculation and can optimize it unexpectedly.
 * Newer versions of gcc provide -fno-strict-overflow switch to handle this
 * case properly. Once all supported versions of gcc understand it, we can
 * remove this Voodoo magic stuff. (i.e. once gcc3.x is deprecated)
 */
#define __pa_symbol(x) \
	__phys_addr_symbol(__phys_reloc_hide((unsigned long)(x)))

#ifndef __va
#define __va(x)			((void *)((unsigned long)(x)+PAGE_OFFSET))
#endif

#define __boot_va(x)		__va(x)
#define __boot_pa(x)		__pa(x)

/*
 * virt_to_page(kaddr) returns a valid pointer if and only if
 * virt_addr_valid(kaddr) returns true.
 */
#define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
extern bool __virt_addr_valid(unsigned long kaddr);
#define virt_addr_valid(kaddr)	__virt_addr_valid((unsigned long) (kaddr))

#define page_to_virt_keyid(x, keyid) \
	(__va(PFN_PHYS(page_to_pfn(x))) + (keyid) * direct_mapping_size)

#define page_to_virt(x) page_to_virt_keyid(x, page_keyid(x))

static __always_inline void *pfn_to_kaddr(unsigned long pfn)
{
	return __va(pfn << PAGE_SHIFT);
}

static __always_inline u64 __canonical_address(u64 vaddr, u8 vaddr_bits)
{
	return ((s64)vaddr << (64 - vaddr_bits)) >> (64 - vaddr_bits);
}

static __always_inline u64 __is_canonical_address(u64 vaddr, u8 vaddr_bits)
{
	return __canonical_address(vaddr, vaddr_bits) == vaddr;
}

#endif	/* __ASSEMBLY__ */

#include <asm-generic/memory_model.h>
#include <asm-generic/getorder.h>

#define HAVE_ARCH_HUGETLB_UNMAPPED_AREA

#endif	/* __KERNEL__ */
#endif /* _ASM_X86_PAGE_H */
