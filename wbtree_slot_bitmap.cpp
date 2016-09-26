// TODO : delete all try & catch
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string.h>
#include <cassert>
// #define DEBUG 1

#ifdef DEBUG
#define PAGESIZE 128
#include <bitset>
#else
//  #define PAGESIZE 8192
//  #define PAGESIZE 4096
//  #define PAGESIZE 2048
#define PAGESIZE 1024
//  #define PAGESIZE 512
//  #define PAGESIZE 256
#endif

#define CACHE_LINE_SIZE 64

#define LEAF 1
#define INTERNAL 0

#define CPU_FREQ_MHZ (1994) //Wook : R930 MAX cpu freq
#define DELAY_IN_NS (1000)  // Wook : Pcoomit latency in ns (now 1us)

using namespace std;

inline void mfence()
{
  asm volatile("mfence":::"memory");
}

int clflush_cnt = 0;
unsigned long write_latency_in_ns=0;

static inline void cpu_pause()
{
  __asm__ volatile ("pause" ::: "memory");
}

static inline unsigned long read_tsc(void)
{
  unsigned long var;
  unsigned int hi, lo;

  asm volatile ("rdtsc" : "=a" (lo), "=d" (hi));
  var = ((unsigned long long int) hi << 32) | lo;

  return var;
}

static inline void pm_wbarrier(unsigned long lat)
{
  unsigned long etsc = read_tsc() + (unsigned long)(lat*CPU_FREQ_MHZ/1000);
  while (read_tsc() < etsc)
    cpu_pause();
}

inline void clflush(char *data, int len) {
  volatile char *ptr = (char *)((unsigned long)data &~(CACHE_LINE_SIZE-1));
  mfence();
  for (; ptr<data+len; ptr+=CACHE_LINE_SIZE) {
    unsigned long etsc = read_tsc() + (unsigned long)(write_latency_in_ns*CPU_FREQ_MHZ/1000);
    asm volatile("clflush %0" : "+m" (*(volatile char *)ptr));
    while (read_tsc() < etsc)
      cpu_pause();
    clflush_cnt++;
  }
  mfence();
}

class btree_log_header {
  private:
    uint64_t pgid;
    uint64_t txid;
    uint8_t commited;

  public:
    btree_log_header () : pgid(-1), txid(-1), commited(0) { } // pgid, txid = 0xFFFFFFFFFFFFFFFF

    btree_log_header(uint64_t _pgid, uint64_t _txid) : pgid(_pgid), txid(_txid), commited(0) { }

    btree_log_header(const btree_log_header& h)
      : pgid(h.pgid), txid(h.txid), commited(0) {
        if (h.commited == 1) {
          // because vector can double its array
          commit();
        }
      }

    void setId(uint64_t id) {
      pgid = id;
    }

    void setTxid(uint64_t id) {
      txid = id;
    }

    void commit() {
      if ( commited == 0 ){
        commited = 1;
        clflush((char*)this, sizeof(btree_log_header));
      }
    }

    void uncommit() {
      commited = 0;
    }

    uint64_t getId() {
      return pgid;
    }

    uint64_t getTxid() {
      return txid;
    }

    uint8_t isCommited() {
      return commited;
    }

    void print() {
      cout << "ID: " << pgid << endl;
      cout << "TXID: " << txid << endl;
      cout << "Commit: " << commited << endl;
    }
};

class btree_log {
  private:
    uint64_t capacity;
    uint64_t size;
    uint64_t prev_size;
    uint64_t txid;
    int last_idx;
    int8_t* log_pg;
    vector<btree_log_header> log_header;
    btree_log_header header;

  public:
    btree_log(uint64_t cap)
      : capacity(cap), size(0), prev_size(0), txid(0), last_idx(0) {
        log_pg = (int8_t*)malloc(capacity);
      }

    btree_log ()
      : capacity(0), size(0), prev_size(0), txid(0), log_pg(NULL), last_idx(0) { }

    ~btree_log() {
      delete log_pg;
    }

    void init(uint64_t cap) {
      capacity = cap;
      log_pg = (int8_t*)malloc(capacity);
    }

    void write(int8_t* ptr, uint64_t s) {
      assert ( size + s < capacity );
      header.setId((uint64_t)ptr);
      header.setTxid(txid);
      log_header.push_back(header);
      memcpy(log_pg + size, ptr, s);
      size += s;
    }

    void commit() {
      //cout << "Commit!: " << size << "\t" << capacity * 0.7 << endl;
      if (size > capacity * 0.7) {
        capacity = 1.5 * capacity;
        int8_t* new_pg = (int8_t*)malloc(capacity);
        memcpy(new_pg, log_pg, size);
        delete log_pg;
        clflush((char*) new_pg, size);
        log_pg = new_pg;
      }
      clflush((char*) log_pg + prev_size, size-prev_size);
      for (int i = last_idx; i < log_header.size(); ++i) {
        log_header[i].commit();
      }
      prev_size = size;
      last_idx = log_header.size();
      txid++;
    }

    bool isCommited() {
      return size == prev_size;
    }
};

class page;

class three_pages {
 public:
   page* l;  // left neighbor
   page* p;  // current node
   page* r;  // right neighbor
   three_pages() {
     l = NULL;
     p = NULL;
     r = NULL;
   }
   three_pages(page* left_neighbor, page* cur, page* right_neighbor) {
     l = left_neighbor;
     p = cur;
     r = right_neighbor;
   }
   friend class page;
};

class split_info{
  public:
    page* left;
    int64_t split_key;
    page* right;
    split_info(page* l, int64_t k, page* r){
      left = l;
      split_key = k;
      right = r;
    }
    friend class page;
};

