#include <types.h>
#include <synch.h>
#include <vnode.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <limits.h>
#include <current.h>
#include <proc.h>
#include <kern/fsyscall.h>
#include <vfs.h>
#include <kern/errno.h>
#include <uio.h>
#include <kern/fcntl.h>
#include <stat.h>
#include <kern/seek.h>
#include <kern/psyscall.h>
#include <addrspace.h>




pid_t sys_getpid(){
	lock_acquire(pid_lock);
	if (curproc == NULL) {
		lock_release(pid_lock);
		return -1;
	}

	lock_release(pid_lock);
	return curproc->pid;
}

static void enter_fork(void *arg_ptr, unsigned long int nargs){
	enter_forked_process((struct trapframe*)arg_ptr, (struct addrspace*)nargs);
}

int sys_fork(struct trapframe* parent_tf, pid_t *retval){
	struct proc *child;
	struct trapframe *child_tf;
	//struct addrspace *child_as;
	int result;

	child_tf = (struct trapframe*)kmalloc(sizeof(struct trapframe));
	if (child_tf == NULL) {return ENOMEM;}

	child = proc_fork("Child");
	if (child == NULL){
		kfree(child_tf);
		return EMPROC;
	}

	lock_acquire(p_table[child->pid]->pid_lock);
	pid_parent[child->pid] = curproc->pid;
	lock_release(p_table[child->pid]->pid_lock);

	//Not sure about this
	result = as_copy(curproc->p_addrspace, &(child->p_addrspace));
	if (result){
		kfree(child_tf);
		proc_destroy(child);
		pid_parent[child->pid] = -1;
		return result;
	}

	lock_acquire(curproc->fd_lock);
	for (int i = 0; i < __OPEN_MAX; i++){
		//if parent fd entry exists, copy it over
		//NOTE: this form of copying will allow for file mods to reflect in both procs. Not a deepcopy
		if (curproc->fd[i] != NULL){
			child->fd[i] = curproc->fd[i];

			lock_acquire(curproc->fd[i]->fd_lock);
			curproc->fd[i]->ref_count++;
			lock_release(curproc->fd[i]->fd_lock);
		}
	}
	lock_release(curproc->fd_lock);

	//copy trapframe over
	memcpy((void *) child_tf, (const void *) parent_tf, sizeof(struct trapframe));
	child_tf->tf_v0 = 0;
	child_tf->tf_v1 = 0;
	child_tf->tf_a3 = 0;
	child_tf->tf_epc += 4;

	*retval = 0;
	result = thread_fork(child->p_name, child, enter_fork, (void*)child_tf, 0);
	if (result){
		//NOTE: maybe lock pid_parent write
		pid_parent[child->pid] = -1;
		kfree(child_tf);
		proc_destroy(child);
		return result;
	}

	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		child->p_cwd = curproc->p_cwd;
		VOP_INCREF(curproc->p_cwd);
	}
	spinlock_release(&curproc->p_lock);

	*retval = child->pid;
	return 0;
}

int sys_waitpid(pid_t pid, int *status, int options) {
	int waitcode;
	bool is_child;

	//Invalid argument
	if (options != 0) {return EINVAL;}
	
	//PID arg not in bounds
	if (pid < __PID_MIN
		|| pid > __PID_MAX)
	{
		return ESRCH;
	}
	
	//Proc doesn't exist for PID
	lock_acquire(pid_lock);
		if(pid_status[pid] == 0){
			lock_release(pid_lock);
			return ESRCH;
		}
	lock_release(pid_lock);


	is_child = false;

	//Check is curproc is parent
	if (p_table[pid] != NULL) {

	lock_acquire(p_table[pid]->pid_lock);
		if (pid_parent[pid] == curproc->pid){
			is_child = true;
		}
	lock_release(p_table[pid]->pid_lock);
	
	}

	if (is_child == false) {return ECHILD;}


	lock_acquire(pid_lock);
	while (pid_status[pid] != 2) {
		cv_wait(pid_cv, pid_lock);
	}

	waitcode = pid_waitcode[pid];
	lock_release(pid_lock);

	if (status != NULL) {
		int result = copyout(&waitcode, (userptr_t) status, sizeof(int));
		if (result){return result;}
	}

	return 0;
}

