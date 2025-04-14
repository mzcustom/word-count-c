#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>

typedef int64_t i64;
typedef int32_t i32; 
typedef uint32_t u32; 
typedef uint16_t u16; 
typedef int32_t bool;
typedef float f32;  
typedef uint8_t u8;   

#define TRUE 1;
#define FALSE 0;
#define SWAP(A, B)  do { typeof(A) temp = A; A = B; B = temp; } while(0)
#define ARRAY_LEN(ARR) sizeof(ARR)/sizeof(ARR[0])
#define IS_UPPER(CHAR) (((CHAR >= 64) && (CHAR <= 90))) || (CHAR == 39) // Ascii 39 is "'"
#define IS_LOWER(CHAR) (CHAR >= 97) && (CHAR <= 122)
#define IS_WORD_CHAR(CHAR) (IS_UPPER(CHAR)) || (IS_LOWER(CHAR))

#define FILE_PATH "./shakespeare.txt"

typedef struct {
    size_t length;
    u8 *data;
} string;

// for debug
static size_t node_pool_alloc_num = 0;

static string string_alloc(size_t length) {
    string result = {};
    result.data = (u8 *)malloc(length);
    if (!result.data) {
        fprintf(stderr, "ERROR: Unable to allocate %llu bytes.\n", length);
    } else {    
        result.length = length;
    }

    return result;
}

static void string_free(string *string) {
    if (string->data) {
        free(string->data);
    }
    
    string->length = 0; 
    string->data = 0;
}

static bool string_equal(string s1, string s2) {
    if (s1.length != s2.length) { return FALSE; }

    size_t count = s1.length;
    while (count-- > 0) {
        if (*s1.data++ != *s2.data++) { return FALSE; }
    }

    return TRUE;
}

static string read_entire_file(char *path) {
    string string_buffer = {};

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: Unable to open file %s\n", path);
    } else {
#if _WIN32
        struct __stat64 stat;
        _stat64(path, &stat);
#else
        struct stat stat;
        lstat(path, &stat);
#endif
        string_buffer = string_alloc(stat.st_size);
        
        if (string_buffer.data) {
            if (fread(string_buffer.data, string_buffer.length, 1, f) != 1) {
                fprintf(stderr, "ERROR: Unable to read file %s\n", path);
                string_free(&string_buffer);
            }
        }

        fclose(f);
    }

    return string_buffer;
}

typedef struct {
    string word; // key in hashtable
    u32 frequency; // value in hashtable
    // caching slot instead of hash can short-cut 1 add, 1 bit-wise op for each look-up.
    u32 slot; 
    //u32 hash; // it it better to save slot instead? If slot's included(fixed bucket size), remove this.
    struct word_node *next_node; // for chaining in the hashtable when colliding
} word_node;

#define NODE_POOL_SIZE 2 << 13 
typedef struct {
    size_t count;
    word_node data[NODE_POOL_SIZE];
    struct node_pool *next_pool;
} node_pool;

static node_pool *node_pool_alloc() {
    node_pool *result = (node_pool *)malloc(sizeof(node_pool));
    
    if (!result) {
        fprintf(stderr, "ERROR: unable to allocate a node pool\n");
    } else {
        memset(result, 0, sizeof(node_pool));
        node_pool_alloc_num++; // for debug
    }

    return result;
}

static void node_pool_free(node_pool *pool) {
    if (pool) {
        size_t pool_free_num = 0; // debug
        do {
            node_pool *temp = pool->next_pool;
            pool->next_pool = 0;
            free(pool);
            pool = temp;
            pool_free_num++; // for debug
        } while (pool);
        
        //printf("pool_free_nums: %llu", pool_free_num); // for debug
    }
}

static word_node *get_new_word_node(node_pool *pool) {
    word_node *result = 0;
   
    if (pool) {
        // find the newest pool that's not filled up.   
        while (pool->next_pool) { pool = pool->next_pool; } 
        assert(pool->count < NODE_POOL_SIZE);
        
        result = &pool->data[pool->count++];

        // create new pool when it's filled up.
        if (pool->count >= NODE_POOL_SIZE) {
            node_pool *new_pool = node_pool_alloc();
            pool->next_pool = new_pool;
        }
    }
    
    return result;
}

// Node hashtable with djb2 hash function.
// This is not a generic hashtable for generic type of a bigger data size. 
// It's a simple lineal-chain table for the specific data set and size for this exercise.
// When processing word nodes with multiple threads, the bucket size can be reduced for 
// each worker thread, however, the size should be still same of all the table, so that the
// slot for the same hash are identical for all the tables taken by each thread. That way, chaining
// becomes quick and easy when merging tables into one after processing words.
// I would still keep it large as long as it fits in the L3 Cache, 
// because once merged, node chains in certain slots could get long,
#define DJB2_INIT_HASH 5381
//#define DJB2_HASH_MULT 32
#define BUCKET_SIZE 2 << 12
typedef struct {
    size_t count;
    word_node *bucket[BUCKET_SIZE];
} node_table;

static node_table *node_table_alloc() {
    node_table *result = (node_table *)malloc(sizeof(node_table));

    if (!result) {
        fprintf(stderr, "ERROR: unable to allocate a node table\n");
    } else {
        memset(result, 0, sizeof(node_table));
    }

    return result;
}

static void node_table_free(node_table *table) {
    if (table) {
        free(table);
        table = 0;
    }
}

static word_node *find_node_from_table(node_table *table, u32 slot, string word) {
    word_node *result = 0;
    
    if (table) {
        word_node *node_found = table->bucket[slot];
        while (node_found) {
            if (string_equal(word, node_found->word)) { result = node_found; }
            node_found = node_found->next_node;
        }
    }

    return result;
}

