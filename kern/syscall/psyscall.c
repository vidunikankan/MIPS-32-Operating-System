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

	child = proc_fork("Child");
	if(child == NULL){
		return EMPROC;
	}

	child->p_cwd = curproc->p_cwd;

	//Not sure about this
	result = as_copy(curproc->p_addrspace, &(child->p_addrspace));
	if(result){
		return result;
	}
	
	for(int i = 0; i < __OPEN_MAX; i++){
		//if parent fd entry exists, copy it over
		//NOTE: this form of copying will allow for file mods to reflect in both procs. Not a deepcopy
		if(curproc->fd[i] != NULL){
			child->fd[i] = curproc->fd[i];
			VOP_INCREF(curproc->fd[i]->file);
		}
	}

	//copy trapframe over
	child_tf->tf_status = parent_tf->tf_status;
	child_tf->tf_epc = parent_tf->tf_epc +4;
	child_tf->tf_a0 = parent_tf->tf_a0;
	child_tf->tf_a1 = parent_tf->tf_a1;
	child_tf->tf_a2 = parent_tf->tf_a2;
	child_tf->tf_a3 = 0;	
	child_tf->tf_sp = parent_tf->tf_sp;
	
	*retval = 0;	
	result = thread_fork(child->p_name, child, enter_fork, (void*)child_tf, 0);
	if(result){
		//NOTE: might need to destroy child addrspace here
		kfree(child_tf);
		kfree(child);
		return result;
	}
	
	*retval = child->pid;
	return 0;

}

