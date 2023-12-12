/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>

#define DUMBVM_STACKPAGES    18
/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}
	//Allocating a page for the first-level PT (page directory)
	as->page_dir = kmalloc(PAGE_SIZE);
	if(as->page_dir == NULL){
		as_destroy(as);
		return NULL;
	}
	
	//Initializing all page directory entries to "mapping DNE" state
	for(int i = 0; i < PAGE_SIZE/4; i++){
		as->page_dir[i] = 0;
	}
	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}
	
	
	//copy entire page directory over
	memmove((void*)new->page_dir, 
		(const void*)old->page_dir, PAGE_SIZE);
	
	for(int i = 0; i < PAGE_SIZE/4; i++){
		vaddr_t va;
		vaddr_t *pt_entry_old;
		va = (i << 22);
		pt_entry_old =(vaddr_t *)pgdir_walk(old, &va, 0);
		if(pt_entry_old){
			new->page_dir[i] = new->page_dir[i] | 0xFFFFE; //zeroing "pt_exists" bit so that a new one is created

			for(int j = 0; j < PAGE_SIZE/4; j++){
				vaddr_t new_va = (i << 22)| (j << 12);
				//Check present bit
				uint8_t page_present = pt_entry_old[j] & 0x2; 
				//If present, set new pte's present bit
				if(page_present){
					page_alloc(new, &new_va);	
				}
				//If not present, ignore for now (TODO: with swapping make disk copy)
			}
		}else{
			//zero the page dir entry, so that next time we check pt_exists is false
			new->page_dir[i] = new->page_dir[i] | 0xFFFFFE;
		}
	}


     *ret = new;
     return 0;


}

void
as_destroy(struct addrspace *as)
{
	/*
	 * Clean up as needed.
	 */
	for(int i = 0; i < PAGE_SIZE/4; i++){
		vaddr_t va;
		vaddr_t *pt_entry;
		va = (i << 22);

		pt_entry = pgdir_walk(as, &va, 0);
		if(pt_entry){

			for(int j = 0; j < PAGE_SIZE/4; j++){
				//NOTE: last 12 bits should be consistent across directory & pt
				vaddr_t new_va = (i << 22)| (j << 12);
				//Check present bit
				uint8_t page_present = pt_entry[j] & 0x2; 
				//If present, set new pte's present bit
				if(page_present){
					page_free(new_va);
				}
				//If not present, ignore for now (TODO: with swapping, free disk page)
			}
			kfree((void*)pt_entry);
		}
	//Freeing memory that was allocated for page directory	
	if(as->page_dir != NULL){
		kfree(as->page_dir);
	}
	//Free addrspace struct
	kfree(as);
	}
}

void
as_activate(void)
{
	struct addrspace *as;
	int i, spl;
	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}
	/* Disable interrupts on this CPU while frobbing the TLB. */
     spl = splhigh();
     
     for (i=0; i<NUM_TLB; i++) {
         tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
     }
     
     splx(spl);
	
}
/*static
void
as_zero_region(paddr_t paddr, unsigned npages)
 {
     bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
 }*/

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages;

     /* Align the region. First, the base... */
     sz += vaddr & ~(vaddr_t)PAGE_FRAME;
     vaddr &= PAGE_FRAME;
 
     /* ...and now the length. */
     sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;
 
     npages = sz / PAGE_SIZE;
 
     /* We don't use these - all pages are read-write */
     (void)readable;
     (void)writeable;
     (void)executable;
 
     if (as->as_vbase1 == 0) {
         as->as_vbase1 = vaddr;
         as->as_npages1 = npages;
         return 0;
     }
 
     if (as->as_vbase2 == 0) {
         as->as_vbase2 = vaddr;
         as->as_npages2 = npages;
         return 0;
     }
 
	     /*
	      * Support for more than two regions is not available.
		*/
     kprintf("dumbvm: Warning: too many regions\n");
     return ENOSYS;
}

int
as_prepare_load(struct addrspace *as)
{	
	(void)as;
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{

	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;

	return 0;
}

