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

int open(userptr_t user_pathname, int user_flag)
{
	char *pathname;
	int result;
	size_t len_copied;
	int index = 0;
	pathname = (char*) kmalloc(sizeof(char)*(__PATH_MAX + 1));
	

	result = copyinstr(user_pathname, pathname, __PATH_MAX, &len_copied);
	if(result){
		
		kfree(pathname);
		return result;
	}

	mode_t dummy_mode = 0;
	struct vnode *dummy_file = NULL;
	
	//Think about case where vfs_open returns, but the index finder fails. Would this affect file?
	result = vfs_open(pathname, user_flag, dummy_mode, &dummy_file);
	if(result){
	
		kfree(pathname);
		return result;
	}
	
	lock_acquire(curproc->fd_lock);
	for(int i = 2; i < __OPEN_MAX; i++){
		if(curproc->fd[i]->file == NULL){
			index = i;
			break;
		}
	}
	if(index == 0){

		kfree(pathname);
		lock_release(curproc->fd_lock);	
		return -1;
	}
	
	curproc->fd[index]->status_flag = user_flag;
	curproc->fd[index]->file = dummy_file;
	lock_release(curproc->fd_lock);
	
	kfree(pathname);

	return result;
}


int close(int user_fd){
	if((user_fd < -1) | (user_fd >__OPEN_MAX - 1)){
		return EBADF;
	}

	lock_acquire(curproc->fd_lock);
	vfs_close(curproc->fd[user_fd]->file);
	curproc->fd[user_fd]->file = NULL;
	curproc->fd[user_fd]->offset =0;
	curproc->fd[user_fd]->status_flag =-1;
	lock_release(curproc->fd_lock);
	
	return 0;
}

