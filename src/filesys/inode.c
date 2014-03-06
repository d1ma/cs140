#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44



/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  //TODO: remove assert after double indirect implemented
  ASSERT (pos >= 0 && pos < 512*251-1);

  if (pos < inode->readable_length) {
	/* sector_pos starts from 0 */
	off_t sector_pos = pos/BLOCK_SECTOR_SIZE;

	struct inode_disk id;
	cache_read(inode->sector, INVALID_SECTOR_ID, &id, 0, BLOCK_SECTOR_SIZE);

	/*sector_pos in the range of direct index*/
	if (sector_pos < DIRECT_INDEX_NUM) {
		return id.direct_idx[sector_pos];
	}

	/*sector_pos in the range of single indirect index*/
	if (sector_pos < DIRECT_INDEX_NUM+INDEX_PER_SECTOR) {
		struct indirect_block ib;
		cache_read(id.single_idx, INVALID_SECTOR_ID, &ib, 0, BLOCK_SECTOR_SIZE);
		return ib.sectors[sector_pos-DIRECT_INDEX_NUM];
	}

	/*sector_pos in the range of double indirect index*/
	off_t double_level_idx = (sector_pos-(DIRECT_INDEX_NUM+INDEX_PER_SECTOR))/INDEX_PER_SECTOR;
	off_t single_level_idx = (sector_pos-(DIRECT_INDEX_NUM+INDEX_PER_SECTOR))%INDEX_PER_SECTOR;
	struct indirect_block db;
	cache_read(id.double_idx, INVALID_SECTOR_ID, &db, 0, BLOCK_SECTOR_SIZE);
	struct indirect_block ib;
	cache_read(db.sectors[double_level_idx], INVALID_SECTOR_ID, &ib, 0, BLOCK_SECTOR_SIZE);
	return ib.sectors[single_level_idx];
  }
  else
    return INVALID_SECTOR_ID;
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

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;

  ASSERT (length >= 0);
  ASSERT (length <= 251*512);
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;

      int i;
      block_sector_t sector_idx = 0;
      static char zeros[BLOCK_SECTOR_SIZE];
      bool allocate_failed = false;

      //TODO: handle double indirect index
      /* allocate sectors for data and write all zeros to sectors*/
      int direct_sector_num = sectors < DIRECT_INDEX_NUM ? sectors : DIRECT_INDEX_NUM;
      int indirect_sector_num = (sectors - direct_sector_num)<INDEX_PER_SECTOR?(sectors - direct_sector_num):INDEX_PER_SECTOR;
      int double_indirect_sector_num=sectors-direct_sector_num-indirect_sector_num;

      /* allocate direct sectors */
      for (i = 0; i < direct_sector_num; i++) {
    	  	  if (free_map_allocate (1, &sector_idx)) {
    	  		  disk_inode->direct_idx[i] = sector_idx;
    	  		  cache_write(sector_idx, zeros, 0, BLOCK_SECTOR_SIZE);
    	  	  } else {
    	  		  allocate_failed = true;
    	  		  break;
    	  	  }
      }
      /* release allocated direct sectors when failed to allocate */
      if (allocate_failed) {
    	  	  int j;
    	  	  for (j = 0; j < i; j++) {
    	  		  free_map_release(disk_inode->direct_idx[j], 1);
    	  	  }
    	  	 free (disk_inode);
    	  	  return false;
      }

      /* allocate single indirect sectors */
      if(indirect_sector_num>0){
    	  	  struct indirect_block ib;
    	        if (!free_map_allocate (1, &disk_inode->single_idx)) {
    	      	  	  int j;
    	  		  for (j = 0; j < DIRECT_INDEX_NUM; j++) {
    	  			  free_map_release(disk_inode->direct_idx[j], 1);
    	  		  }
    	  		 free (disk_inode);
    	      	 return false;
    	        }

    	  	  for (i = 0; i < indirect_sector_num; i++) {
    	  		  if (free_map_allocate (1, &sector_idx)) {
    	  			  ib.sectors[i] = sector_idx;
    	  			  cache_write(sector_idx, zeros, 0, BLOCK_SECTOR_SIZE);
    	  		  } else {
    	  			  allocate_failed = true;
    	  			  break;
    	  		  }
    	  	  }
    	  	  cache_write(disk_inode->single_idx, &ib, 0, BLOCK_SECTOR_SIZE);

    	        /* release all direct sectors and allocated single indirect sectors
    	         * when failed to allocate */
    	        if (allocate_failed) {
    	  		  int j;
    	  		  for (j = 0; j < DIRECT_INDEX_NUM; j++) {
    	  			  free_map_release(disk_inode->direct_idx[j], 1);
    	  		  }

    	  		  free_map_release(disk_inode->single_idx, 1);

    	  		  for (j = 0; j < i; j++) {
    	  			  free_map_release(ib.sectors[j], 1);
    	  		  }
    	  		 free (disk_inode);
    	  		  return false;
    	        }
      }


      /*----------------------------*/

      /*----------------------------*/

      /* write inode_disk(metadata) to sector */
      cache_write(sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
      free (disk_inode);
      return true;
    }
  return false;
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
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->dir_lock);
  lock_init(&inode->inode_lock);
  //TODO: inode->is_dir = ?
  /* retrieve inode_disk from sector */
  struct inode_disk id;
  cache_read(inode->sector, INVALID_SECTOR_ID, &id, 0, BLOCK_SECTOR_SIZE);
  inode->readable_length = id.length;
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
    	  	  /* retrieve inode_disk from sector */
          struct inode_disk id;
          cache_read(inode->sector, INVALID_SECTOR_ID, &id, 0, BLOCK_SECTOR_SIZE);
          /* release metadata sector */
          //TODO: release indirect sectors
          size_t sectors = bytes_to_sectors (id.length);
          int direct_sector_num = sectors < DIRECT_INDEX_NUM ? sectors : DIRECT_INDEX_NUM;
          int indirect_sector_num = sectors - direct_sector_num;
          /* release inode_disk(metadata) sector */
          free_map_release (inode->sector, 1);
          int i;
          /* release data sectors */
          for (i = 0; i < direct_sector_num; i++) {
        	  	  free_map_release (id.direct_idx[i], 1);
          }

          struct indirect_block ib;
          if(indirect_sector_num > 0){
        	  	  cache_read(id.single_idx, INVALID_SECTOR_ID, &ib, 0, BLOCK_SECTOR_SIZE);
          }


          for (i = 0; i < indirect_sector_num; i++) {
        	  	  free_map_release (ib.sectors[i], 1);
          }

          if (indirect_sector_num > 0) {
        	  	  free_map_release (id.single_idx, 1);
          }

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

      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      //TODO: pass next_sector_id
      cache_read(sector_idx, INVALID_SECTOR_ID, buffer+bytes_read, sector_ofs, chunk_size);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
	//TODO: remove the assert after file growth implemented
	ASSERT(offset < inode->readable_length);
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cache_write(sector_idx, (void *)(buffer+bytes_written), sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

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
  return inode->readable_length;
}
