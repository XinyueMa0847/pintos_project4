#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "devices/timer.h"
#include "threads/thread.h"
#include "devices/block.h"
/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define WRITE_BEHIND_ALARM 100*TIMER_FREQ

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
/* struct inode_disk */
/*   { */
/*     block_sector_t start;               /\* First data sector. *\/ */
/*     off_t length;                       /\* File size in bytes. *\/ */
/*     unsigned magic;                     /\* Magic number. *\/ */
/*     uint32_t unused[125];               /\* Not used. *\/ */
/*   }; */


/* array of 64 buffer heads */
static struct buffer_head buffer_heads[64]; 
/* list of buffer blocks */
static struct list buffer_cache; 
static struct list_elem* cache_hand;
struct buffer_head* get_buffer_head(block_sector_t sector);
struct buffer_head* find_buffer_head(void);
struct buffer_head* find_empty_buffer(void);
struct buffer_head*  buffer_select_victim(void);
void buffer_flush_to_disk(struct buffer_head* entry);
void buffer_release(struct buffer_head* entry);
void buffer_read(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size);
void buffer_read(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size);
static void buffer_direct_read(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size);
static void buffer_direct_write(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size);
struct buffer_head* buffer_get(block_sector_t sector);
void buffer_flush_all(void);
void write_behind(void* aux);
bool inode_free_map_allocate(size_t cnt, size_t old,struct inode_disk* inode_disk);
void inode_free_map_deallocate(struct inode_disk* inode_disk);
void cache_init(void){
  int i;
  list_init(&buffer_cache);  
  cache_hand = NULL;
  for ( i =0; i<64; i++){
    lock_init(&buffer_heads[i].extend_lock);
    lock_init(&buffer_heads[i].evict_lock);
    cond_init(&buffer_heads[i].accessed);
    cond_init(&buffer_heads[i].evicted);
    buffer_heads[i].accessing = false;
    buffer_heads[i].evicting = false;
    //list_push_back(&buffer_cache, &buffer_heads[i].elem);
  }
  //thread_create("write_behind",PRI_DEFAULT,write_behind,NULL);
}
/* read the buffer block specified by BUFFER_HEAD into BUFFER */
static void buffer_direct_read(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size){
  
  ASSERT(buffer_head!=NULL); 
  /* Read into BUFFER */
  memcpy(buffer,buffer_head->data+ofs,chunk_size);
  /* update buffer head*/ 
  /* buffer_head->in_use = false;  */
  buffer_head->access = true; 
} 

static void buffer_direct_write(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size){
  
  ASSERT(buffer_head!=NULL);
  /* Write from BUFFER to buffer cache */
  memcpy(buffer_head->data+ofs, buffer,chunk_size);  
  /* update buffer_head */
  buffer_head->access = true; 
  buffer_head->dirty = true; 
}

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}




