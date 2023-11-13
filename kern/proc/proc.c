/*
 * Copyright (c) 2013
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
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <synch.h>
#include <kern/unistd.h>
#include <kern/fcntl.h>
#include <vfs.h>
#include <limits.h>
/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;
struct lock *pid_lock;
//struct pid_entry*  pids[10];
pid_t pid_counter;
struct pid_entry *p_table[__PID_MAX];
int pid_status[__PID_MAX];
int pid_parent[__PID_MAX];
int pid_waitcode[__PID_MAX];

struct pid_entry *pid_entry_create(void){
	struct pid_entry *pe = (struct pid_entry*)kmalloc(sizeof(struct pid_entry));
	if (pe == NULL){
		return NULL;
	}

	pe->proc = NULL;
	pe->pid_lock = lock_create("pid");
	pe->pid = -1;

	if (pe->pid_lock == NULL){
		kfree(pe);
		return NULL;
	}

	return pe;
}

struct file_info *fd_create(void){

	struct file_info *fd = (struct file_info*)kmalloc(sizeof(struct file_info));

	if (fd == NULL){
		return NULL;
	}
	struct lock *lock = lock_create("fd lock");

	if (lock == NULL){
		return NULL;
	}

	fd->fd_lock = lock;
	fd->file = NULL;
	fd->offset = 0;
	fd->status_flag = -1;
	fd->ref_count = 0;

	return fd;
}

void pid_destroy(struct pid_entry *ptr) {
	if (ptr != NULL) {
		int pid = ptr->pid;
		lock_destroy(ptr->pid_lock);
		//proc_destroy(ptr->proc);
		pid_status[pid] = READY;
		pid_parent[pid] = -1;
		pid_waitcode[pid] = -1;
		kfree(ptr);
	}
}

void fd_destroy(struct file_info *fd){
	KASSERT(fd != NULL);
	lock_destroy(fd->fd_lock);
	kfree(fd);
}

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;
	pid_t i;
	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->pid = -1;

	lock_acquire(pid_lock);
	for(i = 0; i < __PID_MAX; i++){

		if(pid_status[i] == READY){
			proc->pid = i;
			pid_status[i] = RUNNING;

			p_table[i] = pid_entry_create();

			if (p_table[i] == NULL) {
				kfree(proc);
				lock_release(pid_lock);
				return NULL;
			}

			p_table[i]->proc = proc;
			p_table[i]->pid = i;
			break;
		}
	}
	lock_release(pid_lock);


	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL || proc->pid == -1) {
		pid_destroy(p_table[proc->pid]);
		kfree(proc);
		return NULL;
	}

	threadarray_init(&proc->p_threads);
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	proc->fd_lock = lock_create("proc lock");
	if (proc->fd_lock == NULL){
		pid_destroy(p_table[proc->pid]);
		kfree(proc);
		return NULL;
	}

	for (int i = 0; i < __OPEN_MAX; i++){
		proc->fd[i] = NULL;
	}

	return proc;
}
struct proc* proc_fork(const char* name){
	struct proc* proc;
	proc = proc_create(name);
	return proc;
}
/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */

void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		} else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}

		(void) as;
	}

	threadarray_cleanup(&proc->p_threads);
	spinlock_cleanup(&proc->p_lock);

	lock_destroy(proc->fd_lock);

	for(int i = 0; i < __OPEN_MAX; i++){

		if(proc->fd[i] == NULL){
			continue;
		} else {
			struct file_info *fhandle = proc->fd[i];
			lock_acquire(fhandle->fd_lock);

			int count = fhandle->ref_count - 1;

			if (count <= 0) {
				vfs_close(fhandle->file);
			}

			fhandle->ref_count = count;
			proc->fd[i] = NULL;

			lock_release(fhandle->fd_lock);

			if (count <= 0) {
				fd_destroy(fhandle);
			}
			proc->fd[i] = NULL;
		}
	}

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	pid_counter = 0;
	pid_lock = lock_create("pid lock");
	if (pid_lock == NULL){
		panic("pid lock creation failed");
	}
	lock_acquire(pid_lock);
	for(int i = 0; i < __PID_MAX; i++){
		/*pids[i] = pid_entry_create();
		if(pids[i] == NULL){
			panic("pid entry create failed\n");
		}*/
		pid_status[i] = READY;
		pid_parent[i] = -1;
		pid_waitcode[i] = -1;
	}
	lock_release(pid_lock);

	pid_cv = cv_create("pid cv");

	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}

}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;
	struct vnode *in;
	struct vnode *out;
	struct vnode *err;
	//mode_t dummy_mode = 0;
	int result;
	char arg[__PATH_MAX + 1] = "con:";
	char arg2[__PATH_MAX + 1] = "con:";
	char arg3[__PATH_MAX + 1] = "con:";

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;
	newproc->fd[STDIN_FILENO] = fd_create();
	if(newproc->fd[STDIN_FILENO] == NULL){
		return NULL;
	}

	newproc->fd[STDOUT_FILENO] = fd_create();
	if(newproc->fd[STDOUT_FILENO] == NULL){
		return NULL;
	}

	newproc->fd[STDERR_FILENO] = fd_create();
	if(newproc->fd[STDERR_FILENO] == NULL){
		return NULL;
	}

	/* VFS fields */
	result = vfs_open(arg, O_RDONLY, 0664, &in);
	if(result){
		return NULL;
	}

	newproc->fd[STDIN_FILENO]->file  = in;
	newproc->fd[STDIN_FILENO]->status_flag = O_RDONLY;

	result = vfs_open(arg2, O_WRONLY, 0664, &out);
	if(result){
		return NULL;
	}
	newproc->fd[STDOUT_FILENO]->file = out;
	newproc->fd[STDOUT_FILENO]->status_flag = O_WRONLY;

	result = vfs_open(arg3, O_WRONLY, 0664, &err);
	if(result){
		return NULL;
	}
	newproc->fd[STDERR_FILENO]->file = err;
	newproc->fd[STDERR_FILENO]->status_flag = O_WRONLY;

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int result;
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	result = threadarray_add(&proc->p_threads, t, NULL);
	spinlock_release(&proc->p_lock);
	if (result) {
		return result;
	}
	spl = splhigh();
	t->t_proc = proc;
	splx(spl);
	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	unsigned i, num;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	/* ugh: find the thread in the array */
	num = threadarray_num(&proc->p_threads);
	for (i=0; i<num; i++) {
		if (threadarray_get(&proc->p_threads, i) == t) {
			threadarray_remove(&proc->p_threads, i);
			spinlock_release(&proc->p_lock);
			spl = splhigh();
			//t->t_proc = NULL;
			splx(spl);
			return;
		}
	}
	/* Did not find it. */
	spinlock_release(&proc->p_lock);
	panic("Thread (%p) has escaped from its process (%p)\n", t, proc);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}
