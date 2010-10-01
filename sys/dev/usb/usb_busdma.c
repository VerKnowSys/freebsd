/* $FreeBSD$ */
/*-
 * Copyright (c) 2008 Hans Petter Selasky. All rights reserved.
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
 */

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/linker_set.h>
#include <sys/module.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>

#define	USB_DEBUG_VAR usb_debug

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_util.h>
#include <dev/usb/usb_debug.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#if USB_HAVE_BUSDMA
static void	usb_dma_tag_create(struct usb_dma_tag *, usb_size_t,
		    usb_size_t);
static void	usb_dma_tag_destroy(struct usb_dma_tag *);
static void	usb_dma_lock_cb(void *, bus_dma_lock_op_t);
static void	usb_pc_alloc_mem_cb(void *, bus_dma_segment_t *, int, int);
static void	usb_pc_load_mem_cb(void *, bus_dma_segment_t *, int, int);
static void	usb_pc_common_mem_cb(void *, bus_dma_segment_t *, int, int,
		    uint8_t, uint8_t);
#endif

/*------------------------------------------------------------------------*
 *  usbd_get_page - lookup DMA-able memory for the given offset
 *
 * NOTE: Only call this function when the "page_cache" structure has
 * been properly initialized !
 *------------------------------------------------------------------------*/
void
usbd_get_page(struct usb_page_cache *pc, usb_frlength_t offset,
    struct usb_page_search *res)
{
	struct usb_page *page;

#if USB_HAVE_BUSDMA
	if ((page = pc->page_start) != NULL) {
		USB_ASSERT(pc->buffer != NULL, ("pc->buffer is NULL"));
		res->buffer = USB_ADD_BYTES(pc->buffer, offset);
		offset += pc->page_offset_buf;
		if (pc->ismultiseg) {
			page += (offset / USB_PAGE_SIZE);
			offset %= USB_PAGE_SIZE;
			res->length = USB_PAGE_SIZE - offset;
			res->physaddr = page->physaddr + offset;
		} else {
			res->length = 0 - 1;
			res->physaddr = page->physaddr + offset;
		}
		return;
	}
#endif
	res->buffer = USB_ADD_BYTES(pc->buffer, offset);
	res->length = 0 - 1;
#if USB_HAVE_BUSDMA
	res->physaddr = 0;
#endif
}

/*------------------------------------------------------------------------*
 *  usbd_copy_in - copy directly to DMA-able memory
 *------------------------------------------------------------------------*/
void
usbd_copy_in(struct usb_page_cache *cache, usb_frlength_t offset,
    const void *ptr, usb_frlength_t len)
{
	struct usb_page_search res;

	while (len != 0) {
		usbd_get_page(cache, offset, &res);
		if (res.length > len)
			res.length = len;

		/* Checks the buffer boundary */
		USB_ASSERT((char *)res.buffer + res.length <=
		    (char *)cache->buffer + cache->buflen,
		    ("overflow is happened (%p %d/%p %d)", res.buffer,
			res.length, cache->buffer, cache->buflen));

		bcopy(ptr, res.buffer, res.length);

		offset += res.length;
		len -= res.length;
		ptr = USB_ADD_BYTES(ptr, res.length);
	}
}

/*------------------------------------------------------------------------*
 *  usbd_copy_in_user - copy directly to DMA-able memory from userland
 *
 * Return values:
 *    0: Success
 * Else: Failure
 *------------------------------------------------------------------------*/
#if USB_HAVE_USER_IO
int
usbd_copy_in_user(struct usb_page_cache *cache, usb_frlength_t offset,
    const void *ptr, usb_frlength_t len)
{
	struct usb_page_search res;
	int error;

	while (len != 0) {
		usbd_get_page(cache, offset, &res);
		if (res.length > len)
			res.length = len;

		/* Checks the buffer boundary */
		USB_ASSERT((char *)res.buffer + res.length <=
		    (char *)cache->buffer + cache->buflen,
		    ("overflow is happened (%p %d/%p %d)", res.buffer,
			res.length, cache->buffer, cache->buflen));

		error = copyin(ptr, res.buffer, res.length);
		if (error)
			return (error);

		offset += res.length;
		len -= res.length;
		ptr = USB_ADD_BYTES(ptr, res.length);
	}
	return (0);			/* success */
}
#endif