class merge_info {
 public:
   page* leftPtr;
   int64_t mergeKey;
   page* rightPtr;
   merge_info(page* l, int64_t k, page* r) {
     leftPtr = l;
     mergeKey = k;
     rightPtr = r;
   }
   friend class page;
};


class header{ // 27 bytes
  private:
    uint8_t cnt; // 1 bytes
    uint8_t flag; // 1 byte, leaf or internal
    uint8_t fragmented_bytes; // 1 byte
    page* leftmost_ptr; // 8 bytes
    int64_t split_key; // 8 bytes
    page* sibling_ptr; // 8 bytes
    friend class page;
    friend class btree;
};

class entry{
  private:
    //int16_t len;  // need this for variable-length record and defragmentation.
    int64_t key; // 8 bytes
    char* ptr;   // 8 bytes

    friend class page;
    friend class btree;
};

const int cardinality = (PAGESIZE-sizeof(header))/ (sizeof(entry)+2);
// const int cardinality = 3;
const uint64_t error = 1UL << 57;

class page{
  private:
    header hdr;  // header in persistent memory
    entry records[cardinality]; // slots in persistent memory

    uint64_t bitmap; // 0 or 1 for validation, rest for entries
    uint8_t slot_array[cardinality];

  public:
    page(){}

    page(short f)
    {
      bitmap=1;
      hdr.cnt=0;
      hdr.flag=f;
      hdr.fragmented_bytes=0;
      hdr.leftmost_ptr = NULL;

      hdr.split_key = 0;
      hdr.sibling_ptr = NULL;

    }

    page(page* left, int64_t key, page* right) // this is called when tree grows
    {
      bitmap=1;
      hdr.cnt=0;
      hdr.flag=INTERNAL;
      hdr.fragmented_bytes=0;
      hdr.leftmost_ptr = left;
      hdr.split_key = 0;
      hdr.sibling_ptr = NULL;

      store((char*) left, key, (char*) right, 1);
    }

    page(page* left, int64_t key, page* right, int flush) // this is called when tree grows
    {
      bitmap=1;
      hdr.cnt=0;
      hdr.flag=INTERNAL;
      hdr.fragmented_bytes=0;
      hdr.leftmost_ptr = left;
      hdr.split_key = 0;
      hdr.sibling_ptr = NULL;

      store((char*) left, key, (char*) right, flush);
    }

    merge_info* merge(page* left, page* right,
        char* leftPtr, int64_t key, char* rightPtr, int64_t splitKey) {
      page* tmp = new page(hdr.flag);
      tmp->copyBulk(left, right, leftPtr, key, rightPtr, splitKey);
      clflush((char*)tmp, sizeof(page));
      merge_info* m = new merge_info(tmp, splitKey, NULL);
      return m;
    }

    inline merge_info* delete_entry(page* l, int64_t key, page* r,
        int64_t leftSplitKey, int64_t rightSplitKey, int pos) {
      if (linear_search(key, pos)) {
        int64_t left_split_key = (pos < 0)?leftSplitKey:getKey(pos);
        int64_t right_split_key = (pos == hdr.cnt - 1)?getKey(pos):rightSplitKey;
        return delete_entry(l, key, r, leftPtr, rightPtr, left_split_key,
            right_split_key, pos);
      }
      return NULL;
    }

    merge_info* delete_entry(page* l, int64_t key, page* r,
        char* leftPtr, char* rightPtr,
        int64_t leftSplitKey, int64_t rightSplitKey, int pos) {
      if (!is_sufficient()) {
        if ((l && !l->is_sufficient(hdr.cnt)) &&
            (r && !r->is_sufficient(hdr.cnt))) {
          // merge
          if (l && r) {
            if (l->hdr.cnt < r->hdr.cnt) {
              // last parameter is true if merge to left
              return merge(l, this, leftPtr, key, rightPtr, leftSplitKey);
            } else {
              return merge(this, r, leftPtr, key, rightPtr, rightSplitKey);
            }
          } else {
            if (l) {
              return merge(l, this, leftPtr, key, rightPtr, leftSplitKey);
            } else {
              return merge(this, r, leftPtr, key, rightPtr, rightSplitKey);
            }
          }
        } else {
          // shift
          // TODO: implement shift
        }
      } else {
        if (hdr.flag == LEAF && pos == 0) {
          // key update
          release(pos, 1);
          // merge_info* m = new merge_info(l, leftSplitKey, r);
          // return m;
          return NULL;
        } else {
          // just delete
          release(pos, 1);
          return NULL;
        }
      }
    }

    char avail_offsets[cardinality];

    inline uint8_t nextSlotOff()
    {
      for(int i=0;i<cardinality;i++){
        avail_offsets[i] = 1;
      }

      for(int i=0;i<hdr.cnt;i++){
        avail_offsets[slot_array[i]] = 0;
      }

      for(int i=0;i<cardinality;i++){
        if(avail_offsets[i] == 1) {
          //printf("available offset=%d\n", i);
          return i;
        }
      }

      printf("not enough space: This should not happen\n");
    }

