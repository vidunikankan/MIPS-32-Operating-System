/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>
#define N_LORD_FLOWERKILLER 8
#define NROPES 16
static int ropes_left = NROPES;
static int dandelion_done = 0;
static int marigold_done= 0;
static int flowerkiller_done = 0;
struct Stake *stake_create(int indx);

struct Rope *rope_create(int indx);

void rope_destroy(struct Rope *rope);

void stake_destroy(struct Stake *stake);
/* Data structures for rope mappings */
struct Rope{
volatile int index;
volatile int is_severed;
struct lock *rope_lock;
};

struct Rope *rope_create(int indx)
{
	struct Rope *rope;
	rope = kmalloc(sizeof(struct Rope));
	if(rope == NULL){
		return NULL;
	}

	rope->index = indx;
	rope->is_severed = 0;

	rope->rope_lock = lock_create("");
	if(rope->rope_lock == NULL){
		kfree(rope);
		return NULL;
	}

	return rope;

}

void rope_destroy(struct Rope *rope)
{
	KASSERT(rope != NULL);
	KASSERT(rope->rope_lock != NULL); //check if we actually need this
	lock_destroy(rope->rope_lock);
	kfree(rope);


}

struct Stake{
int index;
struct Rope *rope;
};

struct Stake *stake_create(int indx)
{
	struct Stake *stake;
	stake = kmalloc(sizeof(struct Stake));
	if(stake == NULL){
		return NULL;
	}

	stake->index = indx;
	stake->rope = rope_create(indx);
	if(stake->rope == NULL){
		rope_destroy(stake->rope);
		kfree(stake);
		return NULL;
	}

	return stake;
}

void stake_destroy(struct Stake *stake)
{
	KASSERT(stake != NULL);
	KASSERT(stake->rope != NULL);

	rope_destroy(stake->rope);
	kfree(stake);

}
/* Implement this! */

struct Stake *allStakes[NROPES];
struct Rope *allRopes[NROPES];
/* Synchronization primitives */
struct lock *nropes_lock;
struct lock *fk_lock;
struct lock *fk_lock_2;
struct lock *done_lock;
struct cv *fk_cv;
struct cv *nropes_cv;
/* Implement this! */

/*
 * Describe your design and any invariants or locking protocols
 * that must be maintained. Explain the exit conditions. How
 * do all threads know when they are done?
 */

static void dandelion(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Dandelion thread starting\n");
	
	start: if(ropes_left != 0){
				int idx = 0;
				idx = random()%(NROPES);
				
				KASSERT(idx < NROPES && idx > -1);
				lock_acquire(allRopes[idx]->rope_lock);
				if(allRopes[idx]->is_severed)
				{
					lock_release(allRopes[idx]->rope_lock);
					goto start;
				} else {
					allRopes[idx]->is_severed = 1;
					lock_acquire(nropes_lock);
					KASSERT(ropes_left > 0);
					ropes_left -= 1;
					lock_release(nropes_lock);
					lock_release(allRopes[idx]->rope_lock);
					kprintf("Dandelion severed rope %d\n", allRopes[idx]->index);
				}
				thread_yield();
				goto start;
			}
	

	kprintf("Dandelion thread done\n");
	lock_acquire(nropes_lock);
	dandelion_done = 1;
	lock_release(nropes_lock);
	thread_exit();

}

static
void
marigold(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Marigold thread starting\n");
start:	if(ropes_left != 0){
			int idx = 0;
			idx = random()%(NROPES);
			KASSERT(idx < NROPES && idx > -1);
			lock_acquire(allStakes[idx]->rope->rope_lock);
			if(allStakes[idx]->rope->is_severed){
				lock_release(allStakes[idx]->rope->rope_lock);
				goto start;
			} else {
				allStakes[idx]->rope->is_severed = 1;
				lock_acquire(nropes_lock);
				KASSERT(ropes_left > 0);
				ropes_left -= 1;
				lock_release(nropes_lock);
				lock_release(allStakes[idx]->rope->rope_lock);
				kprintf("Marigold severed rope %d from stake %d\n", allStakes[idx]->rope->index, allStakes[idx]->index);
			}
			thread_yield();
			goto start;
				
		}
		
		
		kprintf("Marigold thread done\n");
		lock_acquire(nropes_lock);
		marigold_done = 1;
		lock_release(nropes_lock);
		thread_exit();
}


static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Lord FlowerKiller thread starting\n");
	volatile int idx1 = 0;
	volatile int idx2 = 0;
	//kprintf("entering start \n");
