/*-
 * Copyright (c) 2004-2010 Juli Mallett <jmallett@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef	_MACHINE_PTE_H_
#define	_MACHINE_PTE_H_

#ifndef _LOCORE
	/* 32-bit PTE.  */
typedef	uint32_t pt_entry_t;

	/* Page directory entry.  */
typedef	pt_entry_t *pd_entry_t;
#endif

/*
 * TLB and PTE management.  Most things operate within the context of
 * EntryLo0,1, and begin with TLBLO_.  Things which work with EntryHi
 * start with TLBHI_.  PTE bits begin with PG_.
 *
 * Note that we use the same size VM and TLB pages.
 */
#define	TLB_PAGE_SHIFT	(PAGE_SHIFT)
#define	TLB_PAGE_SIZE	(1 << TLB_PAGE_SHIFT)
#define	TLB_PAGE_MASK	(TLB_PAGE_SIZE - 1)

/*
 * TLB PageMask register.  Has mask bits set above the default, 4K, page mask.
 */
#define	TLBMASK_SHIFT	(13)
#define	TLBMASK_MASK	((PAGE_MASK >> TLBMASK_SHIFT) << TLBMASK_SHIFT)

/*
 * XXX This comment is not correct for FreeBSD.
 *
 * PFN for EntryLo register.  Upper bits are 0, which is to say that
 * bit 29 is the last hardware bit;  Bits 30 and upwards (EntryLo is
 * 64 bit though it can be referred to in 32-bits providing 2 software
 * bits safely.  We use it as 64 bits to get many software bits, and
 * god knows what else.) are unacknowledged by hardware.  They may be
 * written as anything, but otherwise they have as much meaning as
 * other 0 fields.
 */
#define	TLBLO_SWBITS_SHIFT	(30)
#define	TLBLO_SWBITS_MASK	(0x3U << TLBLO_SWBITS_SHIFT)
#define	TLBLO_PFN_SHIFT		(6)
#define	TLBLO_PFN_MASK		(0x3FFFFFC0)
#define	TLBLO_PA_TO_PFN(pa)	((((pa) >> TLB_PAGE_SHIFT) << TLBLO_PFN_SHIFT) & TLBLO_PFN_MASK)
#define	TLBLO_PFN_TO_PA(pfn)	((vm_paddr_t)((pfn) >> TLBLO_PFN_SHIFT) << TLB_PAGE_SHIFT)
#define	TLBLO_PTE_TO_PFN(pte)	((pte) & TLBLO_PFN_MASK)
#define	TLBLO_PTE_TO_PA(pte)	(TLBLO_PFN_TO_PA(TLBLO_PTE_TO_PFN((pte))))

/*
 * XXX This comment is not correct for anything more modern than R4K.
 *
 * VPN for EntryHi register.  Upper two bits select user, supervisor,
 * or kernel.  Bits 61 to 40 copy bit 63.  VPN2 is bits 39 and down to
 * as low as 13, down to PAGE_SHIFT, to index 2 TLB pages*.  From bit 12
 * to bit 8 there is a 5-bit 0 field.  Low byte is ASID.
 *
 * XXX This comment is not correct for FreeBSD.
 * Note that in FreeBSD, we map 2 TLB pages is equal to 1 VM page.
 */
#define	TLBHI_ASID_MASK		(0xff)
#define	TLBHI_VPN2_SHIFT	(TLB_PAGE_SHIFT + 1)
#if defined(__mips_n64)
#define	TLBHI_R_SHIFT		62
#define	TLBHI_R_USER		(0x00UL << TLBHI_R_SHIFT)
#define	TLBHI_R_SUPERVISOR	(0x01UL << TLBHI_R_SHIFT)
#define	TLBHI_R_KERNEL		(0x03UL << TLBHI_R_SHIFT)
#define	TLBHI_R_MASK		(0x03UL << TLBHI_R_SHIFT)
#define	TLBHI_VA_R(va)		((va) & TLBHI_R_MASK)
#define	TLBHI_FILL_SHIFT	40
#define	TLBHI_VPN2_MASK		(((~((1UL << TLBHI_VPN2_SHIFT) - 1)) << (63 - TLBHI_FILL_SHIFT)) >> (63 - TLBHI_FILL_SHIFT))
#define	TLBHI_VA_TO_VPN2(va)	((va) & TLBHI_VPN2_MASK)
#define	TLBHI_ENTRY(va, asid)	((TLBHI_VA_R((va))) /* Region. */ | \
				 (TLBHI_VA_TO_VPN2((va))) /* VPN2. */ | \
				 ((asid) & TLBHI_ASID_MASK))