    int copyBulk(page* left, page* right, char* leftPtr, int64_t key,
                 char* rightPtr, int64_t splitKey) {
      // TODO: not to use store()
      int deleted = 0;
      int pos = 0;
      int beginLeft = 0;
      int beginRight = 0;
      int leaf = hdr.flag;

      if (leaf) {
        for (int i = 0; i < left->hdr.cnt; ++i) {
          if (!deleted && key == left->getKey(i)) {
            deleted = 1;
            continue;
          } else {
            records[pos++] = left->records[slot_array[i]];
          }
        }
        for (int i = 0; i < right->hdr.cnt; ++i) {
          if (!deleted && key == right->getKey(i)) {
            deleted = 1;
            continue;
          } else {
            records[pos++] = right->records[slot_array[i]];
          }
        }
      } else {
        // deal with leftmost ptr
        if (!hdr.leftmost_ptr && left->hdr.leftmost_ptr) {
          if (left->hdr.cnt > 0 && key == left->getKey(0)) {
            if (!leftPtr) {
              hdr.leftmost_ptr = (page*)rightPtr;
            } else {
              hdr.leftmost_ptr = (page*)leftPtr;
            }
            beginLeft = 1;
            deleted = 1;
          } else {
            hdr.leftmost_ptr = left->hdr.leftmost_ptr;
          }
        }
        for (int i = beginLeft; i < left->hdr.cnt; ++i) {
          if (!deleted && key == left->getKey(i)) {
            if (!leftPtr) {
              records[pos - 1].ptr = rightPtr;
            } else {
              records[pos - 1].ptr = leftPtr;
            }
            deleted = 1;
          } else {
            records[pos++] = left->records[slot_array[i]];
          }
        }
        if (right->hdr.leftmost_ptr) {
          records[pos].key = splitKey;
          if (!deleted && right->hdr.cnt > 0 && key == right->getKey(0)) {
            if (!leftPtr) {
              records[pos++].ptr = rightPtr;
            } else {
              records[pos++].ptr = leftPtr;
            }
            beginRight = 1;
            deleted = 1;
          } else {
            records[pos++].ptr = (char*)right->hdr.leftmost_ptr;
          }
        }
        for (int i = beginRight; i < right->hdr.cnt; ++i) {
          if (!deleted && key == right->getKey(i)) {
            if (!leftPtr) {
              records[pos - 1].ptr = rightPtr;
            } else {
              records[pos - 1].ptr = leftPtr;
            }
            deleted = 1;
          } else {
            records[pos++] = right->records[slot_array[i]];
          }
        }
      }
      hdr.cnt = pos;
      // TODO: bitmap!!!
      return hdr.cnt;
    }


    // void storeBulk(page* src, const int &begin, const int &end, page* parent) {
    //   int pos = begin;
    //   if (src->hdr.flag == LEAF && begin == -1) {
    //     ++pos;
    //   }
    //   for (int i = pos; i < end; ++i) {
    //     if (i == -1) {
    //       int tmp_pos;
    //       parent->linear_search(src->getAnyKey(), tmp_pos);
    //       int64_t tmpKey = parent->getKey(tmp_pos);
    //       store(NULL, tmpKey, src->hdr.leftmost_ptr, 0);
    //     } else {
    //       store(NULL, src->getKey(i), src->getPtr(i), 0);
    //     }
    //   }
    // }

    inline uint8_t nextSlotOff2()
    {
      uint64_t bit = 2;
      uint8_t i=0;
      //      bitset<64> x(bitmap);
      //      cout << hdr.cnt <<"/"<< cardinality<< ": " << x << endl;
      while ( bit != 0 && i < cardinality) {
        if ( (bitmap & bit) == 0 ) {
          return i;
        }
        bit = bit << 1;
        ++i;
      }

#ifdef DEBUG
      bitset<64> bm (bitmap);
      cout << hdr.cnt << endl;
      cout << bm << endl;
#endif
      cout << "no space: " << bitmap << endl;
      assert(!"not enough space: This should not happen\n");
    }


    inline int64_t getKey(int i)
    {
      return (records[slot_array[i]]).key;
    }

    int get_entry_pos(page *target_entry) {
      if (target_entry == hdr.leftmost_ptr) return -1;
      for (int i = 0; i < hdr.cnt; ++i) {
        if (target_entry == (page*)records[slot_array[i]].ptr) {
          return i;
        }
      }
#ifdef DEBUG
      cout << "get_entry_pos: not found!!!, debug" << endl;
      exit(1);
#endif
      return hdr.cnt;
    }


    inline int64_t getAnyKey() {
      if (hdr.cnt > 0) {
        return records[slot_array[0]].key;
      } else if (hdr.flag != LEAF) {
        return hdr.leftmost_ptr->getAnyKey();
      } else {
        cout << "getAnyKey() : no key !!!!!!!!!!!!!!!, debug" << endl;
      }
    }

    inline int64_t getFirstKey() {
      return records[slot_array[0]].key;
    }

    inline int64_t getLastKey() {
      return records[slot_array[hdr.cnt - 1]].key;
    }

    inline char* getPtr(int i)
    {
      if (i == -1) {
        if (hdr.flag != LEAF) {
          return (char*)hdr.leftmost_ptr;
        } else {
          return NULL;
        }
      } else {
        return (records[slot_array[i]]).ptr;
      }
    }

    // inline int64_t

    inline page* getLeftPtr(int i) {
      if (i > 0) {
        return (page*)records[slot_array[i - 1]].ptr;
      } else if (hdr.flag != LEAF && i == 0) {
        return hdr.leftmost_ptr;
      } else {
        return NULL;
      }
    }

    inline page* getLeftPtr(int i, page *left_parent) {
      if (i == -1 && left_parent != NULL) {
        return left_parent->getLastPtr();
      } else {
        return getLeftPtr(i);
      }
    }

    inline page* getRightPtr(int i) {
      if (i < hdr.cnt - 1) {
        return (page*)records[slot_array[i + 1]].ptr;
      } else {
        return NULL;
      }
    }

