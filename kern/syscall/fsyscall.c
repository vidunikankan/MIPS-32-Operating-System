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

int sys_open(userptr_t user_pathname, int user_flag, int* retval)
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
	struct vnode *dummy_file;


	//Think about case where vfs_open returns, but the index finder fails. Would this affect file?


	lock_acquire(curproc->fd_lock);

	for(int i = 3; i < __OPEN_MAX; i++){
		if(curproc->fd[i]== NULL){
			index = i;
			break;
		}
	}

	lock_release(curproc->fd_lock);
	
	if(index == 0){

		kfree(pathname);	
		return EMFILE;
	}
	
	curproc->fd[index] = fd_create();
	if(curproc->fd[index] == NULL){
		return EIO;
	}

	lock_acquire(curproc->fd[index]->fd_lock);
	result = vfs_open(pathname, user_flag, dummy_mode, &dummy_file);
	
	if(result){
          kfree(pathname);
		  lock_release(curproc->fd[index]->fd_lock);
		  return result;
 	}

	
	curproc->fd[index]->status_flag = user_flag;
	curproc->fd[index]->file = dummy_file;
	curproc->fd[index]->ref_count = 1;

	kfree(pathname);
	lock_release(curproc->fd[index]->fd_lock);
	*retval = index;
	return 0;
}


int sys_close(int user_fd){
	if((user_fd < 3) || (user_fd >__OPEN_MAX - 1)){
		return EBADF;
	}

	lock_acquire(curproc->fd_lock);
		if(curproc->fd[user_fd] == NULL){
			lock_release(curproc->fd_lock);
			return EBADF;
		}
	lock_release(curproc->fd_lock);

	lock_acquire(curproc->fd[user_fd]->fd_lock);
	if (--(curproc->fd[user_fd]->ref_count) == 0) {
		vfs_close(curproc->fd[user_fd]->file);
	}
	curproc->fd[user_fd]->file = NULL;
	curproc->fd[user_fd]->offset =0;
	curproc->fd[user_fd]->status_flag =-1;
	lock_release(curproc->fd[user_fd]->fd_lock);
	
	fd_destroy(curproc->fd[user_fd]);

	return 0;
}

int sys_read(int fd, void *user_buf, size_t buflen, int* retval){

//TODO: Add individual fd locks to file_info struct
	int bytes_read = -1;
	lock_acquire(curproc->fd_lock);

		if(fd > (__OPEN_MAX -1) || (fd < 0)){
			lock_release(curproc->fd_lock);
			return EBADF; //EBADF
		}
		if(curproc->fd[fd] == NULL){
			lock_release(curproc->fd_lock);
			return EBADF;
 		}
	lock_release(curproc->fd_lock);


		lock_acquire(curproc->fd[fd]->fd_lock);
		int CHECK_RD;
		int CHECK_WR;
		int CHECK_RDWR;
		
		CHECK_WR = curproc->fd[fd]->status_flag & O_WRONLY;
		CHECK_RD = curproc->fd[fd]->status_flag & O_RDONLY;
		CHECK_RDWR = curproc->fd[fd]->status_flag & O_RDWR;
		if(CHECK_WR == O_WRONLY){
		lock_release(curproc->fd[fd]->fd_lock);
		return EBADF; //EINVAL
		}
		if(!((CHECK_RD == O_RDONLY) || (CHECK_RDWR == O_RDWR))){
		lock_release(curproc->fd[fd]->fd_lock);
		return EINVAL;
		}

		struct uio read_uio;
		struct iovec read_iov;
		int result;
		bytes_read = 0;
		char *proc_buf = (char*)kmalloc(sizeof(char)*buflen);

	uio_kinit(&read_iov, &read_uio, (void*)proc_buf, buflen, curproc->fd[fd]->offset, UIO_READ);
	/*read_iov.iov_ubase = (void *)proc_buf;
      read_iov.iov_len = buflen;
      read_uio.uio_iov = &read_iov;
      read_uio.uio_iovcnt = 1;
      read_uio.uio_resid = buflen;
      read_uio.uio_offset = 0;
      read_uio.uio_segflg = UIO_USERSPACE;
      read_uio.uio_rw = UIO_READ;
      read_uio.uio_space = curproc->p_addrspace;
*/

		result = VOP_READ(curproc->fd[fd]->file, &read_uio);

		if(result){
			kfree(proc_buf);
			lock_release(curproc->fd[fd]->fd_lock);
			return result;
		}

		curproc->fd[fd]->offset = read_uio.uio_offset;

		result = copyout((const void*)proc_buf, (userptr_t)user_buf, buflen);
		if(result){
			kfree(proc_buf);
	        lock_release(curproc->fd[fd]->fd_lock);
			return result;
		}

		bytes_read = buflen - read_uio.uio_resid;
		kfree(proc_buf);
		lock_release(curproc->fd[fd]->fd_lock);
		*retval = bytes_read;
		return 0;

}



