#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


int main(int arc,char *argv[]){
  int i,fd_ptoc[2],fd_ctoc[2],ret,pid;
  int status;
  ret = pipe(fd_ctoc);
  if (ret == -1){
    fprintf(2,"system call pipe failed!\n");
    exit(-1);
  }
  for(i=2;i<=35;i++){
    write(fd_ctoc[1],&i,4);
  }
  close(fd_ctoc[1]);


  pid = fork();
  if (pid==0){
newsieve:
    int module,n,cont;
    module = 0;//initialize module of this process.
    cont =0;//if need a new child process
    //update pipes
    fd_ptoc[0] = fd_ctoc[0];
    fd_ptoc[1] = fd_ctoc[1];
    //read numbers
    while(read(fd_ptoc[0],&n,4)!=0){
      if(module==0){
        module = n;
        printf("prime %d\n",n);
      }
      else if(n%module==0){
        ;
      }
      else{
        if(cont==0){
          ret = pipe(fd_ctoc);
          if (ret == -1){
            fprintf(2,"system call pipe failed!\n");
            exit(-1);
          }
          cont = 1;
        }
        write(fd_ctoc[1],&n,4);
      }
    }
    close(fd_ptoc[0]);
    close(fd_ctoc[1]);

    //need a new child process before return?
    if(cont==1){
      pid = fork();
      if(pid==0)
        goto newsieve;
      wait(&status);
    }

    exit(0);
  }
  wait(&status);
  exit(0);
}