static i64 qsort_partition(word_node array[], i64 begin, i64 end) {
    i64 pivot_value = array[end].frequency; // quick'n dirty. Better to sample a few elements and find the median.
    i64 left_end = begin - 1;

    for (i64 i = begin; i <= end; i++) {
        if (array[i].frequency >= pivot_value) {
            left_end++;
            SWAP(array[i], array[left_end]);
        }
    }

    return left_end;
}

static void qsort_pool(word_node array[], i64 begin, i64 end) {
    if (end > begin) {
        i64 pivot_index = qsort_partition(array, begin, end);
        qsort_pool(array, begin, pivot_index - 1);
        qsort_pool(array, pivot_index + 1, end);
    }
}

static void sort_pool(node_pool *pool) {
    qsort_pool(pool->data, 0, pool->count - 1);
}
    
#define C_STR_BUF_SIZE 256 
static void print_n_most_frequent_word(node_pool *pool, size_t n) {
    if (pool) {
        n = n > pool->count ? pool->count : n;
        char c_str_buf[C_STR_BUF_SIZE] = {};
        fprintf(stdout, "\n");

        for (size_t i = 0; i < n; ++i) {
            string word = pool->data[i].word;
            memcpy(c_str_buf, word.data, word.length);
            fprintf(stdout, "%llu. %s, frequency: %d\n", i+1, c_str_buf, pool->data[i].frequency);
            memset(c_str_buf, 0, word.length); 
        }
    }
}

// Word counting rules. Randomly chosen.
// Contraction("We'll", "know't", etc) and counted as 1 word
// Dash-connected word counted as separate words
// Case insensitive
// Arabic number not counted(only occurs once "3")
// Roman number counted as a word ("VI")"
static size_t create_table(string buffer, size_t file_begin, size_t file_end, node_pool *pool, node_table *table) {
    while (buffer.data[file_begin] == 39) { file_begin++; } // to avoid the case where file with with "'"
    
    size_t start = file_begin;
    size_t end = start;
    size_t word_node_count = 0;
    u32 hash_mask = (BUCKET_SIZE) - 1; // To mask out the top bits from hash without using mod

    while (start <= file_end - 1) { // skip "'" at the very bigging of the word.
        while(start <= file_end - 1 && ((buffer.data[start] == 39) || !(IS_WORD_CHAR(buffer.data[start])))) { 
            start++;
        }    
        end = start;

        u32 hash = DJB2_INIT_HASH;
        u32 slot = 0;
        while(end <= file_end - 1 && IS_WORD_CHAR(buffer.data[end])) {
            // skip "'" at the very end of the word.
            if ((buffer.data[end] == 39) && !(IS_WORD_CHAR(buffer.data[end+1]))) {
                break;
            }
            // for Case insensitive counting, subtract the lower case ascii value to the upper case
            // save the upper case converted char to the buffer, as well, for the simpler word comparison.
            i32 char_value = IS_LOWER(buffer.data[end]) ? buffer.data[end] -= 32 : buffer.data[end];
            hash = ((hash << 5) + hash) + char_value; // same as multiplying by DHB2_HASH_MULT + val
            end++;
        }
        slot = hash & hash_mask; // getting rid of top bits in lieu of doing mod

        if (start < end) {
            string word = (string){ end - start, buffer.data + start };
            word_node *existing_node = find_node_from_table(table, slot, word);
            if (existing_node) {
                existing_node->frequency++;
            } else {
                word_node *new_node = get_new_word_node(pool);
                if (new_node) {
                    new_node->word = word;
                    new_node->frequency = 1;
                    //new_node->hash = hash;
                    new_node->slot = slot;
                    word_node_count++;
                    
                    word_node *first_node_in_slot = table->bucket[slot];
                    if (first_node_in_slot) {
                        while(first_node_in_slot->next_node) { 
                            first_node_in_slot = first_node_in_slot->next_node;
                        }
                        // now, it's the last node in slot. Naming is hard.
                        first_node_in_slot->next_node = new_node;
                    } else {
                        table->bucket[slot] = new_node;
                    }
                    table->count++;
                }
            }

            start = end + 1;
        }
    }

    assert(word_node_count == table->count);

    return word_node_count;
}

int main(int agrc, char *argv[]) {
    fprintf(stdout, "\nUsage: q3 {N}, to find N most frequent words.\n ");
    fprintf(stdout, "      q3_simd {N}, same as above but runs faster.\n ");
    int n = atoi(argv[1]);
    
    string file_string = read_entire_file(FILE_PATH);
    if (file_string.length == 0) {
        fprintf(stderr, "Can't proceed for not being able to read the file.\n");
        return 1;     
    }
    
    node_pool *pool = node_pool_alloc(); 
    if (!pool) {
        fprintf(stderr, "Can't proceed for not being able to allocate memory for node pool.\n");
        string_free(&file_string);
        return 1;     
    }

    node_table *table = node_table_alloc();
    if (!table) {
        fprintf(stderr, "Can't proceed for not being able to allocate memory for node table.\n");
        node_pool_free(pool);
        string_free(&file_string);
        return 1;     
    }

    size_t word_node_count = create_table(file_string, 0, file_string.length, pool, table);
    sort_pool(pool);
    print_n_most_frequent_word(pool, n);
    
    //printf("Node pool alloc num: %llu\n", node_pool_alloc_num); // for debug
    
    // Free memory. Usually, this isn't necessary because OS "free" the memory and make it available
    // for other processes when this process gets termintated, anyway. This is just for the sake of 
    // old-skool "idiometic" C programming doing RAII sort of stuff manually.
    // When allocating pages with mmap or VirtualAlloc(windows), memory alloc and free gets simpler. <= next time!
    node_pool_free(pool);
    node_table_free(table);
    string_free(&file_string);
}
