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
	for(int i = 3; i < __OPEN_MAX; i++){
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
	if((user_fd < -1) || (user_fd >__OPEN_MAX - 1)){
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

size_t read(int fd, void *user_buf, size_t buflen){
	
//TODO: Add individual fd locks to file_info struct

	lock_acquire(curproc->fd_lock);

		if(fd > (__OPEN_MAX -1) || (fd < 0)) return EBADF;
		if(curproc->fd[fd]->file == NULL) return EBADF;
		
		int CHECK_RD, CHECK_RDW;
		CHECK_RD = curproc->fd[fd]->status_flag & O_RDONLY;
		CHECK_RDW = curproc->fd[fd]->status_flag & O_RDWR;

		if(!(CHECK_RD == O_RDONLY || CHECK_RDW == O_RDWR)) return EINVAL;

		struct uio read_uio;
		struct iovec read_iov;
		int result;
		size_t bytes_read;
		char *proc_buf = (char*)kmalloc(buflen);
		
		uio_kinit(&read_iov, &read_uio, (void*)proc_buf, buflen, curproc->fd[fd]->offset, UIO_READ);
		result = VOP_READ(curproc->fd[fd]->file, &read_uio);

		if(result){
			kfree(proc_buf);
			lock_release(curproc->fd_lock);
			return result;
		}

		curproc->fd[fd]->offset = read_uio.uio_offset;

		result = copyout((const void*)proc_buf, (userptr_t)user_buf, buflen);
		if(result){
			kfree(proc_buf);
	        lock_release(curproc->fd_lock);
			return result;
		}

		bytes_read = buflen - read_uio.uio_resid;
		kfree(proc_buf);
		return bytes_read;

}



size_t write(int fd, const void* user_buf, size_t nbytes){
	
	lock_acquire(curproc->fd_lock);
	if(fd > (__OPEN_MAX -1) || (fd < 0)) return EBADF;
 	if(curproc->fd[fd]->file == NULL) return EBADF;
	
	int CHECK_WR, CHECK_RDW;

	CHECK_WR = curproc->fd[fd]->status_flag & O_WRONLY;
	CHECK_RDW = curproc->fd[fd]->status_flag & O_RDWR;

	if(!(CHECK_WR == O_WRONLY || CHECK_RDW == O_RDWR)) return EINVAL;

	struct uio write_uio;
 	struct iovec write_iov;
	int result;
	size_t bytes_written;        
	size_t bytes;
	char *proc_buf = (char*)kmalloc(nbytes);
		
		result = copyinstr((userptr_t)user_buf, proc_buf, strlen(proc_buf), &bytes);
        if(result){
			kfree(proc_buf);
			lock_release(curproc->fd_lock);
			return result;
		}

		uio_kinit(&write_iov, &write_uio, (void*)proc_buf, nbytes, curproc->fd[fd]->offset, UIO_WRITE);
		result = VOP_WRITE(curproc->fd[fd]->file, &write_uio);
	
		if(result){
			kfree(proc_buf);
			lock_release(curproc->fd_lock);
			return result;
		}

	curproc->fd[fd]->offset = write_uio.uio_offset;
	bytes_written = nbytes - write_uio.uio_resid;
	kfree(proc_buf);
	lock_release(curproc->fd_lock);

	return bytes_written;

}



		

		


	





		





