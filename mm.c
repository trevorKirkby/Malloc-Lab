/*
mm.c

Structure of a memory block:
  long blocksize  //uses the sign bit to represent whether it is allocated or not; includes itself in the total size
  CONTENT         //if unallocated, the first 16 bits of CONTENT contains 2 pointers to the previous and next free blocks, respectively
  long blocksize  //uses the sign bit to represent whether it is allocated or not; includes itself in the total size

Red-black trees looked a bit too complex to learn up on with the time remaining. Instead I'm just using a linked list to keep track of the free memory blocks.
The linked list is stored in the body of an unallocated block, so it doesn't contribute to fragmentation beyond increasing the maximum block size from 16 to 24
*/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

team_t team = {
  "Treblajay",             //Team Name
  "Trevor Kirkby",         //Member 1 Name
  "tkirkby@westmont.edu",  //Member 1 Email
  "Jason Watts",           //Member 2 Name
  "jawatts@westmont.edu"   //Member 2 Email
};

/*
 * Block size and alignment macros
 */
#define ALIGNMENT 8 //Single word (4) or double word (8) alignment
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~0x7) //Rounds up to the nearest multiple of ALIGNMENT
#define BLOCK_MIN (sizeof(long)*2 + sizeof(long *)*2) //The minimum size of a block, generally 24 bytes (16 for the headers and footers, and at least 16 on the inside to maintain a linked list)
#define INNER_MIN (2*sizeof(long *)) //The minimum size for the inside of a block (enough to contain linked list pointers to other free blocks)
#define MAX(a, b) (((a)>(b))?(a):(b)) //Because C doesn't have this as a builtin function for whatever reason
#define ALIGN_FLOOR(size, minimum) (MAX(ALIGN(size), minimum) + 2*sizeof(long)) //Computes the correct size for a block, either interior or exterior depending on whether BLOCK_MIN or INNER_MIN was passed
#define OFFSET (ALIGN(sizeof(long))-sizeof(long)) //Some unused bytes at the beginning to preserve alignment

/*
 * Macros to abstract blocks into a struct-like object
 */
#define BLOCKSIZE(block) ((*block) & ~(1<<31)) //I am treating the first byte of each memory block as a block object
#define INNERSIZE(block) (BLOCKSIZE(block) - 2*sizeof(long)) //The size of a block, minus the size of the header and footer
#define INNER(block) (block + 1)    //A pointer to the inside of the block
#define OUTER(block) (block - 1)    //A pointer to the outside of the block
#define IS_ALLOC(block) ((*block)>>31)         //The sign bit represents whether it is allocated or not
#define ALLOC(block) *block = (*block) | (1<<31); *FOOT(block) = *block //Mark a block as allocated
#define FREE(block) *block = ((*block) & ~(1<<31)); *FOOT(block) = *block //Mark a block as unallocated. I could hypothetically roll these two calls into a single toggle function, but I suspect this method will make debugging easier
#define FOOT(block) ((block + BLOCKSIZE(block)) - sizeof(long)) //Get the address of the blocksize footer
#define FORMAT(block, size) *block = size; *FOOT(block) = size //Initialize a block's headers and footers
#define PREV(block) (block - BLOCKSIZE(block - sizeof(long))) //Gets the address of the previous block
#define NEXT(block) (block + BLOCKSIZE(block))                //Gets the address of the next block
#define LOWER ((long *)mem_heap_lo()+OFFSET)           //Get the address of the first block
#define UPPER (PREV((long *)mem_heap_hi()+1)) //Get the address of the last block
#define MERGE(b1, b2) *b1 = BLOCKSIZE(b1)+BLOCKSIZE(b2); *FOOT(b2) = *b1 //Merge two unallocated, consecutive blocks together

/*
 * Macros to access the interior linked-list pointers in unallocated blocks
 */
#define LL_PREV(block) (block + 1)   //Get a reference to the address of the previous free block
#define LL_NEXT(block) (block + 1 + sizeof(long *)/sizeof(long)) //Get a reference to the address of the next free block
#define PREV_FREE(block) ((long *)*LL_PREV(block)) //Get the address of the previous free block
#define NEXT_FREE(block) ((long *)*LL_NEXT(block)) //Get the address of the next free block

long *free_nodes_head; //The first free node in the linked list, where we prepend newly freed memory

/*
 * extend - a wrapper for mem_sbrk() that merges the last memory block with the new one if the last block is free
 */
void *extend(size_t size) {
  if (mem_heapsize()) {
    if (!IS_ALLOC(UPPER)) {
      long *old = UPPER;
      long *new = (long *)mem_sbrk(ALIGN(size-BLOCKSIZE(old))); //Blocksize includes the header and footer of the block, which we are not interested in, but it works out since by merging the block we are also removing one header and one footer
      MERGE(old, new);
      return (void *)old;
    }
  }
  long bsize = ALIGN_FLOOR(size, BLOCK_MIN);
  long *block = (long *)mem_sbrk(bsize); //We are only ever calling this if we are about to allocate the newly made memory, so there's not much point in adding it to the free_nodes linked list
  FORMAT(block, bsize);
  return (void *)block;
}

/*
 * split - split a single unallocated block into two blocks, the first of a specified size (this takes a few too many lines to fit into a macro like MERGE)
 * assumes that the block is large enough to be split into the specified size
 */