static block_sector_t direct_sec_idx(off_t ofs)
{
  return (ofs/BLOCK_SECTOR_SIZE);
}
static block_sector_t indirect_sec_idx(off_t ofs)
{
  return (ofs - BLOCK_SECTOR_SIZE*DIRECT_BLOCK_ENTRIES)/(BLOCK_SECTOR_SIZE);
}
static block_sector_t d_indirect_sec_idx_first(off_t ofs) 
{
  return (ofs - BLOCK_SECTOR_SIZE*(DIRECT_BLOCK_ENTRIES+INDIRECT_BLOCK_ENTRIES))/(BLOCK_SECTOR_SIZE * INDIRECT_BLOCK_ENTRIES);
}
static block_sector_t d_indirect_sec_idx_second(off_t ofs) 
{
  return (ofs - BLOCK_SECTOR_SIZE*(DIRECT_BLOCK_ENTRIES+INDIRECT_BLOCK_ENTRIES))%(BLOCK_SECTOR_SIZE * INDIRECT_BLOCK_ENTRIES);
}
/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns UINT_MAX if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  block_sector_t result = (block_sector_t)(-1);; 
  struct buffer_head* inode_disk_head = buffer_get(inode->sector);
  struct inode_disk* inode_disk = (struct inode_disk * )inode_disk_head->data;
  //struct inode_disk* inode_disk = calloc(1,sizeof(struct inode_disk));
  //buffer_read(inode_disk_head,(void*) inode_disk,0,BLOCK_SECTOR_SIZE);

  if(pos>=inode_disk->length)
    goto done;
  block_sector_t idx = direct_sec_idx(pos); 
  /* a direct block */ 
  if (idx < DIRECT_BLOCK_ENTRIES){ 
    result = inode_disk->direct_map_table[idx];
    goto done; 
  }	
 
  block_sector_t indirect_idx = indirect_sec_idx(pos);
  /* an indirect block */ 
  if( indirect_idx < INDIRECT_BLOCK_ENTRIES) 
    {
      struct buffer_head* entry = buffer_get(inode_disk->indirect_block_sec); 
      /* read the indirect block */
      block_sector_t* indirect_block = (block_sector_t*) entry->data;  
      //buffer_read(entry,(void*)indirect_block,0,BLOCK_SECTOR_SIZE);
      result = indirect_block[indirect_idx]; 
      //free(indirect_block);
      goto done;
    }

  /* a doubly indirect block */ 
  else if (idx < D_INDIRECT_BLOCK_ENTRIES)
    {
      int first_idx = d_indirect_sec_idx_first(pos);
      int second_idx = d_indirect_sec_idx_second(pos);
      /* read first level sector */ 
      struct buffer_head* first_entry = buffer_get(inode_disk->double_indirect_block_sec);
      // block_sector_t* first_indirect = calloc(1,BLOCK_SECTOR_SIZE);
      // buffer_read(first_entry, (void*) first_indirect,0,BLOCK_SECTOR_SIZE);
      block_sector_t* first_indirect = (block_sector_t*)first_entry->data;
      /* read second level sector */ 
      struct buffer_head* second_entry = buffer_get(first_indirect[first_idx]); 
      // block_sector_t* second_indirect =  calloc(1,BLOCK_SECTOR_SIZE); 
      //buffer_read(second_entry,(void*) second_indirect,0,BLOCK_SECTOR_SIZE); 
      block_sector_t* second_indirect = second_entry->data; 
      result = second_indirect[second_idx]; 
      //free(first_indirect);
      //free(second_indirect);
      goto done;
    }
 done:
  //free(inode_disk);
  return result; 
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* given total number of sectors CNT, determine how many direct blocks,indirect blocks,how many indirect blocks in the doubly indirect block and how many blocks are in the last blocks of doubly indirect block*/ 
static void sectors_divide(size_t cnt, size_t* direct, size_t* indirect, size_t* d_indirect_blocks,size_t* d_indirect_in_last){

  *indirect = 0; 
  *d_indirect_blocks = 0; 
  *d_indirect_in_last = 0; 
  if(cnt<=DIRECT_BLOCK_ENTRIES){
    *direct = cnt;
    return; 
  }
  *direct = DIRECT_BLOCK_ENTRIES; 
  size_t remaining = cnt - DIRECT_BLOCK_ENTRIES; 
  if ( remaining <= INDIRECT_BLOCK_ENTRIES){
    *indirect = remaining; 
    return; 
  }
  *indirect = INDIRECT_BLOCK_ENTRIES; 
  remaining = remaining - INDIRECT_BLOCK_ENTRIES; 
  *d_indirect_blocks = remaining / INDIRECT_BLOCK_ENTRIES; 
  size_t remainder = remaining % INDIRECT_BLOCK_ENTRIES; 
  if (remainder!=0){
    *d_indirect_blocks += 1;
    *d_indirect_in_last = remainder; 
    return; 
  }
  return; 
}
/* /\* allocate CNT sectors for INODE_DISK, return true if successful, */
/*    false otherwise*\/  */
/* bool inode_free_map_allocate(size_t cnt,struct inode_disk* inode_disk){ */
/*   size_t allocated =0;  */
/*   //bool success;  */
/*   size_t direct_blocks = 0;  */
/*   size_t indirect_blocks = 0;  */
/*   size_t d_indirect_blocks = 0; */
/*   size_t d_indirect_in_last = 0; */
/*   size_t i,j; */
/*   /\* determind how many sectors to allocate in each region *\/ */
/*   sectors_divide(cnt,&direct_blocks,&indirect_blocks,&d_indirect_blocks,&d_indirect_in_last);  */
  
/*   /\* allocate direct blocks *\/  */
/*   for(i=0;i<direct_blocks;i++){ */
/*     if(free_map_allocate(1,&inode_disk->direct_map_table[i])){ */
/*       allocated++;  */
/*     } */
/*     else { */
/*       inode_free_map_deallocate(inode_disk); */
/*       return false; */
/*     }  */
/*   } */

/*   if(indirect_blocks==0){ */
/*     return true;  */
/*   } */

