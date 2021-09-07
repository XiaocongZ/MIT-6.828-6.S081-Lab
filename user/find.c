#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
//O_RDONLY
void find(char *path,char *name){
  char buf[512],*p;
  int fd,fd1;
  struct dirent de;
  struct stat st;
  //put path in buf
  strcpy(buf, path);
  p = buf+strlen(buf);
  *p++ = '/';

  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    exit(-1);
  }

  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    if(strcmp(de.name,".")==0)
      continue;
    if(strcmp(de.name,"..")==0)
      continue;

    //concatenate name after path in buf
    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = 0;
    //now buf is the file to deal with
    if((fd1 = open(buf, O_RDONLY)) < 0){
      fprintf(2, "find: cannot open %s\n", buf);
      exit(-1);
    }
    if(fstat(fd1, &st) < 0){
      fprintf(2, "find: cannot stat %s\n", buf);
      return;
    }
    switch(st.type){
      case T_DIR:
        find(buf,name);
        break;
      case T_FILE:
        if(strcmp(de.name,name)==0)
          printf("%s\n",buf);
    }
    close(fd1);
  }
  close(fd);

}

int main(int argc,char * argv[]){
  if(argc==3){
    find(argv[1],argv[2]);
    exit(0);
  }
  if(argc==2){
    find(".",argv[1]);
    exit(0);
  }
  if(argc==1 || argc>2){
    fprintf(2, "find: invalid arguments\n");
    exit(-1);
  }
  exit(0);
}
