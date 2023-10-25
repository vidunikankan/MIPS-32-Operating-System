#include <types.h>
#include <clock.h>
#include <copyinout.h>
#include <limits.h>
#include <syscall.h>
#include <current.h>


int sys_open(userptr_t user_pathname, int user_flag, int* retval);

int sys_close(int user_fd);

int sys_read(int, void *, size_t, int *);

size_t sys_write(int, const void*, size_t, int32_t* retval);

int sys_lseek(int fd, off_t pos, int whence, int32_t* retval, int32_t* retval2);

int sys_chdir(const char *pathname);

int sys__getcwd(char *buf, size_t buflen, int32_t *retval);

int sys_dup2(int oldfd, int newfd, int32_t* retval);
