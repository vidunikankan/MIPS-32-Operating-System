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
	if (pid_parent[child->pid] == -1) {
		pid_parent[child->pid] = curproc->pid;
	} else {
		panic("Already exists parent for this process");
	}
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
	int exitcode;
	bool is_child = pid_parent[pid] == curproc->pid ? true : false;

	//Check is curproc is parent
	if (is_child == false) {return ECHILD;}

	//Invalid argument
	if (options != 0) {return EINVAL;}

	//PID arg not in bounds
	if (pid_status[pid] == READY
		|| pid < __PID_MIN
		|| pid > __PID_MAX)
	{
		return ESRCH;
	}

	lock_acquire(pid_lock);
	while (pid_status[pid] != ZOMBIE) {
		cv_wait(pid_cv, pid_lock);
	}

	exitcode = pid_waitcode[pid];
	lock_release(pid_lock);

	if (status != NULL) {
		int result = copyout(&exitcode, (userptr_t) status, sizeof(int));
		if (result){return result;}
	}

	return 0;
}

void sys__exit(int exitcode) {
	int parent = curproc->pid;

	lock_acquire(pid_lock);
	// Update children
	for (int i = 0; i < PID_MAX; i++) {
		if (p_table[i] != NULL){
			if (pid_parent[i] == parent){
				//if running, now orphan
				if (pid_status[i] == RUNNING){
					pid_status[i] = ORPHAN;
				//if dead,
				} else if (pid_status[i] == ZOMBIE) {
					proc_destroy(p_table[i]->proc);
					pid_destroy(p_table[i]);
					p_table[i] = NULL;
				} else {
					panic("Forget to update somewhere");
				}
			}
		}
	}

	//If parent still alive, update exitcode
	if (pid_status[parent] == RUNNING) {
		pid_status[parent] = ZOMBIE;
		pid_waitcode[parent] = exitcode;

	//If orphaned, just destroy entry since no one is waiting on exitcode
	} else if (pid_status[parent] == ORPHAN) {
		proc_destroy(p_table[parent]->proc);
		pid_destroy(p_table[parent]);
		p_table[parent] = NULL;
	}

	cv_broadcast(pid_cv, pid_lock);
	lock_release(pid_lock);
	thread_exit();
}

