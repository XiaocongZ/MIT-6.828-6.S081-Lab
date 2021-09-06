#include "kernel/types.h"
#include <user/user.h>

int main(int argc, char *argv[]){
  int pid,ret,fd_ptoc[2],fd_ctop[2];

  //create two pipes p0:read p1 write
  ret = pipe(fd_ptoc);
  if (ret == -1){
    fprintf(2,"system call pipe failed!\n");
    exit(-1);
  }
  ret = pipe(fd_ctop);
  if (ret == -1){
    fprintf(2,"system call pipe failed!\n");
    exit(-1);
  }

  pid = fork();
  if (pid==0){
    unsigned char buf[1]={255};
    if(read(fd_ptoc[0],&buf,1)==1){
      printf("%d: received ping\n",getpid());
      write(fd_ctop[1],&buf,1);

    }
    exit(0);
  }
  unsigned char buf[1]={255},buf1[1];
  write(fd_ptoc[1],buf,1);
  if(read(fd_ctop[0],buf1,1)==1 && buf1[0]==buf[0]){
    printf("%d: received pong\n",getpid());
  }
  //wait();
  exit(0);
}