/*   /\* allocate indirect blocks *\/ */
/*   if(!(free_map_allocate(1,&inode_disk->indirect_block_sec))){ */
/*     inode_free_map_deallocate(inode_disk); */
/*     return false;     */
/*   } */
/*   struct buffer_head* indirect_block_head = buffer_get(inode_disk->indirect_block_sec); */
/*   block_sector_t* indirect_block = NULL; */
/*   buffer_read(indirect_block_head,(void*)indirect_block,0,BLOCK_SECTOR_SIZE); */
/*   for(i=0;i<indirect_blocks;i++){ */
/*     if(free_map_allocate(1,&indirect_block[i])){ */
/*       allocated++;  */
/*     } */
/*     else { */
/*       /\* need to write it so it can be traced for deallocation *\/ */
/*       indirect_block_head = buffer_get(inode_disk->indirect_block_sec); */
/*       buffer_write(indirect_block_head,(void*)indirect_block,0,BLOCK_SECTOR_SIZE);  */
/*       inode_free_map_deallocate(inode_disk); */
/*       free(indirect_block); */
/*       return false; */
/*    } */
/*   } */
/*   /\* write the allocated indirect block *\/ */
/*   indirect_block_head = buffer_get(inode_disk->indirect_block_sec); */
/*   buffer_write(indirect_block_head,(void*)indirect_block,0,BLOCK_SECTOR_SIZE); */
/*   free(indirect_block); */
/*   if(d_indirect_blocks==0){ */
/*     return true;  */
/*   }   */

/*   /\* allocate doubly indirect block *\/  */
/*   if(!(free_map_allocate(1,&inode_disk->double_indirect_block_sec))){ */
/*     inode_free_map_deallocate(inode_disk); */
/*     return false;     */
/*   }  */
  
/*   /\* read the allocacted doubly indirect block*\/ */
/*   struct buffer_head* d_indirect_block_head = buffer_get(inode_disk->double_indirect_block_sec); */
/*   block_sector_t* d_indirect_block = NULL; */
/*   buffer_read(d_indirect_block_head,(void*)d_indirect_block,0,BLOCK_SECTOR_SIZE); */

/*   size_t blocks_to_allocate = INDIRECT_BLOCK_ENTRIES; */
/*   /\* allocate indirect blocks *\/ */
/*   for(i=0;i<d_indirect_blocks;i++){ */
/*     if(!(free_map_allocate(1,&d_indirect_block[i]))){ */
/*       d_indirect_block_head = buffer_get(inode_disk->double_indirect_block_sec); */
/*       buffer_write(d_indirect_block_head,(void*)d_indirect_block,0,BLOCK_SECTOR_SIZE); */
/*       inode_free_map_deallocate(inode_disk); */
/*       free(d_indirect_block); */
/*       return false;     */
/*     }     */
/*     /\* read the allocated indirect block*\/ */
/*     struct buffer_head* d_second_indirect_block_head = buffer_get(d_indirect_block[i]); */
/*     block_sector_t* d_second_indirect_block = NULL;  */
/*     buffer_read(d_second_indirect_block_head,(void*)d_second_indirect_block,0,BLOCK_SECTOR_SIZE);  */
/*     /\* allocate data blocks *\/ */
/*     if (i==d_indirect_blocks-1) */
/*       blocks_to_allocate = d_indirect_in_last; */
/*     for(j=0;j<blocks_to_allocate;j++){ */
/*       if(!free_map_allocate(1,&d_second_indirect_block[j])){ */
/* 	d_second_indirect_block_head = buffer_get(d_indirect_block[i]); */
/* 	buffer_write(d_second_indirect_block_head,(void*)d_second_indirect_block,0,BLOCK_SECTOR_SIZE); */
/* 	inode_free_map_deallocate(inode_disk); */
/* 	free(d_second_indirect_block); */
/* 	return false;  */
/*       } */
/*       else */
/* 	allocated++; */
/*     } */
/*     /\* write to buffer*\/  */
/*     d_second_indirect_block_head = buffer_get(d_indirect_block[i]); */
/*     buffer_write(d_second_indirect_block_head,(void*)d_second_indirect_block,0,BLOCK_SECTOR_SIZE); */
/*     free(d_second_indirect_block); */
/*   } */
/*   d_indirect_block_head = buffer_get(inode_disk->double_indirect_block_sec); */
/*   buffer_write(d_indirect_block_head,(void*)d_indirect_block,0,BLOCK_SECTOR_SIZE); */
/*   free(d_indirect_block); */
/*   return true; */
/* } */

/* allocate in total CNT sectors for INODE_DISK which has OLD sectors
   allocated already */
