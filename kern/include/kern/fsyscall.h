#include <types.h>
#include <clock.h>
#include <copyinout.h>
#include <limits.h>
#include <syscall.h>
#include <current.h>


int sys_open(userptr_t user_pathname, int user_flag);

int sys_close(int user_fd);

size_t sys_read(int, void *, size_t);

size_t sys_write(int, const void*, size_t);
