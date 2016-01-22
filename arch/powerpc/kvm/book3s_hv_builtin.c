/*
 * Copyright 2011 Paul Mackerras, IBM Corp. <paulus@au1.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 */

#include <linux/cpu.h>
#include <linux/kvm_host.h>
#include <linux/preempt.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/sizes.h>
#include <linux/cma.h>
#include <linux/bitops.h>

#include <asm/cputable.h>
#include <asm/kvm_ppc.h>
#include <asm/kvm_book3s.h>
#include <asm/archrandom.h>
#include <asm/xics.h>
#include <asm/dbell.h>
#include <asm/cputhreads.h>
#include <asm/io.h>

/*
 * Hash page table alignment on newer cpus(CPU_FTR_ARCH_206)
 * only needs to be 256kB.
 */
#define HPT_ALIGN_ORDER		18		/* 256k */
#define HPT_ALIGN_PAGES		((1 << HPT_ALIGN_ORDER) >> PAGE_SHIFT)

#define KVM_RESV_CHUNK_ORDER	HPT_ALIGN_ORDER

/*
 * By default we reserve 2% of memory exclusively for guest HPT
 * allocations, plus another 3% in the CMA zone which can be used
 * either for HPTs or for movable page allocations.
 * Each guest's HPT will be sized at between 1/128 and 1/64 of its
 * memory, i.e. up to 1.56%, and allowing for about a 3x memory
 * overcommit factor gets us to about 5%.
 */
static unsigned long kvm_hpt_resv_ratio = 2;

static int __init early_parse_kvm_hpt_resv(char *p)
{
	pr_debug("%s(%s)\n", __func__, p);
	if (!p)
		return -EINVAL;
	return kstrtoul(p, 0, &kvm_hpt_resv_ratio);
}
early_param("kvm_hpt_resv_ratio", early_parse_kvm_hpt_resv);

static unsigned long kvm_resv_addr;
static unsigned long *kvm_resv_bitmap;
static unsigned long kvm_resv_chunks;
static DEFINE_MUTEX(kvm_resv_lock);

void kvm_resv_hpt_init(void)
{
	unsigned long align = 1ul << KVM_RESV_CHUNK_ORDER;
	unsigned long size, bm_size;
	unsigned long addr, bm;
	unsigned long *bmp;

	if (!cpu_has_feature(CPU_FTR_HVMODE))
		return;

	size = memblock_phys_mem_size() * kvm_hpt_resv_ratio / 100;
	size = ALIGN(size, align);
	if (!size)
		return;

	pr_info("KVM: Allocating %lu MiB for hashed page tables\n",
		size >> 20);

	addr = __memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ACCESSIBLE);
	if (!addr) {
		pr_err("KVM: Allocation of reserved memory for HPTs failed\n");
		return;
	}
	pr_info("KVM: %lu MiB reserved for HPTs at %lx\n", size >> 20, addr);

	bm_size = BITS_TO_LONGS(size >> KVM_RESV_CHUNK_ORDER) * sizeof(long);
	bm = __memblock_alloc_base(bm_size, sizeof(long),
				   MEMBLOCK_ALLOC_ACCESSIBLE);
	if (!bm) {
		pr_err("KVM: Allocation of reserved memory bitmap failed\n");
		return;
	}
	bmp = __va(bm);
	memset(bmp, 0, bm_size);

	kvm_resv_addr = (unsigned long) __va(addr);
	kvm_resv_chunks = size >> KVM_RESV_CHUNK_ORDER;
	kvm_resv_bitmap = bmp;
}

unsigned long kvmhv_alloc_resv_hpt(u32 order)
{
	unsigned long nr_chunks = 1ul << (order - KVM_RESV_CHUNK_ORDER);
	unsigned long chunk;

	mutex_lock(&kvm_resv_lock);
	chunk = bitmap_find_next_zero_area(kvm_resv_bitmap, kvm_resv_chunks,
					   0, nr_chunks, 0);
	if (chunk < kvm_resv_chunks)
		bitmap_set(kvm_resv_bitmap, chunk, nr_chunks);
	mutex_unlock(&kvm_resv_lock);

	if (chunk < kvm_resv_chunks)
		return kvm_resv_addr + (chunk << KVM_RESV_CHUNK_ORDER);
	return 0;
}
EXPORT_SYMBOL_GPL(kvmhv_alloc_resv_hpt);

void kvmhv_release_resv_hpt(unsigned long addr, u32 order)
{
	unsigned long nr_chunks = 1ul << (order - KVM_RESV_CHUNK_ORDER);
	unsigned long chunk = (addr - kvm_resv_addr) >> KVM_RESV_CHUNK_ORDER;

	mutex_lock(&kvm_resv_lock);
	if (chunk + nr_chunks <= kvm_resv_chunks)
		bitmap_clear(kvm_resv_bitmap, chunk, nr_chunks);
	mutex_unlock(&kvm_resv_lock);
}
EXPORT_SYMBOL_GPL(kvmhv_release_resv_hpt);