bool inode_free_map_allocate(size_t cnt,size_t old,struct inode_disk* inode_disk){
  size_t direct_blocks = 0; 
  size_t indirect_blocks = 0; 
  size_t d_indirect_blocks = 0;
  size_t d_indirect_in_last = 0;

  size_t o_direct_blocks = 0; 
  size_t o_indirect_blocks = 0; 
  size_t o_d_indirect_blocks = 0;
  size_t o_d_indirect_in_last = 0;

  size_t i,j;
  size_t allocated;
  bool extend; 
  /* if the inode was allocated before-->file extension*/
  if(old!=0){
    extend=true;
  }
  /* check how many sectors are allocated in each region */ 
  sectors_divide(old,&o_direct_blocks,&o_indirect_blocks,&o_d_indirect_blocks,&o_d_indirect_in_last); 
  sectors_divide(cnt,&direct_blocks,&indirect_blocks,&d_indirect_blocks,&d_indirect_in_last); 
  /* allocate direct blocks */ 
 for(i=o_direct_blocks;i<direct_blocks;i++){
    if(free_map_allocate(1,&inode_disk->direct_map_table[i])){
      allocated++; 
    }
    else {
      if(!extend)
	inode_free_map_deallocate(inode_disk);
      return false;
    } 
  }

  if(indirect_blocks==0){
    goto success;
  }  
  /* allocate indirect blocks */
  if(o_indirect_blocks==0){
    if(!(free_map_allocate(1,&inode_disk->indirect_block_sec))){
      if(!extend)
	inode_free_map_deallocate(inode_disk);
      return false;    
    }
  }
  struct buffer_head* indirect_block_head = buffer_get(inode_disk->indirect_block_sec);
  //block_sector_t* indirect_block = calloc(1,BLOCK_SECTOR_SIZE);
  //buffer_read(indirect_block_head,(void*)indirect_block,0,BLOCK_SECTOR_SIZE);
  block_sector_t* indirect_block = (block_sector_t*)indirect_block_head->data;
  for(i=o_indirect_blocks;i<indirect_blocks;i++){
    block_sector_t sector; 
    if(free_map_allocate(1,&sector)){
      buffer_write(indirect_block_head,&sector,i*sizeof(block_sector_t),sizeof(block_sector_t));
      allocated++; 
    }
    else {
      /* need to write it so it can be traced for deallocation */
      indirect_block_head = buffer_get(inode_disk->indirect_block_sec);
      //buffer_write(indirect_block_head,(void*)indirect_block,0,BLOCK_SECTOR_SIZE); 
      if(!extend)
	inode_free_map_deallocate(inode_disk);
      //  free(indirect_block);
      return false;
   }
  }
  /* write the allocated indirect block */
  indirect_block_head = buffer_get(inode_disk->indirect_block_sec);
  buffer_write(indirect_block_head,(void*)indirect_block,0,BLOCK_SECTOR_SIZE);
  free(indirect_block);


  if(d_indirect_blocks==0){
    goto success; 
  }
  /* allocate 32 sectors max due to 8MB limit */
  d_indirect_blocks = d_indirect_blocks > D_INDIRECT_BLOCK_FILE_MAX? D_INDIRECT_BLOCK_FILE_MAX: d_indirect_blocks; 
  /* allocate doubly indirect block */ 
  if(o_d_indirect_blocks==0){
    if(!(free_map_allocate(1,&inode_disk->double_indirect_block_sec))){
      if(!extend)
	inode_free_map_deallocate(inode_disk);
      return false;    
    } 
  }

  /* read the allocacted doubly indirect block*/
  struct buffer_head* d_indirect_block_head = buffer_get(inode_disk->double_indirect_block_sec);
  //block_sector_t* d_indirect_block = calloc(1,BLOCK_SECTOR_SIZE);
  // buffer_read(d_indirect_block_head,(void*)d_indirect_block,0,BLOCK_SECTOR_SIZE);
  block_sector_t* d_indirect_block = (block_sector_t*) d_indirect_block_head->data;
  if (o_d_indirect_blocks == D_INDIRECT_BLOCK_FILE_MAX) 
    d_indirect_blocks = D_INDIRECT_BLOCK_FILE_MAX; 

  /* allocate in the last unfull indirect block */ 
  if(o_d_indirect_in_last != INDIRECT_BLOCK_ENTRIES){
    /* read last indirect block */ 
    struct buffer_head* o_d_indirect_lastblock_head = buffer_get(d_indirect_block[o_d_indirect_blocks-1]);
    // block_sector_t* o_d_indirect_lastblock =calloc(1,BLOCK_SECTOR_SIZE); 
    // buffer_read(o_d_indirect_lastblock_head ,(void*) o_d_indirect_lastblock,0,BLOCK_SECTOR_SIZE); 
    block_sector_t* o_d_indirect_lastblock = (block_sector_t*) o_d_indirect_lastblock_head->data; 
   
    /* allocate */ 
    size_t allocate_until = d_indirect_blocks>o_d_indirect_blocks? 128:
      (d_indirect_in_last > D_INDIRECT_LAST_MAX 
       && o_d_indirect_blocks==D_INDIRECT_BLOCK_FILE_MAX)?
      D_INDIRECT_LAST_MAX : d_indirect_in_last; 

    for(i=o_d_indirect_in_last;i<allocate_until;i++){
     if(!(free_map_allocate(1,&o_d_indirect_lastblock[i]))){
      o_d_indirect_lastblock_head =buffer_get(d_indirect_block[o_d_indirect_blocks-1]);
      buffer_write(o_d_indirect_lastblock_head ,(void*) o_d_indirect_lastblock,0,BLOCK_SECTOR_SIZE); 
      if(!extend)
	inode_free_map_deallocate(inode_disk);
      return false;    
     }
     else 
       allocated++; 
    }
}
  size_t blocks_to_allocate = INDIRECT_BLOCK_ENTRIES; 
 /* allocate indirect blocks */
  for(i=o_d_indirect_blocks;i<d_indirect_blocks;i++){
    if(!(free_map_allocate(1,&d_indirect_block[i]))){
      d_indirect_block_head = buffer_get(inode_disk->double_indirect_block_sec);
      buffer_write(d_indirect_block_head,(void*)d_indirect_block,0,BLOCK_SECTOR_SIZE);
      if(!extend)
	inode_free_map_deallocate(inode_disk);
      return false;    
    }  
    /* read the allocated indirect block*/
    struct buffer_head* d_second_indirect_block_head = buffer_get(d_indirect_block[i]);
    //block_sector_t* d_second_indirect_block = calloc(1,BLOCK_SECTOR_SIZE);  
    // buffer_read(d_second_indirect_block_head,(void*)d_second_indirect_block,0,BLOCK_SECTOR_SIZE); 
    block_sector_t* d_second_indirect_block = (block_sector_t*) d_second_indirect_block_head->data;
    if (i==d_indirect_blocks-1)
      blocks_to_allocate = d_indirect_in_last > D_INDIRECT_LAST_MAX? D_INDIRECT_LAST_MAX:d_indirect_in_last; ;
    for(j=0;j<blocks_to_allocate;j++){
      if(!free_map_allocate(1,&d_second_indirect_block[j])){
	d_second_indirect_block_head = buffer_get(d_indirect_block[i]);
	buffer_write(d_second_indirect_block_head,(void*)d_second_indirect_block,0,BLOCK_SECTOR_SIZE);
	if(!extend)
	  inode_free_map_deallocate(inode_disk);
	return false; 
      }
      else
	allocated++;
    }  
    /* write to buffer*/ 
    d_second_indirect_block_head = buffer_get(d_indirect_block[i]);
    buffer_write(d_second_indirect_block_head,(void*)d_second_indirect_block,0,BLOCK_SECTOR_SIZE);
  }
  d_indirect_block_head = buffer_get(inode_disk->double_indirect_block_sec);
  buffer_write(d_indirect_block_head,(void*)d_indirect_block,0,BLOCK_SECTOR_SIZE);
  //free(d_indirect_block);
 success:
  return true;  
}
/* deallocate all sectors used by INODE_DISK */ 
void inode_free_map_deallocate(struct inode_disk* inode_disk){
  size_t cnt = bytes_to_sectors(inode_disk->length);  
  size_t direct = 0;
  size_t indirect = 0; 
  size_t d_indirect_blocks=0;
  size_t d_indirect_in_last=0; 
  size_t i;

  sectors_divide(cnt, &direct,&indirect,&d_indirect_blocks,&d_indirect_in_last);
  // direct blocks
  for(i=0;i<direct;i++){
    free_map_release(inode_disk->direct_map_table[i],1);
    inode_disk->direct_map_table[i] = (block_sector_t)(-1);
  }
  
  //indirect blocks
  if (indirect ==0 ) 
    return;
  struct buffer_head* indirect_block_head = buffer_get(inode_disk->indirect_block_sec);
  block_sector_t* indirect_block=calloc(1,BLOCK_SECTOR_SIZE);
  buffer_read(indirect_block_head,(void*)indirect_block,0,BLOCK_SECTOR_SIZE);
  for (i=0;i<indirect;i++){
    free_map_release(indirect_block[i],1); 
    indirect_block[i] = (block_sector_t)(-1);
  }
  indirect_block_head = buffer_get(inode_disk->indirect_block_sec);
  buffer_write(indirect_block_head,indirect_block,0,BLOCK_SECTOR_SIZE);
  free(indirect_block);
  inode_disk->indirect_block_sec = (block_sector_t)(-1);
  //double indirect blocks
  if (d_indirect_blocks==0) 
    return; 
  struct buffer_head* d_indirect_block_head = buffer_get(inode_disk->double_indirect_block_sec); 
  block_sector_t* d_indirect_block = calloc(1,BLOCK_SECTOR_SIZE); 
  buffer_read(d_indirect_block_head,(void*)d_indirect_block,0,BLOCK_SECTOR_SIZE);
 

  for(i=0;i<d_indirect_blocks;i++){
    struct buffer_head* d_second_indirect_block_head = buffer_get(d_indirect_block[i]); 
    block_sector_t* d_second_indirect_block = calloc(1,BLOCK_SECTOR_SIZE);
    buffer_read(d_second_indirect_block_head,(void*)d_second_indirect_block,0,BLOCK_SECTOR_SIZE);
  
    size_t j;
    size_t until = INDIRECT_BLOCK_ENTRIES;  
    if (i==d_indirect_blocks-1)
      until = d_indirect_in_last; 
    for(j=0;j<until;j++){
      free_map_release(d_second_indirect_block[i],1);
      d_second_indirect_block[i]= (block_sector_t)(-1);
    }
    d_second_indirect_block_head = buffer_get(d_indirect_block[i]); 
    buffer_write(d_second_indirect_block_head,(void*)d_second_indirect_block,0,BLOCK_SECTOR_SIZE);
    free(d_second_indirect_block);  
    d_indirect_block[i] = (block_sector_t)(-1);
  }
  d_indirect_block_head = buffer_get(inode_disk->double_indirect_block_sec); 
  buffer_write(d_indirect_block_head,(void*)d_indirect_block,0,BLOCK_SECTOR_SIZE);
  free(d_indirect_block); 
  inode_disk->double_indirect_block_sec = (block_sector_t)(-1);
  
}