void sys__exit(int exitcode) {
	int parent = curproc->pid;

	// RUNNING = 1
	// ZOMBIE = 2
	// ORPHAN = 3
	lock_acquire(pid_lock);
	// Update children
	for (int i = 0; i < PID_MAX; i++) {
		bool is_child = false;
		if(p_table[i] != NULL){
			lock_acquire(p_table[i]->pid_lock);
			if (pid_parent[i] == parent){
				is_child = true;
			}
			lock_release(p_table[i]->pid_lock);
		}
		if (is_child) {
			//if running, now orphan
			if (pid_status[i] == 1){
			pid_status[i] = 3;
			//if dead, 
			} else if (pid_status[i] == 2) {
				pid_destroy(p_table[i]);
				p_table[i] = NULL;
			} else {
				panic("Forget to update somewhere");
			}
		}
	}
	
	//If parent still alive, update exitcode
	if (pid_status[parent] == 1) {
		pid_status[parent] = 2;
		pid_waitcode[parent] = exitcode;

	//If orphaned, just destroy entry since no one is waiting on exitcode
	} else if (pid_status[parent] == 3) {
		pid_destroy(p_table[parent]);
		p_table[parent] = NULL;
	}

	cv_broadcast(pid_cv, pid_lock);
	lock_release(pid_lock);
	thread_exit();
}

void sys_execv(const char *uprogram, char **uargs, int *retval){
	
	char **kargv = (char**)kmalloc(ARG_MAX);
	char *prog = (char*)kmalloc(PATH_MAX);
	size_t i = 0;
	int result;
	//char *cur_ustr = NULL;
	//char *kargv_ptr = kargv[i+1];
	size_t ustr_size = 0;
	size_t path_size = 0;
	size_t actual = 0;
	size_t MIPS_ADDR = 32;
	size_t total_buf_size = 0;
	size_t j = 0;
	struct vnode *v;
	vaddr_t entrypoint, user_stack;
	struct addrspace *as;
	
	if(uprogram == NULL){
		*retval = EFAULT;
		return;
	}
	
	result= copyinstr((const_userptr_t)uprogram, prog, PATH_MAX, &path_size);
	if(result){
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}
	
	while(uargs[j] != NULL){
		result = copyin((const_userptr_t)&uargs[j], &kargv[j], MIPS_ADDR);
		if(result){
			kfree(kargv);
			kfree(prog);
			*retval = result;
			return;
		}
		j++;
	}

	kargv[j] = NULL;
	total_buf_size += j*sizeof(char*);
	
	for(i = 0; i < j; i++){
		ustr_size = (size_t)get_size(uargs[i]);
		result = copyinstr((const_userptr_t)uargs[i], kargv[i], ustr_size, &actual);
		total_buf_size += actual;
		//TODO: make sure kargv is aligned on 4-byte boundaries
		if(result){
			kfree(kargv);
			kfree(prog);
			*retval = result;
			return;
		}
	}

	result = vfs_open(prog, O_RDONLY, 0, &v); 
	if(result){
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}
	
	as = as_create();
	if(as == NULL){
		vfs_close(v);
		kfree(kargv);
		kfree(prog);
		*retval = ENOMEM;
		return;
	}
	proc_setas(as);
	as_activate();

	result = load_elf(v, &entrypoint);
	if(result){
		vfs_close(v);
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}

	vfs_close(v);

	result = as_define_stack(as, &user_stack);
	if(result){
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}
	
	size_t k = 0;
	//start of arg buffer on user stack
	vaddr_t user_buf = user_stack - total_buf_size;
	while(kargv[k] != NULL){
		kargv[k] = user_buf + kargv[k];
		k++;
	}
	kargv[k] = NULL;
	KASSERT(k == j);

	result = copyout((const void*)kargv, (userptr_t)user_buf, total_buf_size);
	if(result){
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}

	kfree(kargv);
	kfree(prog);

	enter_new_process(j, (userptr_t)user_buf, NULL, user_stack, entrypoint); 

	panic("enter_new_proc returned\n");
	return;

}

int get_size (char * s) {
    char * t; // first copy the pointer to not change the original
    int size = 0;

    for (t = s; *t != '\0'; t++) {
        size++;
    }

    return size;
}
