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

/*
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count)
{
        struct semaphore *sem;

        sem = kmalloc(sizeof(struct semaphore));
        if (sem == NULL) {
                return NULL;
        }

        sem->sem_name = kstrdup(name);
        if (sem->sem_name == NULL) {
                kfree(sem);
                return NULL;
        }

	sem->sem_wchan = wchan_create(sem->sem_name);
	if (sem->sem_wchan == NULL) {
		kfree(sem->sem_name);
		kfree(sem);
		return NULL;
	}

	spinlock_init(&sem->sem_lock);
        sem->sem_count = initial_count;

        return sem;
}

void
sem_destroy(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	/* wchan_cleanup will assert if anyone's waiting on it */
	spinlock_cleanup(&sem->sem_lock);
	wchan_destroy(sem->sem_wchan);
        kfree(sem->sem_name);
        kfree(sem);
}

void
P(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete the P without blocking.
         */
        KASSERT(curthread->t_in_interrupt == false);

	/* Use the semaphore spinlock to protect the wchan as well. */
	spinlock_acquire(&sem->sem_lock);
        while (sem->sem_count == 0) {
		/*
		 *
		 * Note that we don't maintain strict FIFO ordering of
		 * threads going through the semaphore; that is, we
		 * might "get" it on the first try even if other
		 * threads are waiting. Apparently according to some
		 * textbooks semaphores must for some reason have
		 * strict ordering. Too bad. :-)
		 *
		 * Exercise: how would you implement strict FIFO
		 * ordering?
		 */
		wchan_sleep(sem->sem_wchan, &sem->sem_lock);
        }
        KASSERT(sem->sem_count > 0);
        sem->sem_count--;
	spinlock_release(&sem->sem_lock);
}

void
V(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	spinlock_acquire(&sem->sem_lock);

        sem->sem_count++;
        KASSERT(sem->sem_count > 0);
	wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

	spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
        struct lock *lock;

        lock = kmalloc(sizeof(struct lock));
        if (lock == NULL) {
                return NULL;
        }

        lock->lk_name = kstrdup(name);
        if (lock->lk_name == NULL) {
                kfree(lock);
                return NULL;
        }

        // add stuff here as needed
		lock->held = 0;
		lock->holder = NULL;
		
		//init'ing wait channel
		lock->lock_wchan = wchan_create(lock->lk_name);
		
		//free object & return null
		if(lock->lock_wchan == NULL)
		{
			kfree(lock->lk_name);
			kfree(lock);
			return NULL;
		}

		//spinlock init
		spinlock_init(&lock->lock_splk);

        return lock;
}

void
lock_destroy(struct lock *lock)
{
        KASSERT(lock != NULL);

		// clean & destroy the objects

		spinlock_cleanup(&lock->lock_splk);
		wchan_destroy(lock->lock_wchan);
        // add stuff here as needed

        kfree(lock->lk_name);
        kfree(lock);
}

void
lock_acquire(struct lock *lock)
{
		
		KASSERT(lock != NULL);
		KASSERT(curthread->t_in_interrupt == false);
		KASSERT(!lock_do_i_hold(lock));

		//spinlock for wait channel
		spinlock_acquire(&lock->lock_splk);
		
		while(lock->held == 1)
		{
			wchan_sleep(lock->lock_wchan, &lock->lock_splk);
		}
		
		//double check that lock is free
		KASSERT(lock->held == 0);

		//acquire lock
		lock->held = 1;
		lock->holder = curthread;

		spinlock_release(&lock->lock_splk);

        // Write this

       // (void)lock;  // suppress warning until code gets written
}

void
lock_release(struct lock *lock)
{
	KASSERT(lock != NULL);
	KASSERT(lock_do_i_hold(lock));
	
	//get spinlock for wc
	spinlock_acquire(&lock->lock_splk);
 		
		//release lock
         lock->held = 0;
		 lock->holder = NULL;
         KASSERT(lock->held == 0);

	//wake next thread on wait channel queue
     wchan_wakeone(lock->lock_wchan, &lock->lock_splk);

	
     spinlock_release(&lock->lock_splk);
        // Write this

       // (void)lock;  // suppress warning until code gets written
}

bool
lock_do_i_hold(struct lock *lock)
{
		KASSERT(lock!= NULL);
		
		//if lock not held at all
		if(lock->held == 0) return false;

		//if we are holding the lock
		if(lock->holder == curthread) return true;
		else return false;
        // Write this

       // (void)lock;  // suppress warning until code gets written

}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
        struct cv *cv;

        cv = kmalloc(sizeof(struct cv));
        if (cv == NULL) {
                return NULL;
        }

        cv->cv_name = kstrdup(name);
        if (cv->cv_name==NULL) {
                kfree(cv);
                return NULL;
        }
	
		//init wait channel
		cv->cv_wchan = wchan_create(cv->cv_name);
		if(cv->cv_name == NULL){
			kfree(cv->cv_name);
			kfree(cv);
			return NULL;
		}
		
		//init spinlocks for wc
		spinlock_init(&cv->cv_splk); 
		spinlock_init(&cv->cv_splk_2);

        // add stuff here as needed

        return cv;
}

void
cv_destroy(struct cv *cv)
{
        KASSERT(cv != NULL);

        //destroying objs and freeing cv
		wchan_destroy(cv->cv_wchan);
	   spinlock_cleanup(&cv->cv_splk);
	   spinlock_cleanup(&cv->cv_splk_2);
		kfree(cv->cv_name);
        // add stuff here as needed

        kfree(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	KASSERT(cv != NULL);	
	KASSERT(lock != NULL);
	KASSERT(lock_do_i_hold(lock));
	
	//hold spinlock to enter wc
	spinlock_acquire(&cv->cv_splk);

		//let go of lock
		lock_release(lock);

		//enter wc
		wchan_sleep(cv->cv_wchan, &cv->cv_splk);
	
	//release spinlock & try to reacquire lock
	spinlock_release(&cv->cv_splk);

		lock_acquire(lock);
        // Write this
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	KASSERT(cv != NULL);
	KASSERT(lock != NULL);
	KASSERT(lock_do_i_hold(lock));	

	//acquire spinlock
	spinlock_acquire(&cv->cv_splk_2);
		
		//signal to next thread on wc
		wchan_wakeone(cv->cv_wchan, &cv->cv_splk_2);
	
	spinlock_release(&cv->cv_splk_2);


        // Write this
	//(void)cv;    // suppress warning until code gets written
	//(void)lock;  // suppress warning until code gets written
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	KASSERT(cv != NULL);
	KASSERT(lock != NULL);
	KASSERT(lock_do_i_hold(lock));
	
	//spinlock for wc
	spinlock_acquire(&cv->cv_splk_2);
		
		//wake all threads on wc
		wchan_wakeall(cv->cv_wchan, &cv->cv_splk_2);

	spinlock_release(&cv->cv_splk_2);
	// Write this
//	(void)cv;    // suppress warning until code gets written
//	(void)lock;  // suppress warning until code gets written
}