/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length,int is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
 
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
     
      if (inode_free_map_allocate(sectors,0,disk_inode))
        {
          //block_write (fs_device, sector, disk_inode);
          struct buffer_head* inode_head = buffer_get(sector); 
	  buffer_write(inode_head,(void*)disk_inode,0,BLOCK_SECTOR_SIZE);
          success = true; 
        }
      free (disk_inode);
    }

  return success;
}
/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;

  /* /\* bring in the on_disk inode *\/  */
  struct buffer_head* entry = buffer_get(inode->sector); 
  struct inode_disk* inode_disk = calloc(1,sizeof(struct inode_disk)); 
  buffer_read(entry,(void*)inode_disk,0,BLOCK_SECTOR_SIZE);
  inode->length = inode_disk->length;
  inode->is_dir = inode_disk->is_dir;
  inode->pos = 0;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  free(inode_disk);
  lock_init(&inode->lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        { 
	  struct buffer_head* inode_disk_head = buffer_get(inode->sector);
	  struct inode_disk* inode_disk = calloc(1,sizeof(struct inode_disk));
	  buffer_read(inode_disk_head,(void*)inode_disk,0,BLOCK_SECTOR_SIZE);

	  inode_free_map_deallocate(inode_disk);
	  free_map_release(inode->sector,1);
	  free(inode_disk);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      if(sector_idx == (block_sector_t)(-1)){
	return 0 ;
	//sector_idx = byte_to_sector (inode,inode_length(inode)-1);
      }
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) */
      /*   { */
      /*     /\* Read full sector directly into caller's buffer. *\/ */
      /*     block_read (fs_device, sector_idx, buffer + bytes_read); */
      /*   } */
      /* else  */
      /*   { */
      /*     /\* Read sector into bounce buffer, then partially copy */
      /*        into caller's buffer. *\/ */
      /*     if (bounce == NULL)  */
      /*       { */
      /*         bounce = malloc (BLOCK_SECTOR_SIZE); */
      /*         if (bounce == NULL) */
      /*           break; */
      /*       } */
      /*     block_read (fs_device, sector_idx, bounce); */
      /*     memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size); */
      /*   } */

      /* Try to read from buffer cache f*/ 
      struct buffer_head* entry = get_buffer_head(sector_idx); 
      if(entry==NULL){/* cache miss */ 
	/* find a empty entry from buffer heads */
	if(list_size(&buffer_cache)<64){
	  if((entry = find_empty_buffer())==NULL)
	    return -1;
	}
	/* select a victim entry */ 
	else{
	  if((entry=buffer_select_victim())==NULL)
	    return -1; 
	  if (entry->dirty==true)
	    buffer_flush_to_disk(entry);
	  buffer_release(entry); 
	}
	/* read a block into buffer cache */ 
	entry->data = malloc(BLOCK_SECTOR_SIZE);
	block_read (fs_device, sector_idx, entry->data);
	entry->in_use = true; 
	entry->on_disk_sector = sector_idx; 	
	list_push_back(&buffer_cache,&entry->elem);
      }
      
      /* at this point, SECTOR_IDX must have been allocated */
      entry = buffer_get(sector_idx);
      buffer_read(entry, buffer+bytes_read, sector_ofs,chunk_size); 
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* /\* allocate enough sectors to accommadate EXTRA more bytes for INODE *\/ */
/* static bool inode_extend_sector(struct inode* inode, off_t extra){ */
  
/*   size_t new_sectors,old_sectors;  */
/*   /\* read the on_disk inode *\/  */
/*   struct buffer_head* inode_head = buffer_get(inode->sector);  */
/*   struct inode_disk* inode_disk = NULL;  */
/*   buffer_direct_read(inode_head,(void*) inode_disk,0,BLOCK_SECTOR_SIZE);  */

/*   /\* validate file size *\/  */
/*   if( extra  + inode_disk->length > MAX_FILE_SIZE) */
/*     return false; */
  
/*   /\* calculate number of sectors to be allocated *\/ */
/*   old_sectors = bytes_to_sectors(inode_disk->length) */
/*   new_sectors = bytes_to_sectors(inode_disk->length+extra) - old_sectors;    */
/* } */
/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 

{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  off_t old_offset = offset;
  off_t old_size = size;
  off_t length = inode_length(inode);
  bool extend = false;
  int extended = 0;
  struct inode_disk* inode_disk = calloc(1,sizeof(struct inode_disk));
  if (inode->deny_write_cnt)
    return 0;
  
  while (size > 0) 
    {
      if(length == 0){
	extend = true;
      }
      /* update the inode if the file was extended */ 
      if (extend){  
	/* allocate new sectors */ 
	size_t new_cnt = bytes_to_sectors(old_size+old_offset);
	size_t old_cnt = bytes_to_sectors(length);
	struct buffer_head* inode_disk_head = buffer_get(inode->sector);
	//struct inode_disk* inode_disk = calloc(1,sizeof(inode_disk));
	buffer_read(inode_disk_head,(void*)inode_disk,0,BLOCK_SECTOR_SIZE);
	inode_free_map_allocate(new_cnt,old_cnt,inode_disk);
	inode_disk->length = old_size+old_offset;
	inode_disk_head = buffer_get(inode->sector);
	buffer_write(inode_disk_head,(void*)inode_disk,0,BLOCK_SECTOR_SIZE);
	length =  inode_length(inode);
	extend = false;
	extended = 1;
	/* write data */ 
      }
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      if(sector_idx == (block_sector_t)(-1))
	sector_idx = byte_to_sector (inode, length-1);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      if(inode_left==0)
	extend=true;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0 && extend == false)
        break;
      /* try to write to buffer cache */ 
      struct buffer_head* entry = get_buffer_head(sector_idx);
      if(entry==NULL){/* cache miss */
	if(list_size(&buffer_cache)<64){
	  if ((entry = find_empty_buffer())==NULL){ 
	    bytes_written = -1;
	    goto done;
	  }
	}
	/* select a victim entry */ 
	else{
	  if((entry = buffer_select_victim())==NULL){
	    bytes_written = -1;
	    goto done;
	  }
	  if(entry->dirty == true)
	    buffer_flush_to_disk(entry); 
	  buffer_release(entry); 
	}
	/* read from on disk block to cache */ 
	entry->data = malloc(BLOCK_SECTOR_SIZE);
	block_read(fs_device, sector_idx, entry->data); 
	entry->in_use = true; 
	entry->on_disk_sector = sector_idx;
	list_push_back(&buffer_cache,&entry->elem);
      }
      
      entry = buffer_get(sector_idx);
      buffer_write(entry,(void*)(buffer+bytes_written), sector_ofs,chunk_size);
     
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
      /* check for need of file extension */ 
      if(bytes_written+old_offset>length && extended!=1){
	extend = true; 
      }
    }
 done:
  free(inode_disk);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  off_t length = 0;
  struct buffer_head* inode_disk_head = buffer_get(inode->sector);
  struct inode_disk* inode_disk = calloc(1,sizeof(struct inode_disk));
  buffer_read(inode_disk_head, (void*)inode_disk,0,BLOCK_SECTOR_SIZE);
  length = inode_disk->length;
  return length;
}