#else
#define	TLBHI_ENTRY(va, asid)	(((va) & ~(1 << TLBHI_VPN2_SHIFT)) | ((asid) & TLBHI_ASID_MASK))
#endif

/*
 * TLB flags managed in hardware:
 * 	C:	Cache attribute.  Broken out into its own section.
 * 	D:	Dirty bit.  This means a page is writable.  It is not
 * 		set at first, and a write is trapped, and the dirty
 * 		bit is set.  See also PTE_RO.
 * 	V:	Valid bit.  Obvious, isn't it?
 * 	G:	Global bit.  This means that this mapping is present
 * 		in EVERY address space, and to ignore the ASID when
 * 		it is matched.
 */
#define	PTE_C(attr)	((attr & 0x07) << 3)
#define	PTE_C_MASK	(PTE_C(0x07))
#define	PTE_D		0x04
#define	PTE_V		0x02
#define	PTE_G		0x01

/*
 * TLB cache attributtes:
 *	UC:	Uncached.
 *	UA:	Uncached accelerated.
 *	C:	Cacheable, coherency unspecified.
 *	CNC:	Cacheable non-coherent.
 *	CC:	Cacheable coherent.
 *	CCE:	Cacheable coherent, exclusive read.
 *	CCEW:	Cacheable coherent, exclusive write.
 *	CCUOW:	Cacheable coherent, update on write.
 *
 * Note that some bits vary in meaning across implementations (and that the
 * listing here is no doubt incomplete) and that the optimal cached mode varies
 * between implementations.  0x02 is required to be UC and 0x03 is required to
 * be a least C.
 *
 * We define the following logical bits:
 * 	UNCACHED:
 * 		The optimal uncached mode for the target CPU type.  This must
 * 		be suitable for use in accessing memory-mapped devices.
 * 	CACHE:	The optional cached mode for the target CPU type.
 */
#define	PTE_C_UC	(PTE_C(0x02))
#define	PTE_C_C		(PTE_C(0x03))

#if defined(CPU_R4000) || defined(CPU_R10000)
#define	PTE_C_CNC	(PTE_C(0x03))
#define	PTE_C_CCE	(PTE_C(0x04))
#define	PTE_C_CCEW	(PTE_C(0x05))

#ifdef CPU_R4000
#define	PTE_C_CCUOW	(PTE_C(0x06))
#endif

#ifdef CPU_R10000
#define	PTE_C_UA	(PTE_C(0x07))
#endif

#define	PTE_C_CACHE	PTE_C_CCEW
#endif /* defined(CPU_R4000) || defined(CPU_R10000) */

#if defined(CPU_SB1)
#define	PTE_C_CC	(PTE_C(0x05))
#endif

#ifndef	PTE_C_UNCACHED
#define	PTE_C_UNCACHED	PTE_C_UC
#endif

/*
 * If we don't know which cached mode to use and there is a cache coherent
 * mode, use it.  If there is not a cache coherent mode, use the required
 * cacheable mode.
 */
#ifndef PTE_C_CACHE
#ifdef PTE_C_CC
#define	PTE_C_CACHE	PTE_C_CC
#else
#define	PTE_C_CACHE	PTE_C_C
#endif
#endif

/*
 * VM flags managed in software:
 * 	RO:	Read only.  Never set PTE_D on this page, and don't
 * 		listen to requests to write to it.
 * 	W:	Wired.  Allows us to quickly increment and decrement
 * 		the wired count by looking at the PTE and skip wired
 * 		mappings when removing mappings from a process.
 */
#define	PTE_RO	(0x01 << TLBLO_SWBITS_SHIFT)
#define	PTE_W	(0x02 << TLBLO_SWBITS_SHIFT)

/*
 * PTE management functions for bits defined above.
 *
 * XXX Can make these atomics, but some users of them are using PTEs in local
 * registers and such and don't need the overhead.
 */
#define	pte_clear(pte, bit)	((*pte) &= ~(bit))
#define	pte_set(pte, bit)	((*pte) |= (bit))
#define	pte_test(pte, bit)	(((*pte) & (bit)) == (bit))

#endif /* !_MACHINE_PTE_H_ */