    inline page* getRightPtr(int i, page *right_parent) {
      if (i == hdr.cnt && right_parent != NULL) {
        return right_parent->getLeftMostPtr();
      } else {
        return getRightPtr(i);
      }
    }

    inline page* getLeftMostPtr() {
      return hdr.leftmost_ptr;
    }

    inline page* getLastPtr() {
      return (page*)records[slot_array[hdr.cnt - 1]].ptr;
    }

    inline entry* getLeftMostEntry() {
      entry *tmp_entry = new entry();
      tmp_entry->key = (hdr.leftmost_ptr)->getKey(0);
      tmp_entry->ptr = (char*)hdr.leftmost_ptr;
      return tmp_entry;
    }

    inline entry* getFirstEntry() {
      return (entry*)&records[slot_array[0]];
    }

    inline entry* getLastEntry() {
      return (entry*)&records[slot_array[hdr.cnt - 1]];
    }

    inline entry* getEntry(int i)
    {
      return (entry*) &records[slot_array[i]];
    }

    split_info* store(int64_t key, char* ptr, int flush )
    {
      return store(NULL, key, ptr, flush );
    }

    split_info* store(entry *target_entry, int flush) {
      return store(NULL, target_entry->key, target_entry->ptr, flush);
    }

    split_info* store(char* left, int64_t key, char* right, int flush )
    {
#ifdef DEBUG
      printf("\n-----------------------\nBEFORE STORE %d\n", key);
      print();
#endif

      if ( (bitmap & 1) == 0 ) {
        // TODO: recovery
        cerr << "bitmap error: recovery is required." << endl;
        bitmap += 1;
        if(flush)
          clflush((char*) &bitmap, 8);
      }
      if( hdr.cnt < cardinality ){
        // have space
        register uint8_t slot_off = (uint8_t) nextSlotOff2();
        bitmap -= 1;
        if(flush)
          clflush((char*) &bitmap, 8);

        if(hdr.cnt==0){  // this page is empty
          entry* new_entry = (entry*) &records[slot_off];
          new_entry->key = (int64_t) key;
          new_entry->ptr = (char*) right;
          if(left!=NULL) {
            hdr.leftmost_ptr = (page*) left;
          }

          if(flush)
            clflush((char*) new_entry, sizeof(entry));

          slot_array[0] = slot_off;
          if(flush)
            clflush((char*) slot_array, sizeof(uint8_t));

          uint64_t bit = (1UL << (slot_off+1));
          bitmap |= bit;
          bitmap+=1;

          if(flush)
            clflush((char*) &bitmap, 8);

          hdr.cnt++;
          assert(bitmap < error);
        }
        else{
          int pos=0;
          for(pos=0;pos<hdr.cnt;pos++){
            if(key < records[slot_array[pos]].key ){
              break;
            }
          }

          if(left!=NULL){
            // this is needed for CoW
            if(pos>0)
              records[slot_array[pos-1]].ptr = (char*) left;
            else
              hdr.leftmost_ptr = (page*) left;
          }
          records[slot_off].key = (int64_t) key;
          records[slot_off].ptr = (char*) right;
          if(flush)
            clflush((char*) &records[slot_off], sizeof(entry));

          for(int i=hdr.cnt-1; i>=pos; i--){
            slot_array[i+1] = slot_array[i];
          }
          slot_array[pos] = slot_off;

          if(flush)
            clflush((char*) slot_array, sizeof(uint8_t)*hdr.cnt);

          uint64_t bit = (1UL << (slot_off+1));
          bitmap |= bit;
          bitmap+=1;
          //          bitset<64> bm(bitmap);
          //          cout << hdr.cnt+1 << ":\t" << bm << endl;

          if(flush)
            clflush((char*) &bitmap, 8);
          hdr.cnt++;
          assert (bitmap < error);
        }

#ifdef DEBUG
        printf("--------------:\n");
        printf("AFTER STORE:\n");
        print();
        printf("---------------------------------\n");
#endif
      }
      else {
        // overflow
        // TBD: defragmentation not yet implemented.
#ifdef DEBUG
        printf("====OVERFLOW OVERFLOW OVERFLOW====\n");
        print();
#endif
        //page* lsibling = new page(hdr.flag);
        //        bitset<64> map(bitmap);
        //        cout << hdr.cnt << ":\t" << map << endl;
        page* rsibling = new page(hdr.flag);
        register int m = (int) ceil(hdr.cnt/2);
        uint64_t bitmap_change = 0;
        //int n = 0;

        bitmap -= 1;
        if(flush)
          clflush((char*) &bitmap, 8);

        // TODO: redo logging?
        // Maybe I can...
        //split_info s((page*)this, (uint64_t) records[slot_array[m]].key, (page*) rsibling);
        split_info *s = new split_info((page*)this, (uint64_t) records[slot_array[m]].key, (page*) rsibling);

        if(hdr.flag==LEAF){
          //migrate i > hdr.cnt/2 to a rsibling;
          for(int i=m;i<hdr.cnt;i++){
            rsibling->store(records[slot_array[i]].key, records[slot_array[i]].ptr, 0);
            uint64_t bit = (1UL << (slot_array[i]+1));
            bitmap_change += bit;
            //            n++;
          }
        }
        else{
          //migrate i > hdr.cnt/2 to a rsibling;
          rsibling->hdr.leftmost_ptr = (page*) records[slot_array[m]].ptr;
          uint64_t bit = (1UL << (slot_array[m]+1));
          bitmap_change += bit;
          for(int i=m+1;i<hdr.cnt;i++){
            rsibling->store(records[slot_array[i]].key, records[slot_array[i]].ptr, 0);
            uint64_t bit = (1UL << (slot_array[i]+1));
            bitmap_change += bit;
            //            n++;
          }
        }
        assert ((bitmap & bitmap_change) == bitmap_change);

        bitmap -= bitmap_change;
        bitmap += 1;
        hdr.cnt = m;
        hdr.sibling_ptr = rsibling;

        if(flush){
          clflush((char*) rsibling, sizeof(page)-sizeof(entry)*(cardinality-m));
          clflush((char*) this, sizeof(page));
        }
        // split is done.. now let's insert a new entry

        if(key < s->split_key){
          store(left, key, right, 1);
        }
        else {
          rsibling->store(left, key, right, 1);
        }
#ifdef DEBUG
        printf("Split done\n");
        printf("Split key=%lld\n", s->split_key);
        printf("LEFT\n");
        print();
        printf("RIGHT\n");
        rsibling->print();
#endif
        return s;
      }
      return NULL;
    }