/*------------------------------------------------------------------------*
 *  usbd_m_copy_in - copy a mbuf chain directly into DMA-able memory
 *------------------------------------------------------------------------*/
#if USB_HAVE_MBUF
struct usb_m_copy_in_arg {
	struct usb_page_cache *cache;
	usb_frlength_t dst_offset;
};

static int
usbd_m_copy_in_cb(void *arg, void *src, uint32_t count)
{
	register struct usb_m_copy_in_arg *ua = arg;

	usbd_copy_in(ua->cache, ua->dst_offset, src, count);
	ua->dst_offset += count;
	return (0);
}

void
usbd_m_copy_in(struct usb_page_cache *cache, usb_frlength_t dst_offset,
    struct mbuf *m, usb_size_t src_offset, usb_frlength_t src_len)
{
	struct usb_m_copy_in_arg arg = {cache, dst_offset};
	int error;

	error = m_apply(m, src_offset, src_len, &usbd_m_copy_in_cb, &arg);
}
#endif

/*------------------------------------------------------------------------*
 *  usb_uiomove - factored out code
 *------------------------------------------------------------------------*/
#if USB_HAVE_USER_IO
int
usb_uiomove(struct usb_page_cache *pc, struct uio *uio,
    usb_frlength_t pc_offset, usb_frlength_t len)
{
	struct usb_page_search res;
	int error = 0;

	while (len != 0) {
		usbd_get_page(pc, pc_offset, &res);
		if (res.length > len)
			res.length = len;

		/* Checks the buffer boundary */
		USB_ASSERT((char *)res.buffer + res.length <=
		    (char *)pc->buffer + pc->buflen,
		    ("overflow is happened (%p %d/%p %d)", res.buffer,
			res.length, pc->buffer, pc->buflen));

		/*
		 * "uiomove()" can sleep so one needs to make a wrapper,
		 * exiting the mutex and checking things
		 */
		error = uiomove(res.buffer, res.length, uio);
		if (error) {
			break;
		}
		pc_offset += res.length;
		len -= res.length;
	}
	return (error);
}
#endif

/*------------------------------------------------------------------------*
 *  usbd_copy_out - copy directly from DMA-able memory
 *------------------------------------------------------------------------*/
void
usbd_copy_out(struct usb_page_cache *cache, usb_frlength_t offset,
    void *ptr, usb_frlength_t len)
{
	struct usb_page_search res;

	while (len != 0) {
		usbd_get_page(cache, offset, &res);
		if (res.length > len)
			res.length = len;

		/* Checks the buffer boundary */
		USB_ASSERT((char *)res.buffer + res.length <=
		    (char *)cache->buffer + cache->buflen,
		    ("overflow is happened (%p %d/%p %d)", res.buffer,
			res.length, cache->buffer, cache->buflen));

		bcopy(res.buffer, ptr, res.length);

		offset += res.length;
		len -= res.length;
		ptr = USB_ADD_BYTES(ptr, res.length);
	}
}

/*------------------------------------------------------------------------*
 *  usbd_copy_out_user - copy directly from DMA-able memory to userland
 *
 * Return values:
 *    0: Success
 * Else: Failure
 *------------------------------------------------------------------------*/
#if USB_HAVE_USER_IO
int
usbd_copy_out_user(struct usb_page_cache *cache, usb_frlength_t offset,
    void *ptr, usb_frlength_t len)
{
	struct usb_page_search res;
	int error;

	while (len != 0) {
		usbd_get_page(cache, offset, &res);
		if (res.length > len)
			res.length = len;

		/* Checks the buffer boundary */
		USB_ASSERT((char *)res.buffer + res.length <=
		    (char *)cache->buffer + cache->buflen,
		    ("overflow is happened (%p %d/%p %d)", res.buffer,
			res.length, cache->buffer, cache->buflen));

		error = copyout(res.buffer, ptr, res.length);
		if (error)
			return (error);

		offset += res.length;
		len -= res.length;
		ptr = USB_ADD_BYTES(ptr, res.length);
	}
	return (0);			/* success */
}
#endif

/*------------------------------------------------------------------------*
 *  usbd_frame_zero - zero DMA-able memory
 *------------------------------------------------------------------------*/
