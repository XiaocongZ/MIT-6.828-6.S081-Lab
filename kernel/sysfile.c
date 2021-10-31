//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

extern struct vmastruct vma;
extern void   vmaeclear(struct vmae *vmaep);

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

uint64
sys_mmap()
{
  uint64 addrint;
  void *addr, *start=0;
  int length, prot, flags, fd, offset;
  int perm=0;
  struct file *f;
  struct proc *p = myproc();
  struct vmae *vmaep_proc=0;
  struct vmae *vmaep_glob=0;

  if(argaddr(0, &addrint) < 0 )
    return -1;
  addr = (void*)addrint;
  if(argint(1, &length) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 || argint(4, &fd) < 0 || argint(5, &offset) < 0 )
    return -1;
  if((f=p->ofile[fd]) == 0){
    if(DEBUG) printf("mmap: failed to get file\n");
    return -1;
  }
  if(prot&PROT_EXEC) perm |= PTE_X;
  if(prot&PROT_WRITE) perm |= PTE_W;
  if(prot&PROT_READ) perm |= PTE_R;
  if(DEBUG) printf("mmap: args addr %p length %p\n", addr, length);
  if(DEBUG) printf("mmap: proc->sz %p\n", p->sz);
  for(uint64 i = 0; i < p->sz; i += PGSIZE){
    if(DEBUG) printf("mmap: proc uvm %p to %p\n", i, walkaddr(p->pagetable, i));
  }
  //get start in virtual mem
  for(uint64 istart = PGROUNDUP((uint64)addr); istart + PGROUNDUP(length) < KERNBASE; istart += PGSIZE){
    pte_t *pte_i;
TRY_ISTART:
    if(DEBUG) printf("mmap: try to start at %p\n", istart);
    pte_i = walk(p->pagetable, istart, 0);
    //try start here
    if((*pte_i&PTE_V) == 0){
      pte_t *pte_j;
      for(uint64 jlength=istart; jlength < istart + PGROUNDUP(length); jlength += PGSIZE){
        pte_j = walk(p->pagetable, jlength, 0);
        if(*pte_j&PTE_V){
          istart = jlength;
          goto TRY_ISTART;
        }
      }
      //this istart works
      if(DEBUG) printf("mmap: successfully get istart %p\n", istart);
      start = (void*) istart;
      //update proc->sz
      if(p->sz < istart + PGROUNDUP(length)){
        p->sz = istart + PGROUNDUP(length);
        if(DEBUG) printf("mmap: set p->sz to %p\n", p->sz);
      }
      //alloc zeroed pages and ~PTE_U
      uint64 pa;
      for(uint64 jlength=istart; jlength < istart + PGROUNDUP(length); jlength += PGSIZE){
        if((pa=(uint64)kalloc())==0) panic("mmap: kalloc failed");
        memset((void*)pa, 0, PGSIZE);
        mappages(p->pagetable, jlength, PGSIZE,  pa, perm);
        if(DEBUG) printf("mmap: successfully mappages %p\n", jlength);
      }

      break;
    }
  }
  if(start==0){
    if(DEBUG) printf("mmap: nowhere to start");
    return -1;
  }

  //increment file refs
  filedup(f);
  if(flags&MAP_SHARED && (!f->writable) && prot&PROT_WRITE){
    if(DEBUG) printf("mmap: MAP_SHARED PROT_WRITE O_RDONLY\n");
    goto FILECLOSE;
  }
  //get global vma slot
  acquire(&vma.lock);
  for(int i=0; i<20; i++){
    if(vma.vmae[i].length==0){
      vmaep_glob = vma.vmae+i;
      vma.vmae[i].start = start;
      vma.vmae[i].length = length;
      vma.vmae[i].prot = prot;
      vma.vmae[i].flags = flags;
      vma.vmae[i].file_t = f;
      vma.vmae[i].offset = offset;
      break;
    }
  }
  release(&vma.lock);
  if(vmaep_glob==0){
    if(DEBUG) printf("mmap: vmaep_glob==0\n");
    goto FILECLOSE;
  }
  //get process vmaep slot
  for(int i=0; i<16; i++){
    if(p->vmaep[i]==0){
      vmaep_proc = vmaep_glob;
      p->vmaep[i] = vmaep_glob;
      break;
    }
  }

  if(vmaep_proc==0){
    if(DEBUG) printf("mmap: vmaep_proc==0\n");
    goto FILECLOSE;
  }

  if(DEBUG) printf("mmap: return start %p \n", start);
  return (uint64)start;
FILECLOSE:
  fileclose(f);
//ERRORRET:
  return -1;
}

