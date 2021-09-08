#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

//scan for '\n' or "\\n"
int main(int argc,char * argv[]){
  char buf[256],*pbuf,*parg,*hbuf,*ebuf,arg[64],*eargv[MAXARG];
  int ret,pid,status;
  ret = read(0,buf,256);
  if (ret==0){
    fprintf(2,"xargs: read from stdin failed\n");
    exit(-1);
  }
  //prepare eargv
  int i;
  for (i=0;i+1<argc;i++){
    eargv[i] = argv[i+1];
    //printf("eargv[%d]: %s\n",i,eargv[i]);
  }
  //eargv[i] waits to be set
  eargv[i+1] = 0;
  //ebuf --> '\0'
  ebuf = buf+strlen(buf);
  //stripe buf of \n and "
  if (*buf=='\"'){
    hbuf =buf + 1;
  }
  if (*(ebuf-1)=='\n'){
    ebuf--;
    *ebuf = '\0';
  }
  if (*(ebuf-1)=='\"'){
    ebuf--;
    *ebuf = '\0';
  }
  //printf("buf: %s",buf);
  for(pbuf=hbuf,parg=arg;pbuf<ebuf;){
    if ( (*pbuf=='\\') && (*(pbuf+1)=='n') ){
      *parg = '\0';
      parg = arg;
      //pass '\' and 'n'
      pbuf += 2;

      eargv[i] = arg;
      pid = fork();
      if(pid==0){
        //printf("if child\n");
        exec(eargv[0],eargv);
      }
      wait(&status);
    }
    else if(*pbuf=='\n'){
      *parg = '\0';
      parg = arg;
      //pass '\' and 'n'
      pbuf++;

      eargv[i] = arg;
      pid = fork();
      if(pid==0){
        //printf("if child\n");
        exec(eargv[0],eargv);
      }
      wait(&status);
    }
    else {
      *(parg++) = *(pbuf++);
      if(pbuf==ebuf){
        *parg = '\0';
        //printf("arg: %s **",arg);
        eargv[i] = arg;
        pid = fork();
        if(pid==0){
          //printf("else child\n");
          exec(eargv[0],eargv);
        }
        wait(&status);
      }
    }

  }
  exit(0);
}