void
usbd_frame_zero(struct usb_page_cache *cache, usb_frlength_t offset,
    usb_frlength_t len)
{
	struct usb_page_search res;

	while (len != 0) {
		usbd_get_page(cache, offset, &res);
		if (res.length > len)
			res.length = len;

		/* Checks the buffer boundary */
		USB_ASSERT((char *)res.buffer + res.length <=
		    (char *)cache->buffer + cache->buflen,
		    ("overflow is happened (%p %d/%p %d)", res.buffer,
			res.length, cache->buffer, cache->buflen));

		bzero(res.buffer, res.length);

		offset += res.length;
		len -= res.length;
	}
}

#if USB_HAVE_BUSDMA

/*------------------------------------------------------------------------*
 *	usb_dma_lock_cb - dummy callback
 *------------------------------------------------------------------------*/
static void
usb_dma_lock_cb(void *arg, bus_dma_lock_op_t op)
{
	/* we use "mtx_owned()" instead of this function */
}

/*------------------------------------------------------------------------*
 *	usb_dma_tag_create - allocate a DMA tag
 *
 * NOTE: If the "align" parameter has a value of 1 the DMA-tag will
 * allow multi-segment mappings. Else all mappings are single-segment.
 *------------------------------------------------------------------------*/
static void
usb_dma_tag_create(struct usb_dma_tag *udt,
    usb_size_t size, usb_size_t align)
{
	bus_dma_tag_t tag;

	if (bus_dma_tag_create
	    ( /* parent    */ udt->tag_parent->tag,
	     /* alignment */ align,
	     /* boundary  */ (align == 1) ?
	    USB_PAGE_SIZE : 0,
	     /* lowaddr   */ (2ULL << (udt->tag_parent->dma_bits - 1)) - 1,
	     /* highaddr  */ BUS_SPACE_MAXADDR,
	     /* filter    */ NULL,
	     /* filterarg */ NULL,
	     /* maxsize   */ size,
	     /* nsegments */ (align == 1) ?
	    (2 + (size / USB_PAGE_SIZE)) : 1,
	     /* maxsegsz  */ (align == 1) ?
	    USB_PAGE_SIZE : size,
	     /* flags     */ BUS_DMA_KEEP_PG_OFFSET,
	     /* lockfn    */ &usb_dma_lock_cb,
	     /* lockarg   */ NULL,
	    &tag)) {
		tag = NULL;
	}
	udt->tag = tag;
}

/*------------------------------------------------------------------------*
 *	usb_dma_tag_free - free a DMA tag
 *------------------------------------------------------------------------*/
static void
usb_dma_tag_destroy(struct usb_dma_tag *udt)
{

	bus_dma_tag_destroy(udt->tag);
}

/*------------------------------------------------------------------------*
 *	usb_pc_alloc_mem_cb - BUS-DMA callback function
 *------------------------------------------------------------------------*/
static void
usb_pc_alloc_mem_cb(void *arg, bus_dma_segment_t *segs,
    int nseg, int error)
{

	usb_pc_common_mem_cb(arg, segs, nseg, error, 0, 1);
}

/*------------------------------------------------------------------------*
 *	usb_pc_load_mem_cb - BUS-DMA callback function
 *------------------------------------------------------------------------*/
static void
usb_pc_load_mem_cb(void *arg, bus_dma_segment_t *segs,
    int nseg, int error)
{

	usb_pc_common_mem_cb(arg, segs, nseg, error, 1, 0);
}

/*------------------------------------------------------------------------*
 *	usb_pc_common_mem_cb - BUS-DMA callback function
 *------------------------------------------------------------------------*/
static void
usb_pc_common_mem_cb(void *arg, bus_dma_segment_t *segs,
    int nseg, int error, uint8_t isload, uint8_t needwakeup)
{
	struct usb_dma_parent_tag *uptag;
	struct usb_page_cache *pc;
	struct usb_page *pg;
	usb_size_t rem;
	int i;

	pc = arg;
	uptag = pc->tag_parent;

	mtx_assert(uptag->mtx, MA_OWNED);

	if (error)
		goto done;

	pg = pc->page_start;
	pg->physaddr = segs[0].ds_addr & ~(USB_PAGE_SIZE - 1);
	rem = segs[0].ds_addr & (USB_PAGE_SIZE - 1);
	pc->page_offset_buf = rem;
	pc->page_offset_end += rem;
#ifdef USB_DEBUG
	if (rem != (USB_P2U(pc->buffer) & (USB_PAGE_SIZE - 1))) {
		/*
		 * This check verifies that the physical address is correct:
		 */
		DPRINTFN(0, "Page offset was not preserved\n");
		error = 1;
		goto done;
	}
#endif
	for (i = 1; i < nseg; i++)
		pg[i].physaddr = segs[i].ds_addr & ~(USB_PAGE_SIZE - 1);
done:
	uptag->dma_error = (error ? 1 : 0);
	if (isload)
		(uptag->func) (uptag);
	if (needwakeup != 0)
		cv_broadcast(uptag->cv);
}

