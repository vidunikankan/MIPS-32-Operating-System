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

//static global variables
static int ropes_left = NROPES;
static int dandelion_done = 0;
static int marigold_done= 0;
static int flowerkiller_done = 0;

//function prototypes
struct Stake *stake_create(int indx);

struct Rope *rope_create(int indx);

void rope_destroy(struct Rope *rope);

void stake_destroy(struct Stake *stake);
/* Data structures for rope mappings */

//Rope struct
struct Rope{
volatile int index;
volatile int is_severed;
struct lock *rope_lock;
};

//Rope object constructor
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

//rope destructor
void rope_destroy(struct Rope *rope)
{
	KASSERT(rope != NULL);
	KASSERT(rope->rope_lock != NULL); //check if we actually need this
	lock_destroy(rope->rope_lock);
	kfree(rope);


}

//Stake struct
struct Stake{
int index;
struct Rope *rope;
};

//Stake constructor
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

//Stake destructor
void stake_destroy(struct Stake *stake)
{
	KASSERT(stake != NULL);
	KASSERT(stake->rope != NULL);

	rope_destroy(stake->rope);
	kfree(stake);

}

/* Implement this! */
//Object arrays
struct Stake *allStakes[NROPES];
struct Rope *allRopes[NROPES];

/* Synchronization primitives */
struct lock *nropes_lock;
struct lock *fk_lock;
struct lock *fk_lock_2;
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
	
	start: if(ropes_left != 0){ //making sure there are ropes left to sever
				
				int idx = 0;
				idx = random()%(NROPES);
				
				KASSERT(idx < NROPES && idx > -1);

				/*A: First we acquire the rope lock through the rope array (hooks). An crucial invariant
					for the ropes is that they can only be severed once the corresponding rope lock has been acquired by
					a thread. We then decrement the global rope counter (ropes_left) by acquiring the same lock used to 
					put the main thread to sleep.
				*/
				lock_acquire(allRopes[idx]->rope_lock);
				
				//check if severed
				if(allRopes[idx]->is_severed)
				{
					lock_release(allRopes[idx]->rope_lock);
					goto start;
				} else {
				
				//sever
					allRopes[idx]->is_severed = 1;
				
				//decrement ropes_left
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
	
	//exit sequence
	kprintf("Dandelion thread done\n");
	/*A: The character threads know they are done when there seems to be no more ropes left. This is checked through the 
		ropes_left counter, at the front of every sever/switch attempt cycle. When a thread is successful, it yields, but
		is looped back to the beginning when it runs again, where it can then check the counter, and determine whether it 
		should exit or not. As it exits, it increments a global thread counter (one for each character), so that the 
		balloon thread can know when it can wake up the main thread and exit.
	*/
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
start:	if(ropes_left != 0){ //make sure there are ropes left
			
			int idx = 0;
			idx = random()%(NROPES);
			KASSERT(idx < NROPES && idx > -1);
			
			lock_acquire(allStakes[idx]->rope->rope_lock);
			
			//check if acquired rope is severed yet
			if(allStakes[idx]->rope->is_severed){
				
				lock_release(allStakes[idx]->rope->rope_lock);
				goto start;
			
			} else {
				
				//sever rope
				allStakes[idx]->rope->is_severed = 1;
				
				//lock & decrement ropes_left
				lock_acquire(nropes_lock);
				KASSERT(ropes_left > 0);
				ropes_left -= 1;
				lock_release(nropes_lock);
				lock_release(allStakes[idx]->rope->rope_lock);
				
				//message
				kprintf("Marigold severed rope %d from stake %d\n", allStakes[idx]->rope->index, allStakes[idx]->index);
			}
			thread_yield();
			goto start;
				
		}
		
		//exit sequence	
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

	//indices for ropes
	volatile int idx1 = 0;
	volatile int idx2 = 0;

start:			if(ropes_left != 0){ //want to start exit process if there are no ropes left to switch
					idx1 = random() % (NROPES);	
					idx2 = random() % (NROPES);
					/*A: An important locking protocol for FlowerKiller is the one ensuring deadlock doesn't occur as 
						two FlowerKiller threads are picking the ropes they are going to switch. It is as follows:
						the number of ropes are checked at start, and a first index is generated. The rope lock 
						corresponding to that stake index is then acquired after acquiring the global FlowerKiller 
						synchronizing lock, and the sever state of the rope is checked. The global lock is held onto until
						the same process is conducted for the second stake and rope. As soon as we acquire two valid ropes,
						the global lock is released, so that another thread is able to pick up ropes. This is important, 
						as not synchronizing two consecutive lock grabs can result in a deadlock, if the two threads 
						attempt to grab the same locks in different orders.
					*/
				
					if(idx1 == (NROPES-1)) goto start;
					
					//acquiring synch lock for flowerkiller threads & lock for idx1's rope
					lock_acquire(fk_lock);
					lock_acquire(allStakes[idx1]->rope->rope_lock);
					
					//if the rope has not been severed yet, we go on to pick up the next one
					if(!allStakes[idx1]->rope->is_severed) goto next;
					else
					{
					lock_release(allStakes[idx1]->rope->rope_lock);
					lock_release(fk_lock);
					goto start;
					}

next:				if(ropes_left <  2){ //want to release held locks and start exiting if not enough ropes left
						lock_release(allStakes[idx1]->rope->rope_lock);
						lock_release(fk_lock);
						goto finish;
					}
					
					idx2 = random() % (NROPES);
					
					if(idx2 == idx1) goto next;
					
					//acquire second rope lock and check if it is severed
					lock_acquire(allStakes[idx2]->rope->rope_lock);
					
					if(allStakes[idx2]->rope->is_severed){
						lock_release(allStakes[idx2]->rope->rope_lock);
						goto next;
					}	
					
					//release fk lock for other threads to use to pick up rope
					lock_release(fk_lock);
					
					//swap rope pointers
					struct Rope *temp;
					temp = allStakes[idx2]->rope;
					allStakes[idx2]->rope = allStakes[idx1]->rope;
					allStakes[idx1]->rope = temp;
					temp = NULL;
					
					//release in order of index to avoid deadlock
					if(idx2 > idx1){
						lock_release(allStakes[idx1]->rope->rope_lock);
						lock_release(allStakes[idx2]->rope->rope_lock);
					} else {
						lock_release(allStakes[idx2]->rope->rope_lock);
						lock_release(allStakes[idx1]->rope->rope_lock);
					}

					//messages
				kprintf("Lord FlowerKiller has switched rope %d from stake %d to stake %d\n", allStakes[idx1]->rope->index, allStakes[idx2]->index, allStakes[idx1]->index);
				kprintf("Lord FlowerKiller has switched rope %d from stake %d to stake %d\n", allStakes[idx2]->rope->index, allStakes[idx1]->index, allStakes[idx2]->index);		
				
					thread_yield();
					goto start;
					}
							
finish:				lock_acquire(fk_lock_2); //acquire lock to increment thread exit counter
						flowerkiller_done += 1;	
						
						//FlowerKiller is the only character with multiple threads, so each thread adds to it as it exits
						if(flowerkiller_done == N_LORD_FLOWERKILLER)
						{
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
	
	
	/*A: The balloon thread continually yields until a condition is me. When the condition (all the characters having
		exited) has been met, it acquires the lock needed to signal the main thread to wake up, before exiting itself.
	*/
	while((dandelion_done + marigold_done + flowerkiller_done) != 2 + N_LORD_FLOWERKILLER){
		thread_yield();
	}
	
	//messages
	kprintf("Balloon freed and Prince Dandelion escapes!\n");		
	kprintf("Balloon thread done\n");

	//waking up main thread & exiting
	lock_acquire(nropes_lock);
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
	
	//initializing static variables
	marigold_done = 0;
	dandelion_done = 0;
	ropes_left = NROPES;
	flowerkiller_done = 0;

	//creating synch mechanisms
	nropes_cv = cv_create("nropes");
	if(nropes_cv == NULL){
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

	nropes_lock = lock_create("nropes global");
	if(nropes_lock == NULL){
		goto panic;
	}
	
	//initializing stake & rope structures
	for(int i= 0; i < NROPES; i++)
	{	
		allStakes[i] = stake_create(i);
		KASSERT(allStakes[i] != NULL);

		allRopes[i] = allStakes[i]->rope;
		KASSERT(allRopes[i] != NULL);
	}

	//creating character threads
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
	
	//getting main thread to wait for character threads to return
	lock_acquire(nropes_lock);
	cv_wait(nropes_cv, nropes_lock);
	
	goto done;
panic:
	panic("airballoon: thread_fork failed: %s)\n",
	      strerror(err));

done:
	//freeing mem	
	cv_destroy(nropes_cv);	
	lock_destroy(nropes_lock);
	lock_destroy(fk_lock_2);
	lock_destroy(fk_lock);
	
	for(int i = 0; i <NROPES; i++)
	{	
		allStakes[i]->rope->rope_lock->holder = NULL;
		stake_destroy(allStakes[i]);
	}
	kprintf("Main thread done\n");
	return 0;
}
