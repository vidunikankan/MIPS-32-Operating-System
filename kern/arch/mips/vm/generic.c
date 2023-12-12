
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <synch.h>
#include <vm.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 *
 * NOTE: it's been found over the years that students often begin on
 * the VM assignment by copying dumbvm.c and trying to improve it.
 * This is not recommended. dumbvm is (more or less intentionally) not
 * a good design reference. The first recommendation would be: do not
 * look at dumbvm at all. The second recommendation would be: if you
 * do, be sure to review it from the perspective of comparing it to
 * what a VM system is supposed to do, and understanding what corners
 * it's cutting (there are many) and why, and more importantly, how.
 */

/* under dumbvm, always have 72k of user stack */
/* (this must be > 64K so argument blocks of size ARG_MAX will fit) */
#define DUMBVM_STACKPAGES    18
#define PAGE_SIZE 4096
#define MAX_BLOCKS 4096

//Bit masks
#define TOP_BIT_MASK 0xFFC00000
#define MID_BIT_MASK 0x3FF000
#define OFFSET_MASK 0xFFF
#define DESEL_OFFSET 0xFFFFF000
#define PTEXISTS_MASK 0x1
#define PG_PRESENT_MASK 0x2
/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct lock *coremap_lock;
static struct lock *block_id_lock;
static struct lock *addr_pointers_lock;

static struct coremap_entry *coremap;
static int vm_bootstrap_flag = 0;
static paddr_t last_addr;
static paddr_t first_addr;
static paddr_t first_free;
static size_t pages_in_ram;
static int current_block_id;

void
vm_bootstrap(void){
	
	//Get last physical address to find out how much ram we have
	last_addr = ram_getsize();
	KASSERT(last_addr%PAGE_SIZE == 0);

	//Figuring out number of coremap entries
	size_t num_pages = last_addr/PAGE_SIZE;
	pages_in_ram = num_pages;
	size_t core_entries_size = num_pages*sizeof(struct coremap_entry);
	size_t pages_needed = 1;

	//Adjusting the number of pages we need for coremap
	while(core_entries_size > pages_needed*PAGE_SIZE){
		pages_needed++;
	}
	
	//Getting pages for coremap
	//spinlock_acquire(&stealmem_lock);
	coremap = (struct coremap_entry*)kmalloc(core_entries_size);
	//ram_stealmem(pages_needed);
	//spinlock_release(&stealmem_lock);
	
	//The first address of the phys mem  we are responsible for points to coremap
	first_addr = (paddr_t)(coremap - MIPS_KSEG0);
	KASSERT(first_addr%PAGE_SIZE == 0);

	//Init'ing coremap, all pages are free at this point
	for(int i = 0; i < (int)num_pages; i++){
		coremap[i].page_state = free;	
		coremap[i].owner_proc = NULL;
		coremap[i].block_id = -1;
		coremap[i].block_size = -1;
	}
	
	//Checking that the coremap isn't taking up entire physmem
	int cmap_start_page = (int)((((uint32_t)coremap)-MIPS_KSEG0)/PAGE_SIZE);
	//KASSERT((pages_needed + cmap_start_page) < num_pages);
	
	//Setting the pages containing coremap to fixed
	for(int j = cmap_start_page; j < (int)(pages_needed + cmap_start_page); j++){
		coremap[j].page_state = fixed;
		coremap[j].owner_proc = curproc;
		coremap[j].block_id = current_block_id;
		coremap[j].block_size = pages_needed;
	}

	//Setting free address & checking that it points to valid page
	first_free  = (paddr_t)(first_addr - MIPS_KSEG0) + (pages_needed*PAGE_SIZE);
	KASSERT((first_free %PAGE_SIZE) == 0);

	//Setting bootstrap flag
	vm_bootstrap_flag = 1;
	
	//Creating coremap lock to be used by non-bootstrap funcs & procs
	//TODO: check that this doesn't cause problems by lock_create()'s use of kmalloc()
	coremap_lock = lock_create("coremap_lock");
	if(coremap_lock == NULL){
		panic("lock creation failed");
	}
	block_id_lock = lock_create("block_id_lock");
	if(block_id_lock == NULL){
		panic("lock creation failed");
	}
	addr_pointers_lock = lock_create("pointers_lock");
	if(addr_pointers_lock == NULL){
		panic("lock creation failed");
	}
	return;

}


//Used by alloc_kpages prior to bootstrapping
static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
}

