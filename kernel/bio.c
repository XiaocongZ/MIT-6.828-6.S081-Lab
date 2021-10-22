// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BCACHE_BUCKET 17

struct {
  struct spinlock lock[BCACHE_BUCKET];
  struct buf buf[BCACHE_BUCKET][NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[BCACHE_BUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;
  for(int buci=0; buci<BCACHE_BUCKET; buci++){
    initlock(&(bcache.lock)[buci], "bcache.bucket");

    // Create linked list of buffers
    bcache.head[buci].prev = &bcache.head[buci];
    bcache.head[buci].next = &bcache.head[buci];
    for(b = bcache.buf[buci]; b < bcache.buf[buci]+NBUF; b++){
      b->next = bcache.head[buci].next;
      b->prev = &bcache.head[buci];
      initsleeplock(&b->lock, "buffer");
      bcache.head[buci].next->prev = b;
      bcache.head[buci].next = b;
    }
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint buci = blockno % BCACHE_BUCKET;

  acquire(&bcache.lock[buci]);

  // Is the block already cached?
  for(b = bcache.head[buci].next; b != &bcache.head[buci]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[buci]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head[buci].prev; b != &bcache.head[buci]; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[buci]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint buci = b->blockno % BCACHE_BUCKET;
  acquire(&bcache.lock[buci]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head[buci].next;
    b->prev = &bcache.head[buci];
    bcache.head[buci].next->prev = b;
    bcache.head[buci].next = b;
  }

  release(&bcache.lock[buci]);
}

void
bpin(struct buf *b) {
  uint buci = b->blockno % BCACHE_BUCKET;
  acquire(&bcache.lock[buci]);
  b->refcnt++;
  release(&bcache.lock[buci]);
}

void
bunpin(struct buf *b) {
  uint buci = b->blockno % BCACHE_BUCKET;
  acquire(&bcache.lock[buci]);
  b->refcnt--;
  release(&bcache.lock[buci]);
}