/* Buffer cache */

/* return the buffer_head corresponding ot SECTOR, NULL if no such
   entry is found */
struct buffer_head* get_buffer_head(block_sector_t sector){
  int i;
  for (i =0; i<64; i++){
    if (buffer_heads[i].on_disk_sector == sector){
      if (buffer_heads[i].in_use==false)
	return NULL; 
      buffer_heads[i].in_use = true; /* buffer is in use */
      return &buffer_heads[i];
    }
  }
  return NULL;
}

/* find an empty buffer from the cache */ 
struct buffer_head* find_empty_buffer(void){
  int i; 
  for (i=0;i<64;i++){
    if(buffer_heads[i].in_use == false)
      return &buffer_heads[i]; 
    else 
      continue;
  }
  return NULL; 
}
struct buffer_head* buffer_select_victim(void){

  ASSERT(list_size(&buffer_cache)==64);
  if(cache_hand == NULL)/*first eviction*/ 
    cache_hand = list_begin(&buffer_cache);
  struct list_elem* e; 
  struct buffer_head* entry;  
  for (e=cache_hand;e!=list_end(&buffer_cache);e=list_next(e)){
    entry = list_entry(e,struct buffer_head, elem); 
    if(entry->access == true){
      entry->access = false; 
      cache_hand = (cache_hand==list_end(&buffer_cache)->prev)? 
	list_begin(&buffer_cache):list_next(cache_hand); 
    }
    else{  
      return entry; 
    }
  }
  /* if all buffers are accessed recently, evict the first page which
     is in_use for the longest */
  entry = list_entry(list_begin(&buffer_cache),struct buffer_head, elem); 
  return entry; 
}
void buffer_flush_to_disk(struct buffer_head* entry){

  ASSERT(entry->in_use == true);

  lock_acquire(&entry->evict_lock);
  entry->evicting = true;
  if(entry->accessing){
    cond_wait(&entry->accessed,&entry->evict_lock);
  }
  block_write(fs_device,entry->on_disk_sector,entry->data); 
  entry->in_use = false; 
  entry->evicting = false;
  cond_signal(&entry->evicted,&entry->evict_lock);
  lock_release(&entry->evict_lock);
}
void buffer_release(struct buffer_head* entry){
  /* reset victim entry from buffer head */
  ASSERT(entry!=NULL);
  entry->in_use = false; 
  entry->dirty = false; 
  entry->access = false; 
  entry->evicting = false; 
  entry->accessing = false;
  memset(entry->data, 0, BLOCK_SECTOR_SIZE); 
  list_remove(&entry->elem);
} 

