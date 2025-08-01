/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARCH_X86_KVM_CPUID_H
#define ARCH_X86_KVM_CPUID_H

#include "x86.h"
#include "reverse_cpuid.h"
#include <asm/cpu.h>
#include <asm/processor.h>
#include <uapi/asm/kvm_para.h>

extern u32 kvm_cpu_caps[NR_KVM_CPU_CAPS] __read_mostly;
void kvm_set_cpu_caps(void);

void kvm_update_cpuid_runtime(struct kvm_vcpu *vcpu);
void kvm_update_pv_runtime(struct kvm_vcpu *vcpu);
struct kvm_cpuid_entry2 *kvm_find_cpuid_entry2(struct kvm_cpuid_entry2 *entries,
					       int nent, u32 function, u64 index);
struct kvm_cpuid_entry2 *kvm_find_cpuid_entry_index(struct kvm_vcpu *vcpu,
						    u32 function, u32 index);
struct kvm_cpuid_entry2 *kvm_find_cpuid_entry(struct kvm_vcpu *vcpu,
					      u32 function);
int kvm_dev_ioctl_get_cpuid(struct kvm_cpuid2 *cpuid,
			    struct kvm_cpuid_entry2 __user *entries,
			    unsigned int type);
int kvm_vcpu_ioctl_set_cpuid(struct kvm_vcpu *vcpu,
			     struct kvm_cpuid *cpuid,
			     struct kvm_cpuid_entry __user *entries);
int kvm_vcpu_ioctl_set_cpuid2(struct kvm_vcpu *vcpu,
			      struct kvm_cpuid2 *cpuid,
			      struct kvm_cpuid_entry2 __user *entries);
int kvm_vcpu_ioctl_get_cpuid2(struct kvm_vcpu *vcpu,
			      struct kvm_cpuid2 *cpuid,
			      struct kvm_cpuid_entry2 __user *entries);
bool kvm_cpuid(struct kvm_vcpu *vcpu, u32 *eax, u32 *ebx,
	       u32 *ecx, u32 *edx, bool exact_only);

u32 xstate_required_size(u64 xstate_bv, bool compacted);

int cpuid_query_maxphyaddr(struct kvm_vcpu *vcpu);
u64 kvm_vcpu_reserved_gpa_bits_raw(struct kvm_vcpu *vcpu);

static inline int cpuid_maxphyaddr(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.maxphyaddr;
}

static inline bool kvm_vcpu_is_legal_gpa(struct kvm_vcpu *vcpu, gpa_t gpa)
{
	return !(gpa & vcpu->arch.reserved_gpa_bits);
}

static inline bool kvm_vcpu_is_legal_aligned_gpa(struct kvm_vcpu *vcpu,
						 gpa_t gpa, gpa_t alignment)
{
	return IS_ALIGNED(gpa, alignment) && kvm_vcpu_is_legal_gpa(vcpu, gpa);
}

static inline bool page_address_valid(struct kvm_vcpu *vcpu, gpa_t gpa)
{
	return kvm_vcpu_is_legal_aligned_gpa(vcpu, gpa, PAGE_SIZE);
}

static __always_inline void cpuid_entry_override(struct kvm_cpuid_entry2 *entry,
						 unsigned int leaf)
{
	u32 *reg = cpuid_entry_get_reg(entry, leaf * 32);

	BUILD_BUG_ON(leaf >= ARRAY_SIZE(kvm_cpu_caps));
	*reg = kvm_cpu_caps[leaf];
}

static __always_inline u32 *guest_cpuid_get_register(struct kvm_vcpu *vcpu,
						     unsigned int x86_feature)
{
	const struct cpuid_reg cpuid = x86_feature_cpuid(x86_feature);
	struct kvm_cpuid_entry2 *entry;

	entry = kvm_find_cpuid_entry_index(vcpu, cpuid.function, cpuid.index);
	if (!entry)
		return NULL;

	return __cpuid_entry_get_reg(entry, cpuid.reg);
}

static __always_inline bool guest_cpuid_has(struct kvm_vcpu *vcpu,
					    unsigned int x86_feature)
{
	u32 *reg;

	reg = guest_cpuid_get_register(vcpu, x86_feature);
	if (!reg)
		return false;

	return *reg & __feature_bit(x86_feature);
}

static __always_inline void guest_cpuid_clear(struct kvm_vcpu *vcpu,
					      unsigned int x86_feature)
{
	u32 *reg;

	reg = guest_cpuid_get_register(vcpu, x86_feature);
	if (reg)
		*reg &= ~__feature_bit(x86_feature);
}

static inline bool guest_cpuid_is_amd_or_hygon(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best;

	best = kvm_find_cpuid_entry(vcpu, 0);
	return best &&
	       (is_guest_vendor_amd(best->ebx, best->ecx, best->edx) ||
		is_guest_vendor_hygon(best->ebx, best->ecx, best->edx));
}

static inline bool guest_cpuid_is_intel(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best;

	best = kvm_find_cpuid_entry(vcpu, 0);
	return best && is_guest_vendor_intel(best->ebx, best->ecx, best->edx);
}

static inline bool guest_cpuid_is_amd_compatible(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.is_amd_compatible;
}

static inline bool guest_cpuid_is_intel_compatible(struct kvm_vcpu *vcpu)
{
	return !guest_cpuid_is_amd_compatible(vcpu);
}