start:			if(ropes_left != 0){
				idx1 = random() % (NROPES);	
				idx2 = random() % (NROPES);	
				if(idx1 == (NROPES-1)) goto start;
				lock_acquire(fk_lock);
				lock_acquire(allStakes[idx1]->rope->rope_lock);
				
				if(!allStakes[idx1]->rope->is_severed) goto next;
				else
				{
				lock_release(allStakes[idx1]->rope->rope_lock);
				lock_release(fk_lock);
				goto start;
				}

next:			if(ropes_left <  2){
					lock_release(allStakes[idx1]->rope->rope_lock);
					lock_release(fk_lock);
					goto finish;
				}
			//	while(1){
				
				idx2 = random() % (NROPES);
			//	if(idx2 < idx1 || idx2 == idx1) break;
				//KASSERT(idx2 < idx1 || idx2 == idx1);
			//	}
				if(idx2 == idx1 || idx2 < idx1) goto next;

				lock_acquire(allStakes[idx2]->rope->rope_lock);
				if(allStakes[idx2]->rope->is_severed){
					lock_release(allStakes[idx2]->rope->rope_lock);
					goto next;
				}	
					
				
					struct Rope *temp;
					temp = allStakes[idx2]->rope;
					allStakes[idx2]->rope = allStakes[idx1]->rope;
					allStakes[idx1]->rope = temp;
					temp = NULL;
					//kprintf("pointers swapped %d %d \n", idx1, idx2);
					//lock_release(fk_lock);		
					lock_release(allStakes[idx1]->rope->rope_lock);
					lock_release(allStakes[idx2]->rope->rope_lock);
					lock_release(fk_lock);
				kprintf("Lord FlowerKiller has switched rope %d from stake %d to stake %d\n", allStakes[idx1]->rope->index, allStakes[idx2]->index, allStakes[idx1]->index);
				kprintf("Lord FlowerKiller has switched rope %d from stake %d to stake %d\n", allStakes[idx2]->rope->index, allStakes[idx1]->index, allStakes[idx2]->index);		
				
					thread_yield();
					goto start;
					}
							
finish:				lock_acquire(fk_lock_2);
						flowerkiller_done += 1;	
					kprintf("incremented done counter %d\n", flowerkiller_done);
			
					if(flowerkiller_done == N_LORD_FLOWERKILLER){
						kprintf("Lord FlowerKiller thread done\n");
					}
					lock_release(fk_lock_2);

					thread_exit();

}

static
void
balloon(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Balloon thread starting\n");
	
	while((dandelion_done + marigold_done + flowerkiller_done) != 2 + N_LORD_FLOWERKILLER){
		thread_yield();
	}
	lock_acquire(nropes_lock);
	kprintf("Balloon freed and Prince Dandelion escapes!\n");		
	
	kprintf("Balloon thread done\n");
	cv_signal(nropes_cv, nropes_lock);
	lock_release(nropes_lock);
	thread_exit();

}



// Change this function as necessary
int
airballoon(int nargs, char **args)
{

	int err = 0, i;

	(void)nargs;
	(void)args;
	(void)ropes_left;
	marigold_done = 0;
	dandelion_done = 0;
	ropes_left = NROPES;
	flowerkiller_done = 0;
	nropes_cv = cv_create("nropes");
	if(nropes_cv == NULL){
		goto panic;
	}

	fk_cv = cv_create("");
	if(fk_cv == NULL){
		goto panic;
	}

	fk_lock = lock_create("flowerkiller global");
	if(fk_lock == NULL){
		goto panic;
	}

	fk_lock_2 = lock_create("");
	if(fk_lock_2 == NULL){
		goto panic;
	}

	done_lock = lock_create("done_lk");
	if(done_lock ==NULL){
		goto panic;
	}

	nropes_lock = lock_create("nropes global");
	if(nropes_lock == NULL){
		goto panic;
	}

	for(int i= 0; i < NROPES; i++)
	{	
		allStakes[i] = stake_create(i);
		KASSERT(allStakes[i] != NULL);

		allRopes[i] = allStakes[i]->rope;
		KASSERT(allRopes[i] != NULL);
	}

	err = thread_fork("Marigold Thread",
			  NULL, marigold, NULL, 0);
	if(err)
		goto panic;
	
	err = thread_fork("Dandelion Thread",
			  NULL, dandelion, NULL, 0);
	if(err)
		goto panic;
	
	for (i = 0; i < N_LORD_FLOWERKILLER; i++) {
		err = thread_fork("Lord FlowerKiller Thread",
				  NULL, flowerkiller, NULL, 0);
		if(err)
			goto panic;
	}

	err = thread_fork("Air Balloon",
			  NULL, balloon, NULL, 0);
	if(err)
		goto panic;
	
	lock_acquire(nropes_lock);
	cv_wait(nropes_cv, nropes_lock);
	
	goto done;
panic:
	panic("airballoon: thread_fork failed: %s)\n",
	      strerror(err));

done:
	
	cv_destroy(nropes_cv);	
	lock_destroy(nropes_lock);
	lock_destroy(fk_lock_2);
	cv_destroy(fk_cv);
	lock_destroy(fk_lock);
	lock_destroy(done_lock);
	for(int i = 0; i <NROPES; i++)
	{	
		allStakes[i]->rope->rope_lock->holder = NULL;
		stake_destroy(allStakes[i]);
	}
	kprintf("Main thread done\n");
	return 0;
}