void buffer_flush_all(void){
  struct list_elem* e; 
  struct buffer_head* entry; 
  for (e = list_begin(&buffer_cache);e!=list_end(&buffer_cache);e=list_next(e)){
    entry = list_entry(e,struct buffer_head, elem); 
    if (entry->dirty==true)
      buffer_flush_to_disk(entry);
    buffer_release(entry);
  }  
}
/* given a sector index, get the buffer_head associated if it is there, or bring it from disk to buffer first otherwise, return the buffer_head */
struct buffer_head* buffer_get(block_sector_t sector){
  struct buffer_head* entry = get_buffer_head(sector); 
  if (entry==NULL){/*cache miss*/
    if(list_size(&buffer_cache)<64){/* cache not full, find an empty buffer */
      if ((entry=find_empty_buffer())==NULL)
	return NULL; 
    }
    else{/* cache full, evict a entry */
      /* find victim */
      if((entry = buffer_select_victim())==NULL)
	return NULL;
      /* flush the buffer to disk if it is dirty */
      if(entry->dirty == true) 
	buffer_flush_to_disk(entry);
      buffer_release(entry);
    }
    entry->data = malloc(BLOCK_SECTOR_SIZE); 
    block_read(fs_device, sector, entry->data); 
    entry->in_use = true; 
    entry->on_disk_sector = sector; 
    list_push_back(&buffer_cache,&entry->elem); 
  }
  /* /\* read from buffer *\/  */
  //lock_acquire(&entry->evict_lock);
  return entry; 
  
}
void buffer_write(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size){
  if(!lock_held_by_current_thread(&buffer_head->evict_lock))
     lock_acquire(&buffer_head->evict_lock); 
  buffer_head->accessing = true;
  if(buffer_head->evicting){
    cond_wait(&buffer_head->evicted,&buffer_head->evict_lock);
  }
  buffer_direct_write(buffer_head,buffer,ofs,chunk_size);
  buffer_head->accessing = false;
  cond_signal(&buffer_head->accessed,&buffer_head->evict_lock);
  lock_release(&buffer_head->evict_lock);
}
void buffer_read(struct buffer_head* buffer_head, void* buffer, off_t ofs, int chunk_size){
  if(!lock_held_by_current_thread(&buffer_head->evict_lock))
    lock_acquire(&buffer_head->evict_lock); 
  if(buffer_head->evicting){
    cond_wait(&buffer_head->evicted,&buffer_head->evict_lock);
  }
  buffer_direct_read(buffer_head,buffer,ofs,chunk_size);
  cond_signal(&buffer_head->accessed,&buffer_head->evict_lock);
  lock_release(&buffer_head->evict_lock);
}
void write_behind(void* aux UNUSED){
  
  while(1){
    timer_sleep(WRITE_BEHIND_ALARM);
    buffer_flush_all();
  }

}