#define KVM_CMA_CHUNK_ORDER	HPT_ALIGN_ORDER

/*
 * By default we reserve 3% of memory for the CMA zone.
 */
static unsigned long kvm_cma_resv_ratio = 3;

static struct cma *kvm_cma;

static int __init early_parse_kvm_cma_resv(char *p)
{
	pr_debug("%s(%s)\n", __func__, p);
	if (!p)
		return -EINVAL;
	return kstrtoul(p, 0, &kvm_cma_resv_ratio);
}
early_param("kvm_cma_resv_ratio", early_parse_kvm_cma_resv);

unsigned long kvmhv_alloc_cma_hpt(u32 order)
{
	unsigned long nr_pages = 1ul << (order - PAGE_SHIFT);
	struct page *page;

	VM_BUG_ON(order < KVM_CMA_CHUNK_ORDER);
	page = cma_alloc(kvm_cma, nr_pages, HPT_ALIGN_ORDER - PAGE_SHIFT);
	if (page)
		return (unsigned long)pfn_to_kaddr(page_to_pfn(page));
	return 0;
}
EXPORT_SYMBOL_GPL(kvmhv_alloc_cma_hpt);

void kvmhv_release_cma_hpt(unsigned long hpt, u32 order)
{
	unsigned long nr_pages = 1ul << (order - PAGE_SHIFT);
	struct page *page = virt_to_page(hpt);

	cma_release(kvm_cma, page, nr_pages);
}
EXPORT_SYMBOL_GPL(kvmhv_release_cma_hpt);

/**
 * kvm_cma_reserve() - reserve area for kvm hash pagetable
 *
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the memblock allocator
 * has been activated and all other subsystems have already allocated/reserved
 * memory.
 */
void __init kvm_cma_reserve(void)
{
	unsigned long align_size;
	phys_addr_t selected_size;

	/*
	 * We need CMA reservation only when we are in HV mode
	 */
	if (!cpu_has_feature(CPU_FTR_HVMODE))
		return;

	selected_size = memblock_phys_mem_size() * kvm_cma_resv_ratio / 100;
	selected_size = ALIGN(selected_size, 1ull << KVM_CMA_CHUNK_ORDER);
	if (selected_size) {
		pr_debug("%s: reserving %ld MiB for global area\n", __func__,
			 (unsigned long)selected_size / SZ_1M);
		align_size = HPT_ALIGN_PAGES << PAGE_SHIFT;
		cma_declare_contiguous(0, selected_size, 0, align_size,
			KVM_CMA_CHUNK_ORDER - PAGE_SHIFT, false, &kvm_cma);
	}
}

/*
 * Real-mode H_CONFER implementation.
 * We check if we are the only vcpu out of this virtual core
 * still running in the guest and not ceded.  If so, we pop up
 * to the virtual-mode implementation; if not, just return to
 * the guest.
 */
long int kvmppc_rm_h_confer(struct kvm_vcpu *vcpu, int target,
			    unsigned int yield_count)
{
	struct kvmppc_vcore *vc = local_paca->kvm_hstate.kvm_vcore;
	int ptid = local_paca->kvm_hstate.ptid;
	int threads_running;
	int threads_ceded;
	int threads_conferring;
	u64 stop = get_tb() + 10 * tb_ticks_per_usec;
	int rv = H_SUCCESS; /* => don't yield */

	set_bit(ptid, &vc->conferring_threads);
	while ((get_tb() < stop) && !VCORE_IS_EXITING(vc)) {
		threads_running = VCORE_ENTRY_MAP(vc);
		threads_ceded = vc->napping_threads;
		threads_conferring = vc->conferring_threads;
		if ((threads_ceded | threads_conferring) == threads_running) {
			rv = H_TOO_HARD; /* => do yield */
			break;
		}
	}
	clear_bit(ptid, &vc->conferring_threads);
	return rv;
}

/*
 * When running HV mode KVM we need to block certain operations while KVM VMs
 * exist in the system. We use a counter of VMs to track this.
 *
 * One of the operations we need to block is onlining of secondaries, so we
 * protect hv_vm_count with get/put_online_cpus().
 */
static atomic_t hv_vm_count;

void kvm_hv_vm_activated(void)
{
	get_online_cpus();
	atomic_inc(&hv_vm_count);
	put_online_cpus();
}
EXPORT_SYMBOL_GPL(kvm_hv_vm_activated);

void kvm_hv_vm_deactivated(void)
{
	get_online_cpus();
	atomic_dec(&hv_vm_count);
	put_online_cpus();
}
EXPORT_SYMBOL_GPL(kvm_hv_vm_deactivated);