//Called by alloc_kpages() when we need n continuous pages for kernel use 
//TODO: should be static, change back to static & update header once addrspace.c no longer needs this function
paddr_t
page_nalloc(unsigned long npages){
	
	paddr_t addr = 0;
	int continuous_pages = 0;
	size_t block_id;
	size_t page_num_free;
	
	//Getting next available block id 
	lock_acquire(block_id_lock);
		block_id = current_block_id;
		//TODO: Decide if we should do bound checking for block id
		current_block_id++;
	lock_release(block_id_lock);

	lock_acquire(addr_pointers_lock);
		
		//Reading next free page addr
		page_num_free= first_free/PAGE_SIZE;
		first_free+= (npages*PAGE_SIZE);

		//Making sure free pointer points to next free page
		lock_acquire(coremap_lock);
			while(coremap[(first_free/PAGE_SIZE)].page_state != free){
				first_free+= PAGE_SIZE;
				if(first_free == last_addr){
					first_free = 0;
					break;
				}
			}
		lock_release(coremap_lock);
	
	lock_release(addr_pointers_lock);
	
	int coremap_idx = (int)page_num_free;
	int temp_idx = check_if_pages_fixed(coremap_idx, npages);
	
	//If there is a fixed page within npages of free page_num_free, need to use new idx;
	if(temp_idx >  (coremap_idx + (int)npages)){
		coremap_idx = temp_idx - npages;

	/*If temp_idx returned negative (search ran off end of physmem), 
	we have to search from beginning of physmem for a chunk w/o fixed pages*/
	} else if(temp_idx < 0){
		
		coremap_idx = check_if_pages_fixed(0, npages);
	}
	
	//If second search also returns negative, we panic I guess?
	KASSERT(coremap_idx > 0);

	lock_acquire(coremap_lock);
	
	
	while(continuous_pages != (int)npages){
		//If pages are not fixed but not free, we must make them free by evicting
		if(coremap[coremap_idx].owner_proc != NULL){
			lock_release(coremap_lock);
			
			//Call eviction routine (To be implemented)
			make_page_avail((paddr_t)(coremap_idx*PAGE_SIZE));
			lock_acquire(coremap_lock);
		}

		//Check that page is now free
		KASSERT(coremap[coremap_idx].page_state == free);
		if(continuous_pages == 0){

			//Set return addr to very first page of continuous npages
			addr = coremap_idx*PAGE_SIZE;
		}

		//Update coremap
		coremap[coremap_idx].page_state = dirty;
		coremap[coremap_idx].owner_proc = kproc;
		coremap[coremap_idx].block_id = block_id;
		coremap[coremap_idx].block_size = npages;
		continuous_pages++;
		coremap_idx++;
	}
	
	lock_release(coremap_lock);
	KASSERT(continuous_pages == (int)npages);
	KASSERT(addr != 0);

	return addr;

}

/*Returning coremap index of page where next npages are non-fixed (evictable), starting at cmap_idx.
- Returns -1 if we run off end of physmem while searching, up to caller to rerun search from beginning or handle this case
- Otherwise returns either:
	- Original cmap_idx + npages (if next npages were non-fixed)
	- New idx where preceding  npages are non-fixed (there was a fixed page in [cmap_idx, cmap_idx + npages]
*/
int check_if_pages_fixed(size_t cmap_idx, unsigned long npages){
	int page = 0;
	int npgs = (int)npages;
	int idx = (int)cmap_idx;
	
	while(page != npgs){
		if((idx*PAGE_SIZE) > (int)last_addr){
			idx = -1;
			break;
		}
		lock_acquire(coremap_lock);
			//If page is fixed, we set page to 0 and restart search starting from next page
			if(coremap[idx].page_state == fixed){
				page = -1;
			}
		lock_release(coremap_lock);

		page++;
		idx++;
	}
	
	return idx;
}

//Evicts page to give to kernel as continuous block, precondition of paddr_t page being NON-FIXED
void 
make_page_avail(paddr_t page){
	/*Eviction routine*/
	(void)page;
}