    // void replace(char* leftPtr, int64_t key, char* rightPtr, int flush) {
    //   int pos = hdr.cnt;
    //   char* &leftChild;
    //   char* &rightChild;
    //   if (hdr.cnt < 2) {
    //     pos = 0;
    //     leftChild = hdr.leftmost_ptr;
    //     rightChild = getPtr(0);
    //   } else if (key < getKey(1)) {
    //     pos = 0;
    //     leftChild = hdr.leftmost_ptr;
    //     rightChild = getPtr(0);
    //   } else if (getKey(hdr.cnt - 2) < key) {
    //     pos = hdr.cnt - 1;
    //     leftChild = getPtr(hdr.cnt - 2);
    //     rightChild = getPtr(pos);
    //   } else {
    //     for (int i = 1; i < hdr.cnt - 1; ++i) {
    //       if (getKey(i - 1) < key && key < getKey(i + 1)) {
    //         pos = i;
    //         leftChild = getPtr(i - 1);
    //         rightChild = getPtr(i + 1);
    //         break;
    //       }
    //     }
    //   }

    //   if (pos == hdr.cnt) return;

    //   if (bitmap & 1UL != 1UL) {
    //     // TODO: recovery
    //     bitmap |= 1UL;
    //     if (flush) {
    //       clflush((char*)&bitmap, sizeof(uint64_t));
    //     }
    //   }
    //   // invalidate the page.
    //   bitmap &= 0xFFFFFFFFFFFFFFFE;
    //   if (flush) {
    //     clflush((char*)&bitmap, sizeof(uint64_t));
    //   }
    //   leftChild = leftPtr;
    //   rightChild = rightPtr;
    //   if (flush) {
    //     clflush((char*)this, sizeof(page));
    //   }
    //   // validate the page
    //   bitmap |= 1UL;
    //   if (flush) {
    //     clflush((char*)&bitmap, sizeof(uint64_t));
    //   }
    // }

    int update_key(int64_t old_key, int64_t new_key, int flush) {
      for (int i = 0; i < hdr.cnt; ++i) {
        if (records[slot_array[i]].key == old_key) {
          update_key(i, new_key, 1);
          return i;
        }
      }
      return -1;
    }

    void update_key(int pos, int64_t new_key, int flush) {
      if (pos >= 0 && pos < hdr.cnt) {
        if (bitmap & 1UL != 1UL) {
          // TODO: recovery
          bitmap |= 1UL;
          if (flush) {
            clflush((char*)&bitmap, sizeof(uint64_t));
          }
        }
        // invalidate the page.
        bitmap &= 0xFFFFFFFFFFFFFFFE;
        if (flush) {
          clflush((char*)&bitmap, sizeof(uint64_t));
        }
        // update the key.
        entry *tmp_entry = &records[slot_array[pos]];
        tmp_entry->key = new_key;
        if (flush) {
          clflush((char*)&tmp_entry->key, sizeof(int64_t));
        }
        // validate the page
        bitmap |= 1UL;
        if (flush) {
          clflush((char*)&bitmap, sizeof(uint64_t));
        }
      } else {
#ifdef DEBUG
        cout << "update: Not found ***, " <<
                ((hdr.flag == LEAF)?"LEAF":"INTERNAL") << endl;
        print();
        exit(1);
#endif
      }
    }

    int release(page *target, const int &flush) {
      if (hdr.leftmost_ptr == target) {
        release(-1, flush);
        return -1;
      } else {
        for (int i = 0; i < hdr.cnt; ++i) {
          if ((page*)getPtr(i) == target) {
            release(i, flush);
            return i;
          }
        }
#ifdef DEBUG
        cout << "Target not found!!!, debug" << endl;
        exit(1);
#endif
        return hdr.cnt;
      }
    }

    int release(const int64_t &key, const int &flush) {
      int pos = hdr.cnt;
      for (pos = 0; pos < hdr.cnt; ++pos) {
        if (records[slot_array[pos]].key == key) break;
      }
      release(pos, flush);
      return pos;
    }