/*------------------------------------------------------------------------*
 *	usb_pc_alloc_mem - allocate DMA'able memory
 *
 * Returns:
 *    0: Success
 * Else: Failure
 *------------------------------------------------------------------------*/
uint8_t
usb_pc_alloc_mem(struct usb_page_cache *pc, struct usb_page *pg,
    usb_size_t size, usb_size_t align)
{
	struct usb_dma_parent_tag *uptag;
	struct usb_dma_tag *utag;
	bus_dmamap_t map;
	void *ptr;
	int err;

	uptag = pc->tag_parent;

	/*
	 * Checks the requested size first before allocating DMA-able buffer
	 * that if the size is over 2G the alignment value could be overflowed.
	 */
	if (size >= INT_MAX)
		panic("too big size (%d) for DMA-able buffer", size);
	if (align == 0)
		goto error;
	if (align != 1) {
		/*
	         * The alignment must be greater or equal to the
	         * "size" else the object can be split between two
	         * memory pages and we get a problem!
	         */
		while (align < size)
			align *= 2;
#if 1
		/*
		 * XXX BUS-DMA workaround - FIXME later:
		 *
		 * We assume that that the aligment at this point of
		 * the code is greater than or equal to the size and
		 * less than two times the size, so that if we double
		 * the size, the size will be greater than the
		 * alignment.
		 *
		 * The bus-dma system has a check for "alignment"
		 * being less than "size". If that check fails we end
		 * up using contigmalloc which is page based even for
		 * small allocations. Try to avoid that to save
		 * memory, hence we sometimes to a large number of
		 * small allocations!
		 */
		if (size <= (USB_PAGE_SIZE / 2))
			size *= 2;
#endif
	}
	/* get the correct DMA tag */
	utag = usb_dma_tag_find(uptag, size, align);
	if (utag == NULL)
		goto error;
	/* allocate memory */
	if (bus_dmamem_alloc(utag->tag, &ptr,
	    (BUS_DMA_WAITOK | BUS_DMA_COHERENT), &map))
		goto error;
	/* setup page cache */
	pc->buffer = ptr;
	pc->buflen = size;
	pc->page_start = pg;
	pc->page_offset_buf = 0;
	pc->page_offset_end = size;
	pc->map = map;
	pc->tag = utag->tag;
	pc->ismultiseg = (align == 1);

	mtx_lock(uptag->mtx);

	/* load memory into DMA */
	err = bus_dmamap_load(utag->tag, map, ptr, size, &usb_pc_alloc_mem_cb,
	    pc, (BUS_DMA_WAITOK | BUS_DMA_COHERENT));
	if (err == EINPROGRESS) {
		cv_wait(uptag->cv, uptag->mtx);
		err = 0;
	}

	mtx_unlock(uptag->mtx);

	if (err || uptag->dma_error) {
		bus_dmamem_free(utag->tag, ptr, map);
		goto error;
	}
	bzero(ptr, size);

	usb_pc_cpu_flush(pc);

	return (0);

error:
	/* reset most of the page cache */
	pc->buffer = NULL;
	pc->buflen = 0;
	pc->page_start = NULL;
	pc->page_offset_buf = 0;
	pc->page_offset_end = 0;
	pc->map = NULL;
	pc->tag = NULL;
	return (1);
}

/*------------------------------------------------------------------------*
 *	usb_pc_free_mem - free DMA memory
 *
 * This function is NULL safe.
 *------------------------------------------------------------------------*/
void
usb_pc_free_mem(struct usb_page_cache *pc)
{

	if (pc && pc->buffer) {
		bus_dmamap_unload(pc->tag, pc->map);
		bus_dmamem_free(pc->tag, pc->buffer, pc->map);
		pc->buffer = NULL;
		pc->buflen = 0;
	}
}

/*------------------------------------------------------------------------*
 *	usb_pc_load_mem - load virtual memory into DMA
 *
 * Return values:
 * 0: Success
 * Else: Error
 *------------------------------------------------------------------------*/