bool kvm_hv_mode_active(void)
{
	return atomic_read(&hv_vm_count) != 0;
}

extern int hcall_real_table[], hcall_real_table_end[];

int kvmppc_hcall_impl_hv_realmode(unsigned long cmd)
{
	cmd /= 4;
	if (cmd < hcall_real_table_end - hcall_real_table &&
	    hcall_real_table[cmd])
		return 1;

	return 0;
}
EXPORT_SYMBOL_GPL(kvmppc_hcall_impl_hv_realmode);

int kvmppc_hwrng_present(void)
{
	return powernv_hwrng_present();
}
EXPORT_SYMBOL_GPL(kvmppc_hwrng_present);

long kvmppc_h_random(struct kvm_vcpu *vcpu)
{
	if (powernv_get_random_real_mode(&vcpu->arch.gpr[4]))
		return H_SUCCESS;

	return H_HARDWARE;
}

static inline void rm_writeb(unsigned long paddr, u8 val)
{
	__asm__ __volatile__("stbcix %0,0,%1"
		: : "r" (val), "r" (paddr) : "memory");
}

/*
 * Send an interrupt or message to another CPU.
 * This can only be called in real mode.
 * The caller needs to include any barrier needed to order writes
 * to memory vs. the IPI/message.
 */
void kvmhv_rm_send_ipi(int cpu)
{
	unsigned long xics_phys;

	/* On POWER8 for IPIs to threads in the same core, use msgsnd */
	if (cpu_has_feature(CPU_FTR_ARCH_207S) &&
	    cpu_first_thread_sibling(cpu) ==
	    cpu_first_thread_sibling(raw_smp_processor_id())) {
		unsigned long msg = PPC_DBELL_TYPE(PPC_DBELL_SERVER);
		msg |= cpu_thread_in_core(cpu);
		__asm__ __volatile__ (PPC_MSGSND(%0) : : "r" (msg));
		return;
	}

	/* Else poke the target with an IPI */
	xics_phys = paca[cpu].kvm_hstate.xics_phys;
	rm_writeb(xics_phys + XICS_MFRR, IPI_PRIORITY);
}

/*
 * The following functions are called from the assembly code
 * in book3s_hv_rmhandlers.S.
 */
static void kvmhv_interrupt_vcore(struct kvmppc_vcore *vc, int active)
{
	int cpu = vc->pcpu;

	/* Order setting of exit map vs. msgsnd/IPI */
	smp_mb();
	for (; active; active >>= 1, ++cpu)
		if (active & 1)
			kvmhv_rm_send_ipi(cpu);
}

void kvmhv_commence_exit(int trap)
{
	struct kvmppc_vcore *vc = local_paca->kvm_hstate.kvm_vcore;
	int ptid = local_paca->kvm_hstate.ptid;
	struct kvm_split_mode *sip = local_paca->kvm_hstate.kvm_split_mode;
	int me, ee, i;

	/* Set our bit in the threads-exiting-guest map in the 0xff00
	   bits of vcore->entry_exit_map */
	me = 0x100 << ptid;
	do {
		ee = vc->entry_exit_map;
	} while (cmpxchg(&vc->entry_exit_map, ee, ee | me) != ee);

	/* Are we the first here? */
	if ((ee >> 8) != 0)
		return;

	/*
	 * Trigger the other threads in this vcore to exit the guest.
	 * If this is a hypervisor decrementer interrupt then they
	 * will be already on their way out of the guest.
	 */
	if (trap != BOOK3S_INTERRUPT_HV_DECREMENTER)
		kvmhv_interrupt_vcore(vc, ee & ~(1 << ptid));

	/*
	 * If we are doing dynamic micro-threading, interrupt the other
	 * subcores to pull them out of their guests too.
	 */
	if (!sip)
		return;

	for (i = 0; i < MAX_SUBCORES; ++i) {
		vc = sip->master_vcs[i];
		if (!vc)
			break;
		do {
			ee = vc->entry_exit_map;
			/* Already asked to exit? */
			if ((ee >> 8) != 0)
				break;
		} while (cmpxchg(&vc->entry_exit_map, ee,
				 ee | VCORE_EXIT_REQ) != ee);
		if ((ee >> 8) == 0)
			kvmhv_interrupt_vcore(vc, ee);
	}
}

struct kvmppc_host_rm_ops *kvmppc_host_rm_ops_hv;
EXPORT_SYMBOL_GPL(kvmppc_host_rm_ops_hv);