    void release(const int &pos, const int &flush) {
      if (pos == -1) {
        page *new_lm = (page*)getPtr(0);
        release(0, 1);
        hdr.leftmost_ptr = new_lm;
      } else if (pos >= 0 && pos < hdr.cnt) {
        // calculate the entry bit.
        uint64_t bit = (1UL << (slot_array[pos] + 1));
        if (bitmap & 1UL != 1UL) {
          // TODO: recovery
          bitmap |= 1UL;
          if (flush) {
            clflush((char*)&bitmap, sizeof(uint64_t));
          }
        }
        // invalidate the page.
        bitmap &= 0xFFFFFFFFFFFFFFFE;
        if (flush) {
          clflush((char*)&bitmap, sizeof(uint64_t));
        }
        // update slot array
        for (int i = pos; i < hdr.cnt - 1; ++i) {
          slot_array[i] = slot_array[i + 1];
        }
        if (flush) {
          clflush((char*)slot_array, sizeof(uint8_t) * (hdr.cnt - 1));
        }
        // update header count
        --hdr.cnt;
        // update the bitmap.
        bitmap ^= bit;
        // validate the page
        bitmap |= 1UL;
        if (flush) {
          clflush((char*)&bitmap, sizeof(uint64_t));
        }
      } else {
        // invalid pos
#ifdef DEBUG
        cout << "release: Not found ***, " <<
                ((hdr.flag == LEAF)?"LEAF":"INTERNAL") << endl;
        print();
        exit(1);
#endif
      }
    }

    // page::binary_search
    char* binary_search(long long key)
    {
      if(hdr.flag==LEAF){
        register short lb=0, ub=hdr.cnt-1, m;
        while(lb<=ub){
          m = (lb+ub)/2;
          register uint64_t k = records[slot_array[m]].key;
          if( key < k){
            ub = m-1;
          }
          else if( key == k){
            return records[slot_array[m]].ptr;
          }
          else {
            lb = m+1;
          }

        }
        return NULL;
      }
      else {
        register short lb=0, ub=hdr.cnt-1, m;
        while(lb<=ub){
          m = (lb+ub)/2;
          if( key < records[slot_array[m]].key){
            ub = m-1;
          }
          else {
            lb = m+1;
          }
        }
        if(ub>=0) {
          return records[slot_array[ub]].ptr;
        }
        else
          return (char *)hdr.leftmost_ptr;

        return NULL;
      }
    }

    char* linear_search(long long key)
    {
      if(hdr.flag==LEAF){
        for(int i=0;i<hdr.cnt;i++){
          if(records[slot_array[i]].key == key){
            //printf("found: %lld\n", key);
            return records[slot_array[i]].ptr;
          }
        }
        printf("Not found************************************\n");
#ifdef DEBUG
        print();
        exit(1);
#endif
      }
      else {
        if(key < getKey(0)){
          return (char*) hdr.leftmost_ptr;
        }
        for(int i=1;i<hdr.cnt;i++){
          if(key < records[slot_array[i]].key){
            // visit child i
            return records[slot_array[i-1]].ptr;
          }
        }
        // visit rightmost pointer
        return getPtr(hdr.cnt-1); // return rightmost ptr
      }
    }

    char* linear_search(int64_t key, int &pos) {
      if (hdr.flag == LEAF) {
        pos = hdr.cnt;
        for(pos = 0; pos < hdr.cnt; ++pos) {
          if (records[slot_array[pos]].key == key) {
            //printf("found: %lld\n", key);
            return records[slot_array[pos]].ptr;
          }
        }
        return NULL;
#ifdef DEBUG
        printf("Not found************************************\n");
        print();
        exit(1);
#endif
      } else {
        pos = -1;
        if (key < getKey(0)){
          return (char*) hdr.leftmost_ptr;
        }
        for(int i = 1; i < hdr.cnt; ++i) {
          if (key < records[slot_array[i]].key) {
            // visit child i
            pos = i - 1;
            return records[slot_array[i - 1]].ptr;
          }
        }
        // visit rightmost pointer
        pos = hdr.cnt - 1;
        return getPtr(hdr.cnt - 1); // return rightmost ptr
      }
    }

    bool is_sufficient() {
      return (hdr.cnt > 1);
      // return (hdr.cnt > cardinality / 2);
    }

    void print()
    {
      if(hdr.flag==LEAF) printf("leaf\n");
      else printf("internal\n");
      printf("hdr.cnt=%d:\n", hdr.cnt);

      //        printf("slot_array:\n");
      //	  for(int i=0;i<hdr.cnt;i++){
      //              printf("%d ", (uint8_t) slot_array[i]);
      //	  }
      //	  printf("\n");

      for(int i=0;i<hdr.cnt;i++){
        printf("%ld ", getEntry(i)->key);
      }
      printf("\n");

    }

    void printAll()
    {
      if(hdr.flag==LEAF){
        printf("printing leaf node: ");
        print();
      }
      else{
        printf("printing internal node: ");
        print();

        ((page*) hdr.leftmost_ptr)->printAll();
        for(int i=0;i<hdr.cnt;i++){
          ((page*) getEntry(i)->ptr)->printAll();
        }
      }
    }
    friend class btree;
};


class btree{
  private:
    int height;
    page* root;
    page *udf;
    btree_log log;

  public:
    btree(){
      root = new page(LEAF);
      height = 1;
      log.init(sizeof(page)*1024);
    }

    // binary search
    void btree_binary_search(long long key){
      page* p = root;
      while(p){
        if(p->hdr.flag==LEAF){
          // found a leaf
          long long d = (long long) p->binary_search(key);
          if(d==0) {
            printf("************* Not Found %lld\n", key);
            p->print();
            exit(1);
          }
          else {
            //printf("Found %lld\n", key);
          }
          return;
        }
        else{
          p = (page*) p->binary_search(key);
        }
      }
      printf("***** Not Found %lld\n", key);
      return;
    }

    void btree_search(long long key){
      page* p = root;
      while(p){
        if(p->hdr.flag!=LEAF){
          p = (page*) p->linear_search(key);
        }
        else{
          // found a leaf
          p->linear_search(key);
          break;
        }
      }
    }

