#include <types.h>
#include <clock.h>
#include <copyinout.h>
#include <limits.h>
#include <syscall.h>
#include <current.h>
#include <mips/trapframe.h>



pid_t sys_getpid(void);

int sys_fork(struct trapframe*, int *retval);