uint8_t
usb_pc_load_mem(struct usb_page_cache *pc, usb_size_t size, uint8_t sync)
{

	/* setup page cache */
	pc->page_offset_buf = 0;
	pc->page_offset_end = size;
	pc->ismultiseg = 1;

	mtx_assert(pc->tag_parent->mtx, MA_OWNED);

	if (size > 0) {
		if (sync) {
			struct usb_dma_parent_tag *uptag;
			int err;

			uptag = pc->tag_parent;

			/*
			 * We have to unload the previous loaded DMA
			 * pages before trying to load a new one!
			 */
			bus_dmamap_unload(pc->tag, pc->map);

			/*
			 * Try to load memory into DMA.
			 */
			err = bus_dmamap_load(pc->tag, pc->map, pc->buffer,
			    size, &usb_pc_alloc_mem_cb, pc, BUS_DMA_WAITOK);
			if (err == EINPROGRESS) {
				cv_wait(uptag->cv, uptag->mtx);
				err = 0;
			}
			if (err || uptag->dma_error)
				return (1);
		} else {

			/*
			 * We have to unload the previous loaded DMA
			 * pages before trying to load a new one!
			 */
			bus_dmamap_unload(pc->tag, pc->map);

			/*
			 * Try to load memory into DMA. The callback
			 * will be called in all cases:
			 */
			(void)bus_dmamap_load(pc->tag, pc->map, pc->buffer,
			    size, &usb_pc_load_mem_cb, pc, BUS_DMA_WAITOK);
		}
	} else {
		if (!sync) {
			/*
			 * Call callback so that refcount is decremented
			 * properly:
			 */
			pc->tag_parent->dma_error = 0;
			(pc->tag_parent->func) (pc->tag_parent);
		}
	}
	return (0);
}

/*------------------------------------------------------------------------*
 *	usb_pc_cpu_invalidate - invalidate CPU cache
 *------------------------------------------------------------------------*/
void
usb_pc_cpu_invalidate(struct usb_page_cache *pc)
{

	if (pc->page_offset_end == pc->page_offset_buf) {
		/* nothing has been loaded into this page cache! */
		return;
	}

	/*
	 * TODO: We currently do XXX_POSTREAD and XXX_PREREAD at the
	 * same time, but in the future we should try to isolate the
	 * different cases to optimise the code. --HPS
	 */
	bus_dmamap_sync(pc->tag, pc->map, BUS_DMASYNC_POSTREAD);
	bus_dmamap_sync(pc->tag, pc->map, BUS_DMASYNC_PREREAD);
}

/*------------------------------------------------------------------------*
 *	usb_pc_cpu_flush - flush CPU cache
 *------------------------------------------------------------------------*/
void
usb_pc_cpu_flush(struct usb_page_cache *pc)
{

	if (pc->page_offset_end == pc->page_offset_buf) {
		/* nothing has been loaded into this page cache! */
		return;
	}
	bus_dmamap_sync(pc->tag, pc->map, BUS_DMASYNC_PREWRITE);
}

/*------------------------------------------------------------------------*
 *	usb_pc_dmamap_create - create a DMA map
 *
 * Returns:
 *    0: Success
 * Else: Failure
 *------------------------------------------------------------------------*/
uint8_t
usb_pc_dmamap_create(struct usb_page_cache *pc, usb_size_t size)
{
	struct usb_xfer_root *info;
	struct usb_dma_tag *utag;

	/* get info and sanity check */
	info = USB_DMATAG_TO_XROOT(pc->tag_parent);
	if (info == NULL)
		goto error;
	utag = usb_dma_tag_find(pc->tag_parent, size, 1);
	if (utag == NULL)
		goto error;
	/* create DMA map */
	if (bus_dmamap_create(utag->tag, 0, &pc->map))
		goto error;
	pc->tag = utag->tag;
	return 0;			/* success */
error:
	pc->map = NULL;
	pc->tag = NULL;
	return 1;			/* failure */
}

/*------------------------------------------------------------------------*
 *	usb_pc_dmamap_destroy
 *
 * This function is NULL safe.
 *------------------------------------------------------------------------*/
void
usb_pc_dmamap_destroy(struct usb_page_cache *pc)
{

	if (pc && pc->tag) {
		bus_dmamap_destroy(pc->tag, pc->map);
		pc->tag = NULL;
		pc->map = NULL;
	}
}