static struct kvmppc_irq_map *get_irqmap(struct kvmppc_passthru_map *pmap,
					 u32 xisr)
{
	int i;

	/*
	 * We can access this array unsafely because if there
	 * is a pending IRQ, its mapping cannot be removed
	 * and replaced with a new mapping (that corresponds to a
	 * different device) while we are accessing it. After
	 * unmappping, we do a kick_all_cpus_sync which guarantees
	 * that we don't see a stale value in here.
	 *
	 * Since we don't take a lock, we might skip over or read
	 * more than the available entries in here (if a different
	 * entry here is * being deleted), and we might thus miss
	 * our hwirq, but we can never get a bad mapping. Missing
	 * an entry is not fatal, in this case, we simply fall back
	 * on the default interrupt handling mechanism - that is,
	 * this interrupt goes through VFIO.
	 *
	 * We have also carefully ordered the stores in the writer
	 * and the loads here in the reader, so that if we find a matching
	 * hwirq here, the associated GSI field is valid.
	 */
	for (i = 0; i < pmap->n_map_irq; i++)  {
		if (xisr == pmap->irq_map[i].r_hwirq) {
			/*
			 * Order subsequent reads in the caller to serialize
			 * with the writer.
			 */
			smp_rmb();
			return &pmap->irq_map[i];
		}
	}
	return NULL;
}

/*
 * Determine what sort of external interrupt is pending (if any).
 * Returns:
 *	0 if no interrupt is pending
 *	1 if an interrupt is pending that needs to be handled by the host
 *	2 Passthrough that needs completion in the host
 *	-1 if there was a guest wakeup IPI (which has now been cleared)
 *	-2 if there is PCI passthrough external interrupt that was handled
 */

long kvmppc_read_intr(struct kvm_vcpu *vcpu, int path)
{
	unsigned long xics_phys;
	u32 h_xirr, xirr;
	u32 xisr;
	struct kvmppc_passthru_map *pmap;
	struct kvmppc_irq_map *irq_map;
	int r;
	u8 host_ipi;

	/* see if a host IPI is pending */
	host_ipi = local_paca->kvm_hstate.host_ipi;
	if (host_ipi)
		return 1;

	/* Now read the interrupt from the ICP */
	xics_phys = local_paca->kvm_hstate.xics_phys;
	if (unlikely(!xics_phys))
		return 1;

	/*
	 * Save XIRR for later. Since we get control in reverse endian
	 * on LE systems, save it byte reversed and fetch it back in
	 * host endian. Note that xirr is the value read from the
	 * XIRR register, while h_xirr is the host endian version.
	 */
	xirr = _lwzcix(xics_phys + XICS_XIRR);
#ifdef __LITTLE_ENDIAN__
	st_le32(&local_paca->kvm_hstate.saved_xirr, xirr);
	h_xirr = local_paca->kvm_hstate.saved_xirr;
#else
	local_paca->kvm_hstate.saved_xirr = xirr;
	h_xirr = xirr;
#endif
	xisr = h_xirr & 0xffffff;
	/*
	 * Ensure that the store/load complete to guarantee all side
	 * effects of loading from XIRR has completed
	 */
	smp_mb();

	/* if nothing pending in the ICP */
	if (!xisr)
		return 0;

	/* We found something in the ICP...
	 *
	 * If it is an IPI, clear the MFRR and EOI it.
	 */
	if (xisr == XICS_IPI) {
		_stbcix(xics_phys + XICS_MFRR, 0xff);
		_stwcix(xics_phys + XICS_XIRR, xirr);
		/*
		 * Need to ensure side effects of above stores
		 * complete before proceeding.
		 */
		smp_mb();

		/*
		 * We need to re-check host IPI now in case it got set in the
		 * meantime. If it's clear, we bounce the interrupt to the
		 * guest
		 */
		host_ipi = local_paca->kvm_hstate.host_ipi;
		if (unlikely(host_ipi != 0)) {
			/* We raced with the host,
			 * we need to resend that IPI, bummer
			 */
			_stbcix(xics_phys + XICS_MFRR, IPI_PRIORITY);
			/* Let side effects complete */
			smp_mb();
			return 1;
		}

		/* OK, it's an IPI for us */
		local_paca->kvm_hstate.saved_xirr = 0;
		return -1;
	}

	/*
	 * If it's not an IPI, check if we have a passthrough adapter and
	 * if so, check if this external interrupt is for the adapter.
	 * We will attempt to deliver the IRQ directly to the target VCPU's
	 * ICP, the virtual ICP (based on affinity - the xive value in ICS).
	 *
	 * If the delivery fails or if this is not for a passthrough adapter,
	 * return to the host to handle this interrupt. We earlier
	 * saved a copy of the XIRR in the PACA, it will be picked up by
	 * the host ICP driver
	 */
	pmap = kvmppc_get_passthru_map(vcpu);
	if (pmap) {
		irq_map = get_irqmap(pmap, xisr);
		if (irq_map) {
			r = kvmppc_deliver_irq_passthru(vcpu, xirr,
								irq_map, pmap);
			return r;
		}
	}

	return 1;
}
