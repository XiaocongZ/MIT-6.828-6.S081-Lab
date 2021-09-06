#include "kernel/types.h"

#include "user/user.h"
//SYS_sleep  13
int
main(int argc, char *argv[])
{
  int t;
  if(argc<2){
    fprintf(2,"missing an argument\n");
    exit(1);
  }
  else if (argc>2){
    fprintf(2,"redundant arguments\n");
    exit(1);
  }

  t = atoi(argv[1]);
  sleep(t);

  exit(0);
}