    void btree_insert(long long key, char* right){
      page* p = root;
      //vector<page*> path;
      //vector<page*> path(height+1);
      page* path[height+1];
      //path.push_back(p);
      int top = 0;
      path[top++] = p;
      while(p){
        if(p->hdr.flag!=LEAF){
          p = (page*) p->linear_search(key);
          //p = (page*) p->binary_search(key);
          //path.push_back(p);
          path[top++] = p;
        }
        else{
          // found a leaf p
          break;
        }
      }

      if(p==NULL)
        printf("something wrong.. p is null\n");

      char *left = NULL;
      do{
        assert(right!=NULL);
        split_info *s = p->store(left, key, right, 1); // store
        p = NULL;
        if(s!=NULL){
          // split occurred
          // logging needed
          //          page *logPage = new page[2];
          //          memcpy(logPage, s->left, sizeof(page));
          //          memcpy(logPage+1, s->right, sizeof(page));
          //          clflush((char*) logPage, 2*sizeof(page));
          log.write((int8_t*)s->left, sizeof(page));
          // we need log frame header, but let's just skip it for now... need to fix it for later..

          page* overflown = p;
          //p = (page*) path.back();
          //path.pop_back();
          p = (page*) path[--top];
          //if(path.empty())
          if ( top == 0 ) {
            // tree height grows here
            page* new_root = new page(s->left, s->split_key, s->right);
            root = new_root;
            height++;
#ifdef DEBUG
            printf("tree grows: root = %x\n", root);
            root->print();
#endif
            //delete overflown;

            delete s;
            break;
          }
          else{
            // this part needs logging
            //p = (page*) path.back();
            p = (page*) path[top-1];
            left = (char*) s->left;
            key = s->split_key;
            right = (char*) s->right;
            assert(right!=NULL);

            //delete overflown;
          }

          delete s;
        }
      } while(p!=NULL);
      if (!log.isCommited()) {
        log.commit();
      }
    }