void split(long *block, size_t size) {
  long total = BLOCKSIZE(block);
  long b1_size = size;
  *block = b1_size; //Set block1's header to be the size we wanted
  *FOOT(block) = *block; //Follow the new header to where our new footer should be, and write the value of the header to the footer
  *NEXT(block) = total - b1_size; //Then follow the header again to the beginning of our new block, and create a new header there
  *FOOT(NEXT(block)) = *NEXT(block); //Finally, follow our second newly created header to where the footer of the original block should be, and update the block size
}

/*
 * mm_init - calls mem_init and returns 0 automatically because mm_init will exit() on a failure
 */
int mm_init(void)
{
  mem_init();
  mem_sbrk(OFFSET);
  return 0;
}

/*
 * mm_malloc - Allocate a block by consulting our linked list of free blocks
 * Always try to find the smallest free block that fits
 * If nothing works, call sbrk to allocate a new block
 */
void *mm_malloc(size_t size)
{
  printf("Mallocating %d bytes\n", size);
  if (size == 0) { //Don't bother trying to allocate 0 bits
    return NULL;
  }
  if (free_nodes_head == NULL) {
    long bsize = ALIGN_FLOOR(size, BLOCK_MIN);
    printf("No free nodes, creating one of size %d\n", bsize);
    long *newblock = (long *)mem_sbrk(bsize); //We don't have to call extend() in this case, since we already know there aren't any free blocks to merge
    FORMAT(newblock, bsize);
    ALLOC(newblock); //Mark the new block as allocated
    printf("Address: %x\n", newblock);
    printf("Inner: %x\n", INNER(newblock));
    printf("Header: %x\n", *newblock);
    printf("Footer: %x\n", *FOOT(newblock));
    return (void *)INNER(newblock); //Leave free_nodes_head as NULL since we immediately allocated our new block
  }
  long *min = NULL;
  long *block = free_nodes_head;
  while (NEXT_FREE(block) != NULL) { //Loop through the linked list of freed blocks until we hit one pointing to NULL
    if (INNERSIZE(block) >= size && (min == NULL || INNERSIZE(block) < INNERSIZE(min))) { //Try to find the block that fits our requirements the best
      min = block;
    }
    block = NEXT_FREE(block);
  }
  if (min == NULL) {
    printf("No free nodes that are large enough, creating one of size %d\n", ALIGN_FLOOR(size, BLOCK_MIN));
    long *newblock = extend(size);
    ALLOC(newblock);
    return (void *)INNER(newblock);
  } else {
    printf("Found a free node\n");
    //TODO: Do a deletion operation on the linked list
    *LL_NEXT(PREV_FREE(min)) = LL_NEXT(min); //TODO: Also initialize the linked list for the second half of a split block.
    *LL_PREV(NEXT_FREE(min)) = LL_PREV(min);
    if (INNERSIZE(min) - size < INNER_MIN) { //If the block we found is either the right size or close enough to the right size that the bit we would split off would be too small to form its own block, then just return that block
      printf("No split required\n");
      return (void *)min;
    } else { //Otherwise, we need to split the block
      printf("Split required\n");
      split(min, ALIGN_FLOOR(size, INNER_MIN)); //Which, fortunately, is already abstracted into a function call
      *LL_PREV(free_nodes_head) = NEXT(min);
      *LL_NEXT(NEXT(min)) = free_nodes_head;
      free_nodes_head = NEXT(min); //Add the newly split off block into the linked list.
      ALLOC(min);
      return (void *)INNER(min);
    }
  }
}

/*
 * mm_free
 */
void mm_free(void *ptr)
{
  if (ptr == NULL) { //Don't bother trying to free a null pointer
    return;
  }
  long *block = OUTER((long *)ptr);
  printf("Address: %x\n", block);
  printf("Freeing %d bytes...\n", BLOCKSIZE(block));
  printf("Setting bytes as not allocated\n");
  FREE(block); //Mark the block as unallocated
  printf("Handling merges...\n");
  if (!IS_ALLOC(NEXT(block)) && NEXT(block) <= UPPER) {
    printf("Next block is free %x\n", NEXT(block));
    long *old = NEXT(block);
    MERGE(block, NEXT(block));
    printf("Updating free linked list...\n");
    *LL_PREV(block) = PREV_FREE(old);
    *LL_NEXT(block) = NEXT_FREE(old);
    if (!IS_ALLOC(PREV(block)) && PREV(block) >= LOWER) {
      printf("Previous block is free %x\n", PREV(block));
      MERGE(PREV(block), block);
    }
    return; //The new block is merged in with other blocks and assumes the linked list data of the blocks it absorbed
  }
  if (!IS_ALLOC(PREV(block)) && PREV(block) >= LOWER) {
    printf("Previous block is free %x\n", PREV(block));
    MERGE(PREV(block), block);
    return; //The new block is merged in with other blocks and assumes the linked list data of the blocks it absorbed
  }
  printf("Attending to linked list...\n");
  if (free_nodes_head) {
    printf("Updating linked list...\n");
    *LL_PREV(free_nodes_head) = block;
    *LL_NEXT(block) = free_nodes_head;
  } else {
    printf("Writing NULL to *LL_NEXT(block)\n");
    *LL_NEXT(block) = NULL;
  }
  free_nodes_head = block; //Add the block at the head of our linked list of free blocks
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *mm_realloc(void *ptr, size_t size)
{
  mm_free(ptr);
  return mm_malloc(size);
}

/*
 * mm_check - Ensures that nothing wierd is happening
 * TODO: Implement this
 */
int mm_check() {
  return 1;
}