/*------------------------------------------------------------------------*
 *	usb_dma_tag_find - factored out code
 *------------------------------------------------------------------------*/
struct usb_dma_tag *
usb_dma_tag_find(struct usb_dma_parent_tag *udpt,
    usb_size_t size, usb_size_t align)
{
	struct usb_dma_tag *udt = udpt->utag_first;
	uint8_t nudt = udpt->utag_max;

	USB_ASSERT(align > 0, ("Invalid parameter align = 0\n"));
	USB_ASSERT(size > 0, ("Invalid parameter size = 0\n"));

	while (nudt--) {
		if (udt->align == 0) {
			usb_dma_tag_create(udt, size, align);
			if (udt->tag == NULL)
				return (NULL);
			udt->align = align;
			udt->size = size;
			return (udt);
		}
		if ((udt->align == align) && (udt->size == size))
			return (udt);
		udt++;
	}
	return (NULL);
}

/*------------------------------------------------------------------------*
 *	usb_dma_tag_setup - initialise USB DMA tags
 *------------------------------------------------------------------------*/
void
usb_dma_tag_setup(struct usb_dma_parent_tag *udpt, struct usb_dma_tag *udt,
    bus_dma_tag_t dmat, struct mtx *mtx, usb_dma_callback_t *func,
    uint8_t ndmabits, uint8_t nudt)
{

	bzero(udpt, sizeof(*udpt));

	/* sanity checking */
	if (nudt == 0 || ndmabits == 0 || mtx == NULL)
		return;

	/* initialise condition variable */
	cv_init(udpt->cv, "USB DMA CV");

	/* store some information */
	udpt->mtx = mtx;
	udpt->func = func;
	udpt->tag = dmat;
	udpt->utag_first = udt;
	udpt->utag_max = nudt;
	udpt->dma_bits = ndmabits;

	while (nudt--) {
		bzero(udt, sizeof(*udt));
		udt->tag_parent = udpt;
		udt++;
	}
}

/*------------------------------------------------------------------------*
 *	usb_bus_tag_unsetup - factored out code
 *------------------------------------------------------------------------*/
void
usb_dma_tag_unsetup(struct usb_dma_parent_tag *udpt)
{
	struct usb_dma_tag *udt = udpt->utag_first;
	uint8_t nudt = udpt->utag_max;

	while (nudt--) {
		if (udt->align) {
			/* destroy the USB DMA tag */
			usb_dma_tag_destroy(udt);
			udt->align = 0;
		}
		udt++;
	}

	if (udpt->utag_max) {
		/* destroy the condition variable */
		cv_destroy(udpt->cv);
	}
}

/*------------------------------------------------------------------------*
 *	usb_bdma_work_loop
 *
 * This function handles loading of virtual buffers into DMA and is
 * only called when "dma_refcount" is zero.
 *------------------------------------------------------------------------*/
