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


#define NBUCKET 13

struct bucket
{
  struct spinlock lock;
  // Circular linked list of the buffers that hash into this bucket,
  // through prev/next.  Order doesn't matter: we don't maintain LRU.
  struct buf head;
};

struct
{
  // Serializes eviction (the "not cached, need a victim" path) across
  // buckets so we never have to hold two bucket locks at once.
  struct spinlock evict_lock;
  struct buf buf[NBUF];
  struct bucket buckets[NBUCKET];
} bcache;

// A plain blockno % NBUCKET badly clusters when a handful of hot blocks
// happen to be a multiple of NBUCKET apart (e.g. several files' data
// blocks allocated back to back) -- that pins them all on one bucket's
// lock.  Multiply by an odd constant first to spread nearby block
// numbers across buckets.
static inline uint
bhash(uint blockno)
{
  return (blockno * 2654435761u) % NBUCKET;
}

void binit(void)
{
  struct buf *b;

  initlock(&bcache.evict_lock, "bcache_evict");

  for (int i = 0; i < NBUCKET; i++)
  {
    initlock(&bcache.buckets[i].lock, "bcache_bucket");
    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
  }

  // Hand out the buffers to the buckets round-robin; it doesn't matter
  // which bucket an unused buffer starts in.
  for (b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    initsleeplock(&b->lock, "buffer");
    struct bucket *bkt = &bcache.buckets[(b - bcache.buf) % NBUCKET];
    b->next = bkt->head.next;
    b->prev = &bkt->head;
    bkt->head.next->prev = b;
    bkt->head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct bucket *bkt = &bcache.buckets[bhash(blockno)];

  // Is the block already cached?  Only touch our own bucket's lock.
  acquire(&bkt->lock);
  for (b = bkt->head.next; b != &bkt->head; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bkt->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bkt->lock);

  // Not cached: need to evict a buffer.  Serialize evictions across
  // CPUs with evict_lock so we never have to hold two bucket locks
  // at once (which could deadlock if two CPUs evicted in different
  // bucket orders).
  acquire(&bcache.evict_lock);

  // Someone may have cached this block while we didn't hold any lock.
  acquire(&bkt->lock);
  for (b = bkt->head.next; b != &bkt->head; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bkt->lock);
      release(&bcache.evict_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bkt->lock);

  // Find a victim buffer (refcnt == 0) in any bucket.
  struct buf *victim = 0;
  for (int i = 0; i < NBUCKET; i++)
  {
    struct bucket *src = &bcache.buckets[i];
    acquire(&src->lock);
    for (b = src->head.next; b != &src->head; b = b->next)
    {
      if (b->refcnt == 0)
      {
        victim = b;
        break;
      }
    }
    if (victim)
    {
      // Unlink the victim from its current bucket.
      victim->next->prev = victim->prev;
      victim->prev->next = victim->next;
      release(&src->lock);
      break;
    }
    release(&src->lock);
  }

  if (!victim)
    panic("bget: no buffers");

  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->refcnt = 1;

  // Insert the victim into the target bucket.
  acquire(&bkt->lock);
  victim->next = bkt->head.next;
  victim->prev = &bkt->head;
  bkt->head.next->prev = victim;
  bkt->head.next = victim;
  release(&bkt->lock);

  release(&bcache.evict_lock);
  acquiresleep(&victim->lock);
  return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // The buffer stays in whichever bucket it's already in; we don't
  // maintain any recency order, so there's nothing else to do here.
  struct bucket *bkt = &bcache.buckets[bhash(b->blockno)];
  acquire(&bkt->lock);
  b->refcnt--;
  release(&bkt->lock);
}

void bpin(struct buf *b)
{
  struct bucket *bkt = &bcache.buckets[bhash(b->blockno)];
  acquire(&bkt->lock);
  b->refcnt++;
  release(&bkt->lock);
}

void bunpin(struct buf *b)
{
  struct bucket *bkt = &bcache.buckets[bhash(b->blockno)];
  acquire(&bkt->lock);
  b->refcnt--;
  release(&bkt->lock);
}
