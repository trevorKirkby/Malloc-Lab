/*
mm.c

Structure of a memory block:
  long blocksize  //uses the sign bit to represent whether it is allocated or not; includes itself in the total size
  CONTENT         //if unallocated, the first 16 bits of CONTENT contains 2 pointers to the previous and next free blocks, respectively
  long blocksize  //uses the sign bit to represent whether it is allocated or not; includes itself in the total size

Red-black trees looked a bit too complex to learn up on with the time remaining. Instead I'm just using a linked list to keep track of the free memory blocks
The linked list is stored in the body of an unallocated block, so it doesn't contribute to fragmentation at all
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

#define VERBOSE 0 //For debugging purposes

/*
 * Block size and alignment macros
 */
#define ALIGNMENT 8 //Single word (4) or double word (8) alignment
#define ALIGN(size) (((size) + ((ALIGNMENT)-1)) & ~0x7) //Rounds up to the nearest multiple of ALIGNMENT
#define BLOCK_MIN (sizeof(long)*2 + sizeof(long *)*2) //The minimum size of a block, generally 24 bytes (16 for the headers and footers, and at least 16 on the inside to maintain a linked list)
#define INNER_MIN (2*sizeof(long *))  //The minimum size for the inside of a block (enough to contain linked list pointers to other free blocks)
#define MAX(a, b) (((a)>(b))?(a):(b)) //Because C doesn't have this as a builtin function for whatever reason
#define ALIGN_FLOOR(size, minimum) (MAX(ALIGN(size), (minimum)) + 2*sizeof(long)) //Computes the correct size for a block, either interior or exterior depending on whether BLOCK_MIN or INNER_MIN was passed
#define OFFSET (ALIGN(sizeof(long))-sizeof(long)) //Some unused bytes at the beginning to preserve alignment

/*
 * Macros to abstract blocks into a struct-like object
 */
#define BLOCKSIZE(block) ((*(block)) & ~(1<<31)) //I am treating the first byte of each memory block as a block object
#define INNERSIZE(block) (BLOCKSIZE(block) - 2*sizeof(long)) //The size of a block, minus the size of the header and footer
#define INNER(block) ((block) + 1)    //A pointer to the inside of the block
#define OUTER(block) ((block) - 1)    //A pointer to the outside of the block
#define IS_ALLOC(block) ((*block)>>31)         //The sign bit represents whether it is allocated or not
#define ALLOC(block) *block = (*(block)) | (1<<31); *FOOT(block) = *block   //Mark a block as allocated
#define FREE(block) *block = ((*(block)) & ~(1<<31)); *FOOT(block) = *block //Mark a block as unallocated. I could hypothetically roll these two calls into a single toggle function, but I suspect this method will make debugging easier
#define FOOT(block) (((block)+BLOCKSIZE(block)/sizeof(long))-1)  //Get the address of the blocksize footer
#define FORMAT(block, size) *block = size; *FOOT(block) = size   //Initialize a block's headers and footers
#define PREV(block) ((block)-BLOCKSIZE((block)-1)/sizeof(long))  //Gets the address of the previous block
#define NEXT(block) ((block) + (BLOCKSIZE(block)/sizeof(long)))        //Gets the address of the next block
#define LOWER ((long *)(mem_heap_lo()+OFFSET))               //Get the address of the first block
#define UPPER (PREV((long *)(mem_heap_hi()+1))) //Get the address of the last block
#define MERGE(b1, b2) *b1 = BLOCKSIZE(b1)+BLOCKSIZE(b2); *FOOT(b2) = *b1 //Merge two unallocated, consecutive blocks together

/*
 * Macros to access the interior linked-list pointers in unallocated blocks
 */
#define LL_PREV(block) ((block) + 1)   //Get a reference to the address of the previous free block
#define LL_NEXT(block) ((block) + 1 + sizeof(long *)/sizeof(long)) //Get a reference to the address of the next free block
#define PREV_FREE(block) ((long *)*LL_PREV(block)) //Get the address of the previous free block
#define NEXT_FREE(block) ((long *)*LL_NEXT(block)) //Get the address of the next free block

long *free_nodes_head; //The first free node in the linked list, where we prepend newly freed memory

/*
 * extend - a wrapper for mem_sbrk() that merges the last memory block with the new one if the last block is free
 */
