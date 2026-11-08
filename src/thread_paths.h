#ifndef MELODY_THREAD_PATHS_H
#define MELODY_THREAD_PATHS_H
#if defined(__PSP__) || defined(THREAD_PATHS_HOST_TEST)
#ifndef THREAD_PATHS_HOST_TEST
#include <unistd.h>
#include <pspiofilemgr.h>
#endif
/* libc cwd belongs to the application; PSP sceIo relative cwd is per thread.
 * Bind every I/O worker before opening files, including respawn after MV. */
static inline int melody_bind_thread_directory(void){
 char root[512];
 if(!getcwd(root,sizeof(root)))return -1;
 return sceIoChdir(root);
}
#endif
#endif