size_t sys_write(int fd, const void* user_buf, size_t nbytes){

	lock_acquire(curproc->fd_lock);
	if(fd > (__OPEN_MAX -1) || (fd < 0)){
		lock_release(curproc->fd_lock);
		return -1; //EBADF
	}
 	if(curproc->fd[fd]->file == NULL){
		lock_release(curproc->fd_lock);
		return -1;
	}
	lock_release(curproc->fd_lock);

	lock_acquire(curproc->fd[fd]->fd_lock);
	int CHECK_WR, CHECK_RDW;

	CHECK_WR = curproc->fd[fd]->status_flag & O_WRONLY;
	CHECK_RDW = curproc->fd[fd]->status_flag & O_RDWR;

	if(!(CHECK_WR == O_WRONLY || CHECK_RDW == O_RDWR)){
		lock_release(curproc->fd[fd]->fd_lock);
		return -1; //EINVAL
	}

	struct uio write_uio;
 	struct iovec write_iov;
	int result;
	size_t bytes_written;
	size_t bytes;
	char *proc_buf = (char*)kmalloc(sizeof(char)*nbytes);

		result = copyinstr((userptr_t)user_buf, proc_buf, nbytes, &bytes);

		uio_kinit(&write_iov, &write_uio, (void*)proc_buf, nbytes, curproc->fd[fd]->offset, UIO_WRITE);
	/*write_iov.iov_ubase = (void*)proc_buf;
     write_iov.iov_len = nbytes;
     write_uio.uio_iov = &write_iov;
     write_uio.uio_iovcnt = 1;
     write_uio.uio_resid = nbytes;
     write_uio.uio_offset = 0;
     write_uio.uio_segflg = UIO_USERSPACE;
	 write_uio.uio_rw = UIO_WRITE;
     write_uio.uio_space = curproc->p_addrspace;
*/
		result = VOP_WRITE(curproc->fd[fd]->file, &write_uio);

		if(result){
			kfree(proc_buf);
			lock_release(curproc->fd[fd]->fd_lock);
			return -1;
		}

	curproc->fd[fd]->offset = write_uio.uio_offset;
	bytes_written = nbytes - write_uio.uio_resid;
	kfree(proc_buf);
	lock_release(curproc->fd[fd]->fd_lock);

	return bytes_written;

}

int sys_lseek(int fd, off_t pos, int whence, int32_t* retval, int32_t* retval2){
	if (whence < 0 || whence > 2) {return EINVAL;}

	lock_acquire(curproc->fd_lock);
	if(fd >= __OPEN_MAX || fd < 0){
		lock_release(curproc->fd_lock);
		return EBADF;
	}

	if(curproc->fd[fd]->file == NULL){
		lock_release(curproc->fd_lock);
		return EBADF;
	}
	lock_release(curproc->fd_lock);

	struct file_info *fhandle = curproc->fd[fd];
	lock_acquire(fhandle->fd_lock);

	if (!VOP_ISSEEKABLE(fhandle->file)) {
		lock_release(fhandle->fd_lock);
		return ESPIPE;
    }

	struct stat *ptr = kmalloc(sizeof(struct stat));

	if (ptr == NULL) {
		lock_release(fhandle->fd_lock);
		kfree(ptr);
		return ENOMEM;
	 }

	VOP_STAT(fhandle->file, ptr);
	off_t size = ptr->st_size;
	off_t new_offset = fhandle->offset;

	kfree(ptr);

	switch (whence) {
        case SEEK_SET:
        new_offset = pos;
        break;

        case SEEK_CUR:
        new_offset += pos;
        break;

        case SEEK_END:
        new_offset = size + pos;
        break;
    }

	if (new_offset < 0) {
		lock_release(fhandle->fd_lock);
		return EINVAL;
	}

	fhandle->offset = new_offset;
	*retval = (uint32_t) (new_offset >> 32);
	*retval2 = (uint32_t) new_offset;

	lock_release(fhandle->fd_lock);

	return 0;
}

int sys_chdir(const char *pathname) {
	char *path = kmalloc(PATH_MAX);
	size_t len;
	int err;

	err = copyinstr((const_userptr_t) pathname, path, PATH_MAX, &len);

	if (err) {
		kfree(path);
		return err;
	}

	err = vfs_chdir((char*) pathname);
	kfree(path);

	if (err) {return err;}

	return 0;
}

int sys__getcwd(char *buf, size_t buflen, int32_t *retval) {
	struct iovec cwd_iov;
    struct uio cwd_u;
	int err;

	struct vnode *cwd = curproc->p_cwd;
    if (cwd == NULL) {return ENOENT;}

	cwd_iov.iov_ubase = (userptr_t)buf;
    cwd_iov.iov_len = buflen;
    cwd_u.uio_iov = &cwd_iov;
    cwd_u.uio_iovcnt = 1;
    cwd_u.uio_resid = buflen;
    cwd_u.uio_offset = 0;
    cwd_u.uio_segflg = UIO_USERSPACE;
    cwd_u.uio_rw = UIO_READ;
    cwd_u.uio_space = curproc->p_addrspace;

	err = vfs_getcwd(&cwd_u);
	if (err) {return err;}

	err = copyout(buf, cwd_u.uio_iov->iov_ubase, cwd_u.uio_resid);
    if (err) {return EFAULT;}

	*retval = buflen - cwd_u.uio_resid;

	return 0;
}

int sys_dup2(int oldfd, int newfd, int32_t* retval) {
	if (newfd < 0 || oldfd < 0 || newfd >= OPEN_MAX || oldfd >= OPEN_MAX) {return EBADF;}

	lock_acquire(curproc->fd_lock);
	if(curproc->fd[oldfd]->file == NULL){
		lock_release(curproc->fd_lock);
		return EBADF;
	}
	lock_release(curproc->fd_lock);

	struct file_info *old = curproc->fd[oldfd];
	struct file_info *new = curproc->fd[newfd];

	lock_acquire(old->fd_lock);
	if (old == new) {return 0;}

	if(curproc->fd[newfd]->file != NULL){sys_close(newfd);}

	lock_acquire(new->fd_lock);

	curproc->fd[newfd] = curproc->fd[oldfd];
	curproc->fd[oldfd]->ref_count++;

	lock_release(new->fd_lock);
	lock_release(old->fd_lock);

	*retval = newfd;
	return 0;
}



