void *extend(size_t size) {
  if (VERBOSE) printf("Extending by %d...\n", size);
  if (mem_heapsize()) {
    if (!IS_ALLOC(UPPER)) {
      if (VERBOSE) printf("Merging with previous unallocated block...\n");
      long *old = UPPER;
      long newsize = 2*sizeof(long)+size-BLOCKSIZE(old);
      newsize = ALIGN(newsize);
      long *new = (long *)mem_sbrk(newsize); //Blocksize includes the header and footer of the block, which we are not interested in, but it works out since by merging the block we are also removing one header and one footer
      FORMAT(new, newsize);
      MERGE(old, new);
      if (VERBOSE) printf("New block size: %d\n", BLOCKSIZE(old));
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
  //printf("Splitting into pieces of %d and %d\n", size, (total-b1_size));
  FORMAT(block, b1_size);
  long *next = NEXT(block);
  FORMAT(next, (total-b1_size));
}

/*
 * LL_delete - delete an item from the free blocks linked list
 */
void LL_delete(long *block) {
  if (PREV_FREE(block) != NULL) {
    *LL_NEXT(PREV_FREE(block)) = NEXT_FREE(block); //Delete the block we are about to allocate from our "free blocks" linked list
  }
  if (NEXT_FREE(block) != NULL) {
    *LL_PREV(NEXT_FREE(block)) = PREV_FREE(block);
  }
  if (free_nodes_head == block) {
    free_nodes_head = NEXT_FREE(block); //Will be NULL if NEXT_FREE of min is NULL, which is the desired behavior since that means we've run out of free nodes
  }
}

/*
 * mm_init - calls mem_init and returns 0 automatically because mm_init will exit() on a failure
 */
int mm_init(void)
{
  if (VERBOSE) printf("\n-----------------\n(RE)INITIALIZING\n-----------------\n\n");
  mem_init();
  mem_sbrk(OFFSET);
  free_nodes_head = NULL;
  return 0;
}

/*
 * mm_malloc - Allocate a block by consulting our linked list of free blocks
 * Always try to find the smallest free block that fits
 * If nothing works, call sbrk to allocate a new block
 */
void *mm_malloc(size_t size)
{
  if (VERBOSE) printf("Mallocating %d bytes\n", size);
  if (size == 0) { //Don't bother trying to allocate 0 bits
    return NULL;
  }
  if (free_nodes_head == NULL) {
    long bsize = ALIGN_FLOOR(size, BLOCK_MIN);
    long *newblock = (long *)mem_sbrk(bsize); //We don't have to call extend() in this case, since we already know there aren't any free blocks to merge
    FORMAT(newblock, bsize);
    ALLOC(newblock); //Mark the new block as allocated
    if (VERBOSE) printf("No free blocks, creating one of size %d at %x\n", bsize, newblock);
    return (void *)INNER(newblock); //Leave free_nodes_head as NULL since we immediately allocated our new block
  }
  long *min = NULL;
  long *block = free_nodes_head;
  while (block != NULL && block < mem_heap_hi()) { //Loop through the linked list of freed blocks until we hit one pointing to NULL
    if (INNERSIZE(block) >= size && (min == NULL || INNERSIZE(block) < INNERSIZE(min))) { //Try to find the block that fits our requirements the best
      min = block;
    }
    block = NEXT_FREE(block);
  }
  if (min == NULL) {
    if (VERBOSE) printf("No free blocks large enough, creating one of size %d\n", ALIGN_FLOOR(size, BLOCK_MIN));
    long *newblock = extend(size);
    ALLOC(newblock);
    LL_delete(newblock);
    return (void *)INNER(newblock);
  } else {
    if (VERBOSE) printf("Using free block at %x\n", min);
    LL_delete(min);
    long minimum = BLOCK_MIN;
    long splitsize = ALIGN_FLOOR(size, minimum) + 2*sizeof(long);
    long leftover = BLOCKSIZE(min) - splitsize;
    if (leftover < minimum) { //If the block we found is either the right size or close enough to the right size that the bit we would split off would be too small to form its own block, then just return that block
      ALLOC(min);
      return (void *)INNER(min);
    } else { //Otherwise, we need to split the block
      if (VERBOSE) printf("Splitting block into sizes %d and %d...\n", splitsize, leftover);
      split(min, splitsize); //Which, fortunately, is already abstracted into a function call
      ALLOC(min);
      long *newblock = NEXT(min);
      if (free_nodes_head) {
        //printf("Updating linked list...\n");
        *LL_PREV(free_nodes_head) = newblock;
        *LL_NEXT(newblock) = free_nodes_head;
        *LL_PREV(newblock) = NULL;
      } else {
        //printf("Writing NULL to *LL_NEXT(newblock)\n");
        *LL_PREV(newblock) = NULL;
        *LL_NEXT(newblock) = NULL;
      }
      free_nodes_head = newblock;
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
  if (!IS_ALLOC(block)) {
    if (VERBOSE) printf("Warning: Freeing a block that is already unallocated.\n");
    return;
  }
  if (VERBOSE) printf("Freeing %d bytes at %x\n", BLOCKSIZE(block), block);
  FREE(block); //Mark the block as unallocated
  if (!IS_ALLOC(NEXT(block)) && NEXT(block) <= UPPER) {
    if (VERBOSE) printf("Next block is free %x\n", NEXT(block));
    long *old = NEXT(block);
    MERGE(block, old);
    *LL_PREV(block) = PREV_FREE(old);
    *LL_NEXT(block) = NEXT_FREE(old); //Take the absorbed block's linked list features
    if (PREV_FREE(old) != NULL) {
      *LL_NEXT(PREV_FREE(old)) = block;
    }
    if (NEXT_FREE(old) != NULL) {
      *LL_PREV(NEXT_FREE(old)) = block; //Move any linked list references pointing to the absorbed block to our new block address
    }
    if (free_nodes_head == old) {
      free_nodes_head = block;
    }
    if (!IS_ALLOC(PREV(block)) && PREV(block) >= LOWER && block != LOWER) { //The LOWERmost block will appear as its own PREV which we want to avoid
      if (VERBOSE) printf("Previous block is also free %x\n", PREV(block));
      long *new = PREV(block);
      LL_delete(block);
      MERGE(new, block);
    }
    return; //The new block is merged in with other blocks and inherits the linked list data of the blocks it absorbed
  }
  if (!IS_ALLOC(PREV(block)) && PREV(block) >= LOWER && block != LOWER) { //Due to how PREV is calculated and how the lowest 4 bytes have zeros in them as an offset, we need to make sure we aren't dealing with the LOWER most block since it will register as its own previous block.
    if (VERBOSE) printf("Previous block is free %x\n", PREV(block));
    long *new = PREV(block);
    MERGE(new, block);
    return; //The new block is merged in with other blocks and assumes the linked list data of the blocks it absorbed
  }
  if (VERBOSE) printf("Attending to linked list...\n");
  if (free_nodes_head) {
    *LL_PREV(free_nodes_head) = block;
    *LL_NEXT(block) = free_nodes_head;
    *LL_PREV(block) = NULL;
  } else {
    *LL_PREV(block) = NULL;
    *LL_NEXT(block) = NULL;
  }
  free_nodes_head = block; //Add the block at the head of our linked list of free blocks
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *mm_realloc(void *ptr, size_t size)
{
  void *new = mm_malloc(size);
  memcpy(new, ptr, size);
  mm_free(ptr);
  return new;
}

/*
 * mm_check - Ensures that nothing wierd is happening
 * In this iteration, it just prints out diagnostic information for manual debugging
 * TODO: Make this more automated
 */
void mm_check() {
  long *block = LOWER;
  while (block < mem_heap_hi()) {
    printf("BLOCK : %x to %x : %d and %d", block, FOOT(block), BLOCKSIZE(block), BLOCKSIZE(FOOT(block)));
    if (!IS_ALLOC(block)) {
      printf(" -- prev: %x next: %x\n", *LL_PREV(block), *LL_NEXT(block));
    } else {
      printf(" -- allocated\n");
    }
    if (BLOCKSIZE(block) == 0) { //This should never happen, but if it does, it would be nice to get an actual error message instead of having the part of the program that is meant to diagnose problems be trapped in an infinite loop.
      printf("Halted mm_check to prevent infinite loop.\n");
      exit(1);
    }
    block = NEXT(block);
  }
  printf("Free nodes head: %x\n", free_nodes_head);
}