vaddr_t
page_alloc(struct addrspace *as, vaddr_t *va){
	//TODO: add as, va info in as parameters. Add swap info (where page is) into coremap_entry struct
	paddr_t free_ptr;
	paddr_t last_ptr;
	int block_id;
	size_t coremap_idx;
	paddr_t paddr;
	vaddr_t vaddr = *va;
	lock_acquire(addr_pointers_lock);
		free_ptr = first_free;
		last_ptr = last_addr;
		//TODO: check that the incremented free ptr actually points to a free & NON-FIXED page!
		//TODO: add check to make sure free pointer wraps around if it goes over last addr
		first_free += PAGE_SIZE;
	lock_release(addr_pointers_lock);


	KASSERT(free_ptr < last_ptr);
	coremap_idx = free_ptr/PAGE_SIZE;

	lock_acquire(block_id_lock);
		block_id = current_block_id;
		current_block_id++;
	lock_release(block_id_lock);
	
	lock_acquire(coremap_lock);
		if(coremap[coremap_idx].owner_proc != NULL || coremap[coremap_idx].page_state == fixed){
			lock_release(coremap_lock);
			make_page_avail((paddr_t)(coremap_idx*PAGE_SIZE));
			lock_acquire(coremap_lock);
		}
		coremap[coremap_idx].page_state = dirty;
		coremap[coremap_idx].owner_proc = curproc;
		coremap[coremap_idx].block_id = block_id;
		coremap[coremap_idx].block_size = 1;
	lock_release(coremap_lock);
	
	paddr = (coremap_idx*PAGE_SIZE);
	//vaddr = (vaddr_t)(paddr + MIPS_KUSEG); //TODO: check if this is a correct initial pa->va translation
	vaddr_t pgdir_index = (vaddr & TOP_BIT_MASK) >> 22;
	
	//TODO: set other bits when they are implemented
	uint8_t pt_exists = as->page_dir[pgdir_index] & PTEXISTS_MASK;
	
	//Create second-level PT if it DNE
	if(!pt_exists){
		//Storing physical addr of second PT in page directory
		as->page_dir[pgdir_index] = ((uint32_t)(kmalloc(PAGE_SIZE)- MIPS_KSEG0) << 12);
		//Setting ptexists bit for page directory entry
		as->page_dir[pgdir_index] = as->page_dir[pgdir_index] | PTEXISTS_MASK;	
	}
	
	//Indexing into second page table
	vaddr_t pt_index = (vaddr & MID_BIT_MASK) >> 12;
	vaddr_t *pt_addr = (vaddr_t*)PADDR_TO_KVADDR((as->page_dir[pgdir_index] & DESEL_OFFSET) >> 12);

	//Storing PPN in second page table
	//TODO: Store swap info in last 12 bits by OR'ing with mask
	pt_addr[pt_index] = (paddr << 12) | PG_PRESENT_MASK;
	*va = vaddr; //NOTE: maybe adapt so caller can pass in NULL ptr if they don't have a va?
	return vaddr;
}

/*Given an address space and virtual address, returns a kvaddr pointer (kernel heap) to page table entry.
Creates new pt entry if flag set, returns 0 if flag not set and entry DNE.*/
vaddr_t
pgdir_walk(struct addrspace *as, vaddr_t *vaddr, uint8_t create_table_flag){
	vaddr_t va = *vaddr;
	vaddr_t pgdir_index = (va & TOP_BIT_MASK) >> 22;
	vaddr_t pt_entry;
	uint8_t pt_exists = as->page_dir[pgdir_index] & PTEXISTS_MASK;

	if(pt_exists){
		pt_entry = PADDR_TO_KVADDR((as->page_dir[pgdir_index] & DESEL_OFFSET) >> 12);
		return pt_entry;
	} else {
		if(create_table_flag){
			//TODO: Change so that instead of allocating a page, it just creates a new table, write new func for this
			//NOTE: make sure new func sets pt_exists bit, as_copy will be dependent on this
			va = page_alloc(as, vaddr);
			pt_entry = PADDR_TO_KVADDR((as->page_dir[pgdir_index] & DESEL_OFFSET) >> 12);
			return pt_entry;
		}
		return 0;
	}
		

}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages)
{
	paddr_t pa;
	
	if(vm_bootstrap_flag){
		pa = page_nalloc(npages);
	
	KASSERT((pa + (npages*PAGE_SIZE)) < last_addr);
	} else {
		pa = getppages(npages);
		if (pa==0) {
			return 0;
		}
	
	}

	return PADDR_TO_KVADDR(pa);
}

//Precondition: vaddr_t addr must be returned by page_alloc, NOT alloc_kpages!
void 
page_free(vaddr_t addr){
	paddr_t addr_to_free = (paddr_t)(addr - MIPS_KUSEG);
	KASSERT(addr_to_free % PAGE_SIZE == 0);
	size_t coremap_idx = addr_to_free/PAGE_SIZE;
	
	lock_acquire(coremap_lock);
		coremap[coremap_idx].page_state = free;
		coremap[coremap_idx].owner_proc = NULL;
		coremap[coremap_idx].block_id = -1;
		coremap[coremap_idx].block_size = -1;
	lock_release(coremap_lock);
	
	//TODO: add stuff for user addrspace check, unmapping, tlb shootdown, etc.
	lock_acquire(addr_pointers_lock);
		first_free = (paddr_t)(coremap_idx*PAGE_SIZE);
	lock_release(addr_pointers_lock);
	return;
}

void
free_kpages(vaddr_t addr)
{
	paddr_t addr_to_free = (paddr_t)(addr - MIPS_KSEG0);
	KASSERT(addr_to_free % PAGE_SIZE == 0);
	size_t coremap_idx = addr_to_free/PAGE_SIZE;
	int num_pages_to_free = 0;

	lock_acquire(coremap_lock);
		num_pages_to_free = coremap[coremap_idx].block_size;
	lock_release(coremap_lock);
	
	for(int i = 0; i < num_pages_to_free; i++){
	lock_acquire(coremap_lock);
		coremap[coremap_idx].page_state = free;
		coremap[coremap_idx].owner_proc = NULL;
		coremap[coremap_idx].block_id = -1;
		coremap[coremap_idx].block_size = -1;
	lock_release(coremap_lock);
	coremap_idx++;
	
	}
	
	return;
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}