void
usb_bdma_work_loop(struct usb_xfer_queue *pq)
{
	struct usb_xfer_root *info;
	struct usb_xfer *xfer;
	usb_frcount_t nframes;

	xfer = pq->curr;
	info = xfer->xroot;

	mtx_assert(info->xfer_mtx, MA_OWNED);

	if (xfer->error) {
		/* some error happened */
		USB_BUS_LOCK(info->bus);
		usbd_transfer_done(xfer, 0);
		USB_BUS_UNLOCK(info->bus);
		return;
	}
	if ((xfer->status & XFER_STATUS_DMASETUP) == 0) {
		struct usb_page *pg;
		usb_frlength_t frlength_0;
		uint8_t isread;

		xfer->status |= XFER_STATUS_DMASETUP;

		/* reset BUS-DMA load state */

		info->dma_error = 0;

		if ((xfer->status & XFER_STATUS_ISOCXFER) != 0) {
			/* only one frame buffer */
			nframes = 1;
			frlength_0 = xfer->sumlen;
		} else {
			/* can be multiple frame buffers */
			nframes = xfer->nframes;
			frlength_0 = xfer->frlengths[0];
		}

		/*
		 * Set DMA direction first. This is needed to
		 * select the correct cache invalidate and cache
		 * flush operations.
		 */
		isread = USB_GET_DATA_ISREAD(xfer);
		pg = xfer->dma_page_ptr;

		if ((xfer->status & XFER_STATUS_CTRLXFER) != 0 &&
		    (xfer->status & XFER_STATUS_CTRLHDR) != 0) {
			/* special case */
			if (xfer->usb_mode == USB_MODE_DEVICE) {
				/* The device controller writes to memory */
				xfer->frbuffers[0].isread = 1;
			} else {
				/* The host controller reads from memory */
				xfer->frbuffers[0].isread = 0;
			}
		} else {
			/* default case */
			xfer->frbuffers[0].isread = isread;
		}

		/*
		 * Setup the "page_start" pointer which points to an array of
		 * USB pages where information about the physical address of a
		 * page will be stored. Also initialise the "isread" field of
		 * the USB page caches.
		 */
		xfer->frbuffers[0].page_start = pg;

		info->dma_nframes = nframes;
		info->dma_currframe = 0;
		info->dma_frlength_0 = frlength_0;

		pg += (frlength_0 / USB_PAGE_SIZE);
		pg += 2;

		while (--nframes > 0) {
			xfer->frbuffers[nframes].isread = isread;
			xfer->frbuffers[nframes].page_start = pg;

			pg += (xfer->frlengths[nframes] / USB_PAGE_SIZE);
			pg += 2;
		}

	}
	if (info->dma_error) {
		USB_BUS_LOCK(info->bus);
		usbd_transfer_done(xfer, USB_ERR_DMA_LOAD_FAILED);
		USB_BUS_UNLOCK(info->bus);
		return;
	}
	if (info->dma_currframe != info->dma_nframes) {
		if (info->dma_currframe == 0) {
			/* special case */
			usb_pc_load_mem(xfer->frbuffers,
			    info->dma_frlength_0, 0);
		} else {
			/* default case */
			nframes = info->dma_currframe;
			usb_pc_load_mem(xfer->frbuffers + nframes,
			    xfer->frlengths[nframes], 0);
		}

		/* advance frame index */
		info->dma_currframe++;
		return;
	}
	/* go ahead */
	usb_bdma_pre_sync(xfer);
	/* start loading next USB transfer, if any */
	usb_command_wrapper(pq, NULL);
	/* finally start the hardware */
	usbd_pipe_enter(xfer);
}

/*------------------------------------------------------------------------*
 *	usb_bdma_done_event
 *
 * This function is called when the BUS-DMA has loaded virtual memory
 * into DMA, if any.
 *------------------------------------------------------------------------*/
void
usb_bdma_done_event(struct usb_dma_parent_tag *udpt)
{
	struct usb_xfer_root *info = USB_DMATAG_TO_XROOT(udpt);

	mtx_assert(info->xfer_mtx, MA_OWNED);

	/* copy error */
	info->dma_error = udpt->dma_error;

	/* enter workloop again */
	usb_command_wrapper(&info->dma_q,
	    info->dma_q.curr);
}

/*------------------------------------------------------------------------*
 *	usb_bdma_pre_sync
 *
 * This function handles DMA synchronisation that must be done before
 * an USB transfer is started.
 *------------------------------------------------------------------------*/
void
usb_bdma_pre_sync(struct usb_xfer *xfer)
{
	struct usb_page_cache *pc;
	usb_frcount_t nframes;

	if ((xfer->status & XFER_STATUS_ISOCXFER) != 0) {
		/* only one frame buffer */
		nframes = 1;
	} else {
		/* can be multiple frame buffers */
		nframes = xfer->nframes;
	}

	pc = xfer->frbuffers;

	while (nframes--) {
		if (pc->isread)
			usb_pc_cpu_invalidate(pc);
		else
			usb_pc_cpu_flush(pc);
		pc++;
	}
}

/*------------------------------------------------------------------------*
 *	usb_bdma_post_sync
 *
 * This function handles DMA synchronisation that must be done after
 * an USB transfer is complete.
 *------------------------------------------------------------------------*/
void
usb_bdma_post_sync(struct usb_xfer *xfer)
{
	struct usb_page_cache *pc;
	usb_frcount_t nframes;

	if ((xfer->status & XFER_STATUS_ISOCXFER) != 0) {
		/* only one frame buffer */
		nframes = 1;
	} else {
		/* can be multiple frame buffers */
		nframes = xfer->nframes;
	}

	pc = xfer->frbuffers;

	while (nframes--) {
		if (pc->isread)
			usb_pc_cpu_invalidate(pc);
		pc++;
	}
}

#endif
