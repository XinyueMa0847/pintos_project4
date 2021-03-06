#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h"
#include <list.h>

/* on_disk inodes keep 124 direct block entries, one indirect and one
   doubly indirect block entries */
#define DIRECT_BLOCK_ENTRIES 122 
#define INDIRECT_BLOCK_ENTRIES 128
#define D_INDIRECT_BLOCK_ENTRIES 128*128
#define D_INDIRECT_BLOCK_FILE_MAX 127
#define D_INDIRECT_LAST_MAX 6
#define MAX_FILE_SIZE 8*1024*1024

struct bitmap;
struct inode_disk
{
  block_sector_t self_sector;
  unsigned magic;                     /* Magic number. */ 
  off_t length;                       /* File size in bytes. */
  int is_dir; 
  block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
  block_sector_t indirect_block_sec; 
  block_sector_t double_indirect_block_sec;
};
/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    //struct inode_disk data;             /* Inode content. */
    struct lock lock; 
    off_t length; 
    int is_dir;
    off_t pos; 		/* only used for directories */
  };
/* buffer cache */
struct buffer_head
{
  struct list_elem elem; 		/* Element in cache list */
  bool in_use;				/* entry is in use or not */ 
  bool dirty; 				/* entry is dirty or not */
  bool access; 				/* entry is accessed or not*/ 
  block_sector_t on_disk_sector; 	/* sector number on disk */ 
  void* data; 				/* virtual address of the
					   associated buffer cache
					   entry */
  /* when a buffer is being read or written, or when it is being evicted, EVICT_LOCK is acquired. Because these two actions are mutually exclusive: a buffer being read should not be evicted, and a buffer being evicted should not be read or written.  */
  struct lock evict_lock; 	  	 
  struct condition evicted; 
  bool evicting; 
  
  struct condition accessed;  		/* make sure buffer in the
					   process of reading or
					   writing will not be
					   evicted */ 
  bool accessing;

  struct lock extend_lock;		/* file extending should be atomic */ 
  struct condition extended;
};
void inode_init (void);
bool inode_create (block_sector_t, off_t,int);
bool inode_free_map_allocate(size_t cnt,size_t old, struct inode_disk* inode_disk);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);


void cache_init(void);
struct buffer_head* get_buffer_head(block_sector_t sector);
struct buffer_head* find_buffer_head(void);
struct buffer_head* find_empty_buffer(void);
struct buffer_head*  buffer_select_victim(void);
void buffer_flush_to_disk(struct buffer_head* entry);
void buffer_release(struct buffer_head* entry);
void buffer_read(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size);
void buffer_write(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size);
struct buffer_head* buffer_get(block_sector_t sector);
void buffer_flush_all(void);
void write_behind(void* aux);
#endif /* filesys/inode.h */