uint64
sys_munmap()
{
  uint64 addrint;
  void *addr;
  int length;
  struct proc *p = myproc();
  struct vmae *vmaep_proc=0;

  if(argaddr(0, &addrint) < 0 )
    return -1;
  addr = (void*)addrint;
  if(argint(1, &length) < 0 )
    return -1;
  if(DEBUG) printf("munmap: args addr %p length %p\n", addr, length);
  acquire(&vma.lock);
  //get vmaep_proc
  int i;
  for(i=0; i < 16; i++){
    if(DEBUG) printf("munmap: p->vmaep[%d]\n", i);
    if(p->vmaep[i] && p->vmaep[i]->start <= addr && (addr+length) <= p->vmaep[i]->start + p->vmaep[i]->length){
      if(DEBUG) printf("munmap: vmaep_proc = p->vmaep[%d]\n", i);
      vmaep_proc = p->vmaep[i];
      break;
    }
  }
  if(DEBUG) printf("munmap: got vmaep_proc slot\n");
  if(vmaep_proc==0){
    if(DEBUG) printf("munmap: vmaep_proc==0\n");
    release(&vma.lock);
    return -1;
  }
  //check not punch a hole
  if(vmaep_proc->start < addr && addr+length < vmaep_proc->start+vmaep_proc->length){
    if(DEBUG) printf("munmap: punched a hole\n");
    release(&vma.lock);
    return -1;
  }
  release(&vma.lock);
  //possibly write back to file
  if(vmaep_proc->flags&MAP_SHARED){
    if(DEBUG) printf("munmap: write back to file\n");
    pte_t *pte;
    for(uint64 pg = (uint64)addr; pg < (uint64)addr+length; pg=pg+PGSIZE){
      pte = walk(p->pagetable, pg, 0);
      vmaep_proc->file_t->off = vmaep_proc->offset + pg - (uint64)addr;
      if(*pte & PTE_D) filewrite(vmaep_proc->file_t, pg, PGSIZE);
    }
  }

  if(DEBUG) printf("munmap: possibly close file and evict vmae\n");
  if(vmaep_proc->start ==addr && length == vmaep_proc->length){
    if(DEBUG) printf("munmap: whole\n");
    vmaeclear(vmaep_proc);
    //*(p->vmaep[i]) = 0; wrong
    p->vmaep[i] = 0;
  }
  else if(vmaep_proc->start == addr){
    if(DEBUG) printf("munmap: head\n");
    vmaep_proc->start = addr + length;
    vmaep_proc->length -= length;
    if(DEBUG) printf("munmap: new vmaep_proc->start %p\n", vmaep_proc->start);
  }
  else if(addr+length == vmaep_proc->start+vmaep_proc->length){
    if(DEBUG) printf("munmap: tail\n");
    vmaep_proc->length = addr - vmaep_proc->start;
  }

  //uvmmap and remap to what?
  if(DEBUG) printf("munmap: uvmunmap\n");
  for(uint64 pg = (uint64)addr; pg < (uint64)addr+length; pg=pg+PGSIZE){
    uvmunmap(p->pagetable, pg, 1, 1);
  }
  if(DEBUG) printf("munmap: ret\n");

  return 0;
}