static inline int guest_cpuid_family(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best;

	best = kvm_find_cpuid_entry(vcpu, 0x1);
	if (!best)
		return -1;

	return x86_family(best->eax);
}

static inline int guest_cpuid_model(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best;

	best = kvm_find_cpuid_entry(vcpu, 0x1);
	if (!best)
		return -1;

	return x86_model(best->eax);
}

static inline bool cpuid_model_is_consistent(struct kvm_vcpu *vcpu)
{
	return boot_cpu_data.x86_model == guest_cpuid_model(vcpu);
}

static inline int guest_cpuid_stepping(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best;

	best = kvm_find_cpuid_entry(vcpu, 0x1);
	if (!best)
		return -1;

	return x86_stepping(best->eax);
}

static inline bool guest_has_spec_ctrl_msr(struct kvm_vcpu *vcpu)
{
	return (guest_cpuid_has(vcpu, X86_FEATURE_SPEC_CTRL) ||
		guest_cpuid_has(vcpu, X86_FEATURE_AMD_STIBP) ||
		guest_cpuid_has(vcpu, X86_FEATURE_AMD_IBRS) ||
		guest_cpuid_has(vcpu, X86_FEATURE_AMD_SSBD));
}

static inline bool guest_has_pred_cmd_msr(struct kvm_vcpu *vcpu)
{
	return (guest_cpuid_has(vcpu, X86_FEATURE_SPEC_CTRL) ||
		guest_cpuid_has(vcpu, X86_FEATURE_AMD_IBPB) ||
		guest_cpuid_has(vcpu, X86_FEATURE_SBPB));
}

static inline bool supports_cpuid_fault(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.msr_platform_info & MSR_PLATFORM_INFO_CPUID_FAULT;
}

static inline bool cpuid_fault_enabled(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.msr_misc_features_enables &
		  MSR_MISC_FEATURES_ENABLES_CPUID_FAULT;
}

static __always_inline void kvm_cpu_cap_clear(unsigned int x86_feature)
{
	unsigned int x86_leaf = __feature_leaf(x86_feature);

	reverse_cpuid_check(x86_leaf);
	kvm_cpu_caps[x86_leaf] &= ~__feature_bit(x86_feature);
}

static __always_inline void kvm_cpu_cap_set(unsigned int x86_feature)
{
	unsigned int x86_leaf = __feature_leaf(x86_feature);

	reverse_cpuid_check(x86_leaf);
	kvm_cpu_caps[x86_leaf] |= __feature_bit(x86_feature);
}

static __always_inline u32 kvm_cpu_cap_get(unsigned int x86_feature)
{
	unsigned int x86_leaf = __feature_leaf(x86_feature);

	reverse_cpuid_check(x86_leaf);
	return kvm_cpu_caps[x86_leaf] & __feature_bit(x86_feature);
}

static __always_inline bool kvm_cpu_cap_has(unsigned int x86_feature)
{
	return !!kvm_cpu_cap_get(x86_feature);
}

static __always_inline void kvm_cpu_cap_check_and_set(unsigned int x86_feature)
{
	if (boot_cpu_has(x86_feature))
		kvm_cpu_cap_set(x86_feature);
}

static __always_inline bool guest_pv_has(struct kvm_vcpu *vcpu,
					 unsigned int kvm_feature)
{
	if (!vcpu->arch.pv_cpuid.enforce)
		return true;

	return vcpu->arch.pv_cpuid.features & (1u << kvm_feature);
}

enum kvm_governed_features {
#define KVM_GOVERNED_FEATURE(x) KVM_GOVERNED_##x,
#include "governed_features.h"
	KVM_NR_GOVERNED_FEATURES
};

static __always_inline int kvm_governed_feature_index(unsigned int x86_feature)
{
	switch (x86_feature) {
#define KVM_GOVERNED_FEATURE(x) case x: return KVM_GOVERNED_##x;
#include "governed_features.h"
	default:
		return -1;
	}
}

static __always_inline bool kvm_is_governed_feature(unsigned int x86_feature)
{
	return kvm_governed_feature_index(x86_feature) >= 0;
}

static __always_inline void kvm_governed_feature_set(struct kvm_vcpu *vcpu,
						     unsigned int x86_feature)
{
	BUILD_BUG_ON(!kvm_is_governed_feature(x86_feature));

	__set_bit(kvm_governed_feature_index(x86_feature),
		  vcpu->arch.governed_features.enabled);
}

static __always_inline void kvm_governed_feature_check_and_set(struct kvm_vcpu *vcpu,
							       unsigned int x86_feature)
{
	if (kvm_cpu_cap_has(x86_feature) && guest_cpuid_has(vcpu, x86_feature))
		kvm_governed_feature_set(vcpu, x86_feature);
}

static __always_inline bool guest_can_use(struct kvm_vcpu *vcpu,
					  unsigned int x86_feature)
{
	BUILD_BUG_ON(!kvm_is_governed_feature(x86_feature));

	return test_bit(kvm_governed_feature_index(x86_feature),
			vcpu->arch.governed_features.enabled);
}

static inline bool kvm_vcpu_is_legal_cr3(struct kvm_vcpu *vcpu, unsigned long cr3)
{
	if (guest_can_use(vcpu, X86_FEATURE_LAM))
		cr3 &= ~(X86_CR3_LAM_U48 | X86_CR3_LAM_U57);

	return kvm_vcpu_is_legal_gpa(vcpu, cr3);
}

#endif