   void btree_delete(long long key) {
     page* p = root;
     page* l = NULL;
     page* r = NULL;
     int pos;
     three_pages* path[height + 1];
     int path_pos[height + 1];
     int top = 0;
     path[top++] = new three_pages(NULL, p, NULL);
     while (p) {
       if (p->hdr.flag != LEAF) {
         page* child = (page*)p->linear_search(key, pos);
         path_pos[top] = pos;
         path[top++] = new three_pages(p->getLeftPtr(pos), child,
                                       p->getRightPtr(pos));
         p = child;
       } else {
         break;
       }
     }

     if (p == NULL) {
       cout << "key not found. exiting..." << endl;
       exit(1);
     }

     do {
       merge_info* m = p->delete_entry(l, key, r, leftSplitKey, rightSplitKey,
           pos);
       p = NULL;
       if (m != NULL) {
         p = path[--top]->p;
         l = path[top]->l;
         r = path[top]->r;
         pos = path_pos[top];
         if (top == 0) {
         } else {
           p = path[top - 1]->p;
           l = path[top - 1]->l;
           r = path[top - 1]->r;
         }
         delete m;
       }
     } while (p != NULL);

    // page* shiftEntries(page* left, page* right, page* parent,
    //                    const int &numShift, const int &fromLeft) {
    //   page* sLeft;
    //   page* sRight;
    //   int64_t newKey;
    //   if (fromLeft) {
    //     // Shift from left to right
    //     sLeft = new page(left->leftmost_ptr, left->getKey(0),
    //                      left->getPtr(0), 0);
    //     int basePos = left->hdr.cnt - numShift;
    //     int pos = basePos;
    //     if (hdr.flag != LEAF) {
    //       if (numShift > 2) {
    //         newKey = left->getKey(basePos);
    //         sRight = new page(left->getPtr(basePos),
    //           left->getKey(basePos + 1), left->getPtr(basePos + 1), 0);
    //         pos += 2;
    //       } else {
    //         cout << "NO!!!!!!!!!!!";
    //         exit(1);
    //       }
    //     } else {
    //       // LEAF
    //       sRight = new page(NULL, left->getKey(basePos),
    //                         left->getPtr(basePos), 0);
    //       ++pos;
    //     }
    //     sLeft->storeBulk(left, 1, basePos, parent);
    //     sLeft->hdr.sibling_ptr = sRight;
    //     clflush((char*)sLeft, sizeof(page));

    //     sRight->storeBulk(left, pos, left->hdr.cnt, parent);
    //     sRight->storeBulk(right, -1, right->hdr.cnt, parent);
    //     clflush((char*)sRight, sizeof(page));

    //     parent->store(sLeft, newKey, sRight, 1);
    //   } else {
    //     // Shift from right to left
    //     sLeft = new page(left->leftmost_ptr, left->getKey(0),
    //                      left->getPtr(0), 0);
    //     int basePos = (hdr.flag != LEAF)?numShift - 1:numShift;
    //     itn pos = basePos;
    //     if (hdr.flag != LEAF) {
    //       if (basePos + 1 < right->hdr.cnt) {
    //         newKey = right->getKey(basePos);
    //         sRight = new page(right->getPtr(basePos),
    //             right->getKey(basePos + 1), right->getPtr(basePos + 1), 0);
    //         pos += 2;
    //       } else {
    //         cout << "NO!!!!!!!!!!!!!!!!!";
    //       }
    //     } else {
    //       sRight = new page(NULL, right->getKey(basePos),
    //           right->getPtr(basePos), 0);
    //       ++pos;
    //     }
    //     sLeft->storeBulk(left, 1, left->hdr.cnt, parent);
    //     sLeft->storeBulk(right, -1, basePos, parent);
    //     sLeft->hdr.sibling_ptr = sRight;
    //     clflush((char*)sLeft, sizeof(page));

    //     sRight->storeBulk(right, pos, right->hdr.cnt, parent);
    //     clflush((char*)sRight, sizeof(page));

    //     parent->store(sLeft, newKey, sRight, 1);
    //   }
    //   sLeft->hdr.sibling_ptr = NULL;
    //   clflush((char*)sLeft->hdr, sizeof(header));
    // }

    int64_t btree_check(page *p, bool &is_valid, int64_t &max) {
      int64_t m = 0;
      if (p->hdr.flag == LEAF) {
        max = p->getKey(p->hdr.cnt-1);
        return p->getKey(0);
      }
      for (int i = 0; i < p->hdr.cnt; ++i) {
        int64_t ret = btree_check((page*)p->getPtr(i), is_valid, m);
        if (!is_valid) return 0;
        if (ret != p->getKey(i)) {
          is_valid = false;
          return 0;
        }
      }
      max = m;
      int64_t min = btree_check(p->hdr.leftmost_ptr, is_valid, m);
      if (!is_valid) return 0;
      if (p->hdr.cnt > 0 && !(m < p->getKey(0))) {
        is_valid = false;
        return 0;
      }
      return min;
    }

    bool btree_ck() {
      bool ret = true;
      int64_t m = 0;
      btree_check(root, ret, m);
      return ret;
    }

    void printAll(){
      root->printAll();
    }

    int get_height() {
      return height;
    }
};

int main(int argc,char** argv)
{
  int dummy=0;
  btree bt;
  struct timespec start, end;

  //    printf("sizeof(page)=%lu\n", sizeof(page));

  if(argc<3) {
    printf("Usage: %s NUMDATA WRITE_LATENCY\n", argv[0]);
  }
  int numData =atoi(argv[1]);
  write_latency_in_ns=atol(argv[2]);


  assert(cardinality < 64);

  long *keys;
  unsigned long *values;
  keys = new long[(sizeof(long)*numData)];
  values= new unsigned long[(sizeof(unsigned long)*numData)];

  ifstream ifs;
  ifs.open("../input_1b.txt");
  // ifs.open("../input_small.txt");

  assert(ifs);

  for(int i=0; i<numData; i++){
#ifdef DEBUG
    keys[i] = rand()%10000000;
#else
    ifs >> keys[i];
#endif
  }

  ifs.close();

  clflush_cnt=0;
  clock_gettime(CLOCK_MONOTONIC,&start);
  for(int i=0;i<numData;i++){
#ifdef DEBUG
    printf("inserting %lld\n", keys[i]);
#endif
    bt.btree_insert(keys[i], (char*) keys[i]);
#ifdef DEBUG
    bt.btree_search(keys[i]);
#endif
  }
  clock_gettime(CLOCK_MONOTONIC,&end);

  long long elapsedTime = (end.tv_sec-start.tv_sec)*1000000000 + (end.tv_nsec-start.tv_nsec);
  cout<<"INSERTION"<<endl;
  cout<<"elapsedTime : "<<elapsedTime/1000<<endl;
  cout<<"clflush_cnt : "<<clflush_cnt<<endl;
#ifdef DEBUG
  if (!bt.btree_ck()) {
    cout << "btree invalid, debug" << endl;
    exit(1);
  }
#endif

  char *garbage = new char[256*1024*1024];
  for(int i=0;i<256*1024*1024;i++){
    garbage[i] = i;
  }
  for(int i=100;i<256*1024*1024;i++){
    garbage[i] += garbage[i-100];
  }

  assert(bt.btree_ck());

  clock_gettime(CLOCK_MONOTONIC,&start);
  for(int i=0;i<numData;i++){
    bt.btree_binary_search(keys[i]);
  }
  clock_gettime(CLOCK_MONOTONIC,&end);

  elapsedTime = (end.tv_sec-start.tv_sec)*1000000000 + (end.tv_nsec-start.tv_nsec);
  cout<<"BINARY SEARCH"<<endl;
  cout<<"elapsedTime : "<<elapsedTime/1000 << "usec" <<endl;

  clock_gettime(CLOCK_MONOTONIC,&start);
  for(int i=0;i<numData;i++){
    bt.btree_search(keys[i]);
  }
  clock_gettime(CLOCK_MONOTONIC,&end);

  elapsedTime = (end.tv_sec-start.tv_sec)*1000000000 + (end.tv_nsec-start.tv_nsec);
  cout<<"LINEAR SEARCH"<<endl;
  cout<<"elapsedTime : "<<elapsedTime/1000 << "usec" <<endl;


  // Test btree::btree_delete()
  garbage = new char[256*1024*1024];
  for(int i=0;i<256*1024*1024;i++){
    garbage[i] = i;
  }
  for(int i=100;i<256*1024*1024;i++){
    garbage[i] += garbage[i-100];
  }

  clflush_cnt = 0;
  clock_gettime(CLOCK_MONOTONIC,&start);
  for(int i=0;i<numData;i++){
    bt.btree_delete(keys[i], 1);
  }
  clock_gettime(CLOCK_MONOTONIC,&end);

  elapsedTime = (end.tv_sec-start.tv_sec)*1000000000 + (end.tv_nsec-start.tv_nsec);
  cout<<"DELETION"<<endl;
  cout<<"elapsedTime : "<<elapsedTime/1000 << "usec" <<endl;
  cout<<"clflush_cnt : "<<clflush_cnt<<endl;

  return 0;
}