void sys_execv(const char *uprogram, char **uargs, int *retval){

	char **kargv = (char**)kmalloc(ARG_MAX);
	char **tempargs = (char**)kmalloc(ARG_MAX);
	char *prog = (char*)kmalloc(PATH_MAX);
	uint32_t follower;
	size_t i = 0;
	int result;
	size_t ustr_size = 0;
	size_t path_size = 0;
	size_t actual = 0;
	size_t total_buf_size = 0;
	size_t j = 0;
	size_t ADDR_MIPS = 32;
	struct vnode *v;
	vaddr_t entrypoint, user_stack;
	struct addrspace *as;
	

	if(uprogram == NULL || uargs == NULL){
		*retval = EFAULT;
		return;
	}

	//copyin name 
	result= copyinstr((const_userptr_t)uprogram, prog, PATH_MAX, &path_size);
	if(result){
		kfree(kargv);
		kfree(tempargs);
		kfree(prog);
		*retval = result;
		return;
	}

	size_t path_length;
	path_length = (size_t)get_size(prog);
	if(path_length == 0){
		kfree(kargv);
		kfree(tempargs);
		kfree(prog);
		*retval = EINVAL;
		return;
	}
	
	result = copyin((const_userptr_t)&uargs[0], &tempargs[0], ADDR_MIPS);
	if(result){
		kfree(kargv);
		kfree(tempargs);
		kfree(prog);
		*retval = result;
		return;
	}
	

	size_t p = 0;
	while(uargs[p] != NULL){
		result = copyin((const_userptr_t) uargs[p], tempargs[p], ADDR_MIPS);
		if(result){
			kfree(kargv);
			kfree(tempargs);
			kfree(prog);
			*retval = result;
			return;
		}
		p++;
	}

		//counting number of args
	size_t l = 0;
	while(uargs[l] != NULL){
		l++;
	}

	kfree(tempargs);

	
	//adding array pointer portion of kernel buffer to total size
	total_buf_size += l*sizeof(char*) + 4;
	
	//init an array that holds kernel buffer block sizes
	size_t kbuf_block_sizes[l];
	
	//packing in strings & setting pointers in kernel buffer accordingly
	for(j = 0; j < l; j++){
		
		//if j != 0, we use follower pointer to set alignment
		if(j > 0){	
			//+1 because string is 0-term'd (size of 1 char)
			kargv[j] = (char*)(follower + (size_t)get_size(uargs[j-1]) +1);
		
		//for j = 0, we must start from beginning of kernel buffer
		}else{
			//+4 because array is null-term'd (size of 1 char *)
			kargv[j] = (char*) ((uint32_t)&kargv[0] + l*sizeof(char*) +4);
			kbuf_block_sizes[j] = (size_t)l*sizeof(char*) + 4;

		}

		/*4B boundary aligning*/
		uint32_t temp = (size_t)kargv[j];
		
		//Checking alignment
		while(temp % 4 != 0){
			temp += 1;

		}

		//setting buffer pointer & follower pointer for next iteration
		kargv[j] = (char *)temp;
		follower = (size_t)kargv[j];

	}

	//setting null-term
	kargv[j] = NULL;
	
	//setting rest of block size array and copying in strings from user buffer
	for(i = 0; i < j; i++){
		//getting size of user string to pass into copinstr
		ustr_size = (size_t)get_size(uargs[i]);
		ustr_size++; //(+1 for 0-term)

		if(i > 0){
			kbuf_block_sizes[i] = (size_t)kargv[i] - (size_t)kargv[i-1];
			total_buf_size += kbuf_block_sizes[i]; //need to account for 0-term
		}
		result = copyinstr((const_userptr_t)(uargs[i]), (kargv[i]), ustr_size, &actual);
		if(result){
			kfree(kargv);
			kfree(prog);
			*retval = result;
			return;
		}

		//need to do this, otherwise last argument's bytecount isn't added to total_buf_size
		if(i == (j - 1)){
			while(actual % 4 != 0){
				actual++;
			}
			total_buf_size += actual;
		}
	}

	//open program file
	result = vfs_open(prog, O_RDONLY, 0, &v);
	if(result){
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}

	//create new address space
	as = as_create();
	if(as == NULL){
		vfs_close(v);
		kfree(kargv);
		kfree(prog);
		*retval = ENOMEM;
		return;
	}
	
	//set new address space & activate
	proc_setas(as);
	as_activate();

	//load elf exec
	result = load_elf(v, &entrypoint);
	if(result){
		vfs_close(v);
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}

	//close program file now
	vfs_close(v);

	//get user stack pointer
	result = as_define_stack(as, &user_stack);
	if(result){
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}

	size_t k = 0;
	vaddr_t user_buf;
	kargv[j] = NULL;
	
	//stack grows down, so we minus kernel buf size from top of stack to get user buffer pointer
	user_buf = user_stack - total_buf_size;

	//setting pointers on user stack before we copy out
	while(k < j){
		if(k == 0){
		kargv[k] = (char*)((size_t)user_buf + kbuf_block_sizes[k]);
		} else {
		kargv[k] = (char*)((size_t)kargv[k-1] + kbuf_block_sizes[k]);
		}
		k++;
	}
		

	//copyout kernel buffer into user stack all in one chunk
	result = copyout((const void*)&kargv[0], (userptr_t)user_buf, (total_buf_size));
	if(result){
		kfree(kargv);
		kfree(prog);
		*retval = result;
		return;
	}

	//free heap mem
	kfree(kargv);
	kfree(prog);

	//set user stack pointer
	user_stack = user_buf;

	//enter process
	enter_new_process(j, (userptr_t)user_buf, NULL, user_stack, entrypoint);

	panic("enter_new_proc returned\n");
	return;

}

int get_size (char * s) {
    char * t; 
    int size = 0;

    for (t = s; *t != '\0'; t++) {
        size++;
    }

    return size;
}
