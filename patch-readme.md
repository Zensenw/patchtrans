a/arch/x86/entry/syscalls/syscall_32.tbl

471	i386	encrypt_mprotect	sys_encrypt_mprotect

a/arch/x86/entry/syscalls/syscall_64.tbl

471	common	encrypt_mprotect	sys_encrypt_mprotect

arch/x86/include/asm/pgtable_types.h

origin:

#define _COMMON_PAGE_CHG_MASK	(PTE_PFN_MASK | _PAGE_PCD | _PAGE_PWT |	\
				 _PAGE_SPECIAL | _PAGE_ACCESSED |	\
				 _PAGE_DIRTY_BITS | _PAGE_SOFT_DIRTY |	\
				 _PAGE_DEVMAP | _PAGE_CC | _PAGE_UFFD_WP)
#define _PAGE_CHG_MASK	(_COMMON_PAGE_CHG_MASK | _PAGE_PAT)
#define _HPAGE_CHG_MASK (_COMMON_PAGE_CHG_MASK | _PAGE_PSE | _PAGE_PAT_LARGE)

_PAGE_PAT_LARGE有什么用？??