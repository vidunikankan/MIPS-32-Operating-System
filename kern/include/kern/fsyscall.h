#include <types.h>
#include <clock.h>
#include <copyinout.h>
#include <limits.h>
#include <syscall.h>
#include <current.h>


int open(userptr_t user_pathname, int user_flag);

int close(int user_fd);

