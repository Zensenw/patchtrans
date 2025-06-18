#ifndef	_ASM_X86_MKTME_H
#define	_ASM_X86_MKTME_H

#include <linux/types.h>
#include <linux/page_ext.h>
#include <linux/jump_label.h>

struct vm_area_struct;

#ifdef CONFIG_X86_INTEL_MKTME
extern phys_addr_t __mktme_keyid_mask;
extern phys_addr_t mktme_keyid_mask(void);
extern int __mktme_keyid_shift;
extern int mktme_keyid_shift(void);
extern int __mktme_nr_keyids;
extern int mktme_nr_keyids(void);
extern unsigned int mktme_algs;

/* Set the encryption keyid bits in a VMA */
extern void mprotect_set_encrypt(struct vm_area_struct *vma, int newkeyid,
				unsigned long start, unsigned long end, unsigned long mprotect_fixup_flags);

/* MTKME encrypt_count for VMAs */
extern struct percpu_ref *encrypt_count;
extern void vma_get_encrypt_ref(struct vm_area_struct *vma);
extern void vma_put_encrypt_ref(struct vm_area_struct *vma);

DECLARE_STATIC_KEY_FALSE(mktme_enabled_key);
static inline bool mktme_enabled(void)
{
	return static_branch_unlikely(&mktme_enabled_key);
}

void mktme_disable(void);

extern struct page_ext_operations page_mktme_ops;

#define page_keyid page_keyid
static inline int page_keyid(const struct page *page)
{
	if (!mktme_enabled())
		return 0;

	return lookup_page_ext(page)->keyid;
}

#define vma_keyid vma_keyid
int __vma_keyid(struct vm_area_struct *vma);
static inline int vma_keyid(struct vm_area_struct *vma)
{
	if (!mktme_enabled())
		return 0;

	return __vma_keyid(vma);
}

#define prep_encrypted_page prep_encrypted_page
void __prep_encrypted_page(struct page *page, int order, int keyid, bool zero);
static inline void prep_encrypted_page(struct page *page, int order,
		int keyid, bool zero)
{
	if (keyid)
		__prep_encrypted_page(page, order, keyid, zero);
}

#define HAVE_ARCH_FREE_PAGE
void free_encrypted_page(struct page *page, int order);
static inline void arch_free_page(struct page *page, int order)
{
	if (page_keyid(page))
		free_encrypted_page(page, order);
}

int sync_direct_mapping(unsigned long start, unsigned long end);

#else
#define mktme_keyid_mask()	((phys_addr_t)0)
#define mktme_nr_keyids()	0
#define mktme_keyid_shift()	0

#define page_keyid(page) 0

static inline bool mktme_enabled(void)
{
	return false;
}

static inline void mktme_disable(void) {}

static inline int sync_direct_mapping(unsigned long start, unsigned long end)
{
	return 0;
}
#endif

#endif
