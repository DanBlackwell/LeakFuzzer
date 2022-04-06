/*
   american fuzzy lop++ - bitmap related routines
   ----------------------------------------------

   Originally written by Michal Zalewski

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This is the real deal: the program takes an instrumented binary and
   attempts a variety of basic fuzzing tricks, paying close attention to
   how they affect the execution path.

 */

#include "afl-fuzz.h"
#include "hashfuzz.h"
#include "hashmap.h"
#include "lz4.h"
#include <limits.h>
#if !defined NAME_MAX
  #define NAME_MAX _XOPEN_NAME_MAX
#endif

static u32 prevLongest = 0;
static u32 maxCompressedLen = 0;
static u8 *uncompressedData = NULL;
static u8 *compressedData = NULL;
//#define NOISY

float calc_normalised_levenshtein_dist(afl_state_t *afl,
                                       struct queue_entry *queue_entry_1,
                                       struct queue_entry *queue_entry_2) {

  if (!queue_entry_2) { return 0.0f; }
  if (!queue_entry_1->len || !queue_entry_2->len) { return 0.0f; }
  if (queue_entry_1->len == queue_entry_2->len &&
      !memcmp(queue_entry_1->testcase_buf, queue_entry_2->testcase_buf, queue_entry_1->len)) {
    return 0.0f;
  }

  size_t len_1, len_2;
  u8 *str_1, *str_2;
  if (queue_entry_1->len > queue_entry_2->len) {
    len_1 = queue_entry_1->len;
    str_1 = queue_entry_1->testcase_buf;
    len_2 = queue_entry_2->len;
    str_2 = queue_entry_2->testcase_buf;
  } else {
    len_1 = queue_entry_2->len;
    str_1 = queue_entry_2->testcase_buf;
    len_2 = queue_entry_1->len;
    str_2 = queue_entry_1->testcase_buf;
  }

  if (!len_1 || !len_2) { return 0; }

  u32 matrix1[len_2]; // Previous row of distances
  u32 matrix2[len_2]; // Current row of distances

  u32 i, j;
  for (i = 0; i < len_2; i++)
    matrix1[i] = i;

  // handle case where len = 1
  matrix2[0] = str_1[0] == str_2[0] ? 0 : 1;

  for (i = 0; i < len_1 - 1; i++) {
    matrix2[0] = i + 1;
    for (j = 0; j < len_2 - 1; j++) {
      int cost = (str_1[i] == str_2[j]) ? 0 : 1;
#define LEVMIN(a, b, c) ((a) < (b) ? ((a) < (c) ? (a) : (c)) : ((b) < (c) ? (b) : (c)))
      matrix2[j+1] = LEVMIN(matrix2[j] + 1, matrix1[j+1] + 1, matrix1[j] + cost);
    }

    memcpy(matrix1, matrix2, sizeof(matrix2));
  }

  u32 edit_dist = matrix2[len_2 - 1];
  float norm_dist = (float)(len_1 - edit_dist) / (float)len_1;
  if (norm_dist > 1.0f || norm_dist < 0.0f) {
    printf("str_1: [");
    for (i = 0; i < len_1; i++) printf("%u, ", str_1[i]);
    printf("\b\b]\n");

    printf("str_2: [");
    for (i = 0; i < len_2; i++) printf("%u, ", str_2[i]);
    printf("\b\b]\n");

    printf("matrix1: [");
    for (i = 0; i < len_2; i++) printf("%u, ", matrix1[i]);
    printf("\b\b]\n");

    printf("matrix2: [");
    for (i = 0; i < len_2; i++) printf("%u, ", matrix2[i]);
    printf("\b\b]\n");

    FATAL("got norm dist %f, from formula %u - %u / %u\n", norm_dist, len_1, edit_dist, len_1);
  }

  return (float)(len_1 - edit_dist) / (float)len_1;
}

float calc_NCDm(afl_state_t *afl,
               struct queue_entry *queue_entries[],
               int queue_entries_count) {

  u32 totalLen = 0;
  u32 minCompressedLen = UINT32_MAX;

  for (int i = 0; i < queue_entries_count; i++) {
    struct queue_entry *entry = queue_entries[i];
#ifdef PATH_DIVERSITY
    u32 len = (afl->fsrv.map_size >> 3);
    totalLen += len;

    if (unlikely(!entry->compressed_len || !entry->trace_mini)) {
      u8 *input_buf = entry->trace_mini;
#else
    u32 len = entry->len;
    totalLen += len;
    if (unlikely(!entry->compressed_len || !entry->testcase_buf)) {
      u8 *input_buf = entry->testcase_buf;
#endif

      if (!input_buf) {
//        FATAL("No trace_mini for input!");
        printf("Oops - missing buffer for entry\n");
        input_buf = queue_testcase_get(afl, entry);
      }

      entry->compressed_len = LZ4_compress_default(input_buf,
                                                   compressedData,
                                                   (int)len,
                                                   (int)maxCompressedLen);
    }

    if (entry->compressed_len < minCompressedLen) {
      minCompressedLen = entry->compressed_len;
    }
  }

  if (prevLongest <= totalLen) {
    u32 bitcnt = 0, val = totalLen;
    while (val > 1) { bitcnt++; val >>= 1; }
    prevLongest = 1 << (bitcnt + 2);  // round up to next power of 2

    uncompressedData = ck_realloc(uncompressedData, prevLongest);
    if (!uncompressedData) printf("Realloc FAILED!\n");

    maxCompressedLen = LZ4_compressBound((int)prevLongest);
    compressedData = ck_realloc(compressedData, maxCompressedLen);
    if (!compressedData) printf("Realloc FAILED!\n");
  }

  u32 pos = 0;
  for (int i = 0; i < queue_entries_count; i++) {
    struct queue_entry *entry = queue_entries[i];
#ifdef PATH_DIVERSITY
    u32 len = (afl->fsrv.map_size >> 3);
    memcpy(uncompressedData + pos, entry->trace_mini, len);
    pos += len;
#else
    memcpy(uncompressedData + pos, entry->testcase_buf, entry->len);
    pos += entry->len;
#endif
  }
  u32 fullSetCompressedLen = LZ4_compress_default(uncompressedData,
                                                  compressedData,
                                                  (int)pos,
                                                  (int)maxCompressedLen);

  u32 maxSubsetCompressedLen = 0;
  for (int leftOut = 0; leftOut < queue_entries_count; leftOut++) {
    int pos = 0;
    for (int i = 0; i < queue_entries_count; i++) {
      if (i == leftOut) continue;
      struct queue_entry *entry = queue_entries[i];
#ifdef PATH_DIVERSITY
      u32 len = (afl->fsrv.map_size >> 3);
      memcpy(uncompressedData + pos, entry->trace_mini, len);
      pos += len;
#else
      memcpy(uncompressedData + pos, entry->testcase_buf, entry->len);
      pos += entry->len;
#endif
    }

    u32 compressedLen = LZ4_compress_default(uncompressedData,
                                             compressedData,
                                             (int)pos,
                                             (int)maxCompressedLen);

    if (compressedLen > maxSubsetCompressedLen) {
      maxSubsetCompressedLen = compressedLen;
    }
  }

  // don't divide by 0...
  if (maxSubsetCompressedLen == 0)
    return 0;

  return ((float)fullSetCompressedLen - (float)minCompressedLen) /
         (float)maxSubsetCompressedLen;
}

// returns 1 if traces differ in coverage, 0 if they are the same
u8 compare_trace_minis(afl_state_t *afl, u8 *trace1, u8 *trace2) {
  u64 *t1 = (u64 *)trace1; 
  u64 *t2 = (u64 *)trace2;
  u32 i = afl->fsrv.map_size >> 6;

  while (i--) {
    if (*t1 != *t2) {
      return 1;
    }
    t1++; t2++;
  }

  return 0;
}

// Returns 1 if trace1 contains coverage not seen in trace2
u8 trace_contains_new_coverage(afl_state_t *afl, u8 *trace1, u8 *trace2) {
  u64 *t1 = (u64 *)trace1;
  u64 *t2 = (u64 *)trace2;
  u32 i = afl->fsrv.map_size >> 6;

  while (i--) {
    if ((*t1 | *t2) != *t2) {
      return 1;
    }
    t1++; t2++;
  }

  return 0;
}

u32 count_minified_trace_bits(afl_state_t *afl, u8 *trace) {
  u64 *t = (u64 *)trace;
  u32 total = 0;
  u32 i = afl->fsrv.map_size >> 6;

  while (i--) {
    for (u8 i = 0; i < 64; i++)
      total += ((*t) >> i) & 1;
    t++;
  }

  return total;
}

void set_NCDm_favored(afl_state_t *afl) {
  u32 alloced = 100;
  struct queue_entry **selected_inputs = ck_alloc(alloced * sizeof(struct queue_entry *));
  u32 len = 0;

  for (u32 i = 0; i < afl->queued_paths; i++)
    afl->queue_buf[i]->ncdm_favored = false;

  u32 discovered_edges = count_non_255_bytes(afl, afl->virgin_bits);
  u8 *inverted = ck_alloc(afl->fsrv.map_size);
  for (u32 i = 0; i < afl->fsrv.map_size; i++) {
    inverted[i] = ~afl->virgin_bits[i];
  }
  u8 *all_discovered = ck_alloc(afl->fsrv.map_size >> 3);
  minimize_bits(afl, all_discovered, inverted);
  ck_free(inverted);

  u8 *selected_inputs_map = ck_alloc(afl->fsrv.map_size >> 3);

  float total_NCDm;

  while (compare_trace_minis(afl, all_discovered, selected_inputs_map)) {
    u32 shortest = UINT32_MAX;
    float best_NCDm = 0.0;
    struct queue_entry *best_candidate = NULL;
    bool found_cov = false;

    for (u32 i = 0; i < afl->queued_paths; i++) {
      struct queue_entry *q = afl->queue_buf[i];

      if (!trace_contains_new_coverage(afl, q->trace_mini, selected_inputs_map)) {
        // Adding this entry won't improve coverage so ignore it
        continue;
      }
      found_cov = true;

      if (len == 0) {
        if (q->compressed_len < shortest) {
          best_candidate = q;
          shortest = q->compressed_len;
        }
        continue;
      }

      selected_inputs[len] = q;
      float ncdm = calc_NCDm(afl, selected_inputs, (int)(len + 1));
      if (ncdm > best_NCDm) {
        best_candidate = q;
        best_NCDm = ncdm;
      }
    }

    if (!found_cov) {
      printf("Just about to bail, map_size: %u (>> 3 = %u)\n", afl->fsrv.map_size, afl->fsrv.map_size >> 3);
      fflush(stdout);
      FATAL("failed to find an entry providing new coverage???? got to %u edges, expected: %u edges (%u)",
            count_minified_trace_bits(afl, selected_inputs_map), count_minified_trace_bits(afl, all_discovered), discovered_edges);
    }

    u64 *discovered = (u64 *)selected_inputs_map;
    u64 *new_input_map = (u64 *)best_candidate->trace_mini;
    u32 i = afl->fsrv.map_size >> 6;

    while (i--) {
      *discovered |= *new_input_map;
      new_input_map++;
      discovered++;
    }

    selected_inputs[len] = best_candidate;
    len++;
    best_candidate->ncdm_favored = true;
    total_NCDm = best_NCDm;
    if (len + 1 > alloced) {
      alloced *= 2;
      selected_inputs = ck_realloc(selected_inputs, alloced * sizeof(u8*));
    }

  }

  char favs_buf[8192]; int fav_buf_pos = 0;
  fav_buf_pos = sprintf(favs_buf, "favs: [");
  char ncd_buf[8192]; int ncd_buf_pos = 0;
  ncd_buf_pos = sprintf(ncd_buf, "NCDm_favs: [");

  struct queue_entry *favs[1024]; u32 fav_pos = 0;
  for (u32 i = 0; i < afl->queued_paths; i++) {
    if (afl->queue_buf[i]->favored) {
      favs[fav_pos++] = afl->queue_buf[i];
      fav_buf_pos += sprintf(favs_buf + fav_buf_pos, "%u, ", i);
    }
    if (afl->queue_buf[i]->ncdm_favored) {
      ncd_buf_pos += sprintf(ncd_buf + ncd_buf_pos, "%u, ", i);
    }
  }
  float favored_NCDm = calc_NCDm(afl, favs, fav_pos);

  ck_free(selected_inputs);
  ck_free(selected_inputs_map);
  ck_free(all_discovered);

  printf("Managed to get an NCD maxed subset (with 100%% coverage) in %u entries with NCDm: %f (vs %u favored entries with NCDm: %f)\n",
         len, total_NCDm, afl->queued_favored, favored_NCDm);
  sprintf(favs_buf + fav_buf_pos, "\b\b]\n");
  sprintf(ncd_buf + ncd_buf_pos, "\b\b]\n");
  printf("%s", favs_buf);
  printf("%s", ncd_buf);
}

/* Returns the index of the existing candidate that when replaced give the
 * largest NCD. If forced is false, then return -1 if the new_entry cannot beat
 * any others. If forced is true then just find best candidate regardless */

int find_eviction_candidate(afl_state_t *afl,
#ifdef  LEVENSHTEIN_DIST
                            float existing_entries_lev_dist,
#else
                            float existing_entries_NCD,
#endif
                            struct queue_entry **existing_edge_entries,
                            int existing_entries_count,
                            struct queue_entry *new_entry,
                            bool forced) {
#ifdef LEVENSHTEIN_DIST
  if (existing_entries_count != 2) {
    PFATAL("Need 2 entries only for levenshtein dist\n");
  }
#else
  if (existing_entries_count > 32) {
    PFATAL("Cannot handle more than 32 entries\n");
  }
#endif

  struct queue_entry *all_entries[33];

  int evictionCandidate = -1;
#ifdef LEVENSHTEIN_DIST
  float best_dist = forced ? 0.0f : existing_entries_lev_dist;
#else
  float best_dist = forced ? 0.0f : existing_entries_NCD;
#endif

  for (int i = 0; i < existing_entries_count; i++) {
    memcpy(all_entries, existing_edge_entries, sizeof(existing_edge_entries) * i);
    memcpy(all_entries + i,
           &existing_edge_entries[i + 1],
           sizeof(existing_edge_entries) * (existing_entries_count - 1 - i));
    all_entries[existing_entries_count - 1] = new_entry;

#ifdef LEVENSHTEIN_DIST
    float candidate_dist = calc_normalised_levenshtein_dist(afl, existing_edge_entries[0], existing_edge_entries[1]);
#else
    float candidate_dist = calc_NCDm(afl, all_entries, existing_entries_count);
#endif
    if (candidate_dist > best_dist) {
      evictionCandidate = i;
      best_dist = candidate_dist;
    }
  }

#ifdef NOISY
  printf("  New best candidate NCD: %0.05f [was: %0.05f]\n", bestNCD, initialNCD);
#endif

#ifdef LEVENSHTEIN_DIST
  if (!forced && best_dist <= existing_entries_lev_dist) {
#else
  if (!forced && best_dist <= existing_entries_NCD) {
#endif
    return -1;
  }

  return evictionCandidate;
}

bool printPathPartition(const void *item, void *udata) {
  const struct path_partitions *pp = item;
  afl_state_t *afl = udata;

  printf("{ %020llu: { ncd: %0.05f, queue_entries (indices): [", pp->checksum, pp->normalised_compression_dist);
  for (int i = 0; i < pp->foundPartitionsCount; i++) {
    for (u32 j = 0; j < afl->queued_paths; j++) {
      if (afl->queue_buf[j] == pp->queue_entries[i]) {
        printf("%d, ", j);
        break;
      }
    }
  }
  printf("\b\b] } }\n");

  return true;
}

void dumpOutDebugInfo(afl_state_t *afl) {
  printf("queued_paths (indices): [");
  for (u32 i = 0; i < afl->queued_paths; i++) {
    if (!afl->queue_buf[i]->disabled)
      printf("%d, ", i);
  }
  printf("\b\b]\n");

  printf("PathPartitions:\n");
  hashmap_scan(hashfuzzFoundPartitions, printPathPartition, afl);
}


/* Write bitmap to file. The bitmap is useful mostly for the secret
   -B option, to focus a separate fuzzing session on a particular
   interesting input without rediscovering all the others. */

void write_bitmap(afl_state_t *afl) {

  u8  fname[PATH_MAX];
  s32 fd;

  if (!afl->bitmap_changed) { return; }
  afl->bitmap_changed = 0;

  snprintf(fname, PATH_MAX, "%s/fuzz_bitmap", afl->out_dir);
  fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, DEFAULT_PERMISSION);

  if (fd < 0) { FATAL("Unable to open '%s'", fname); }

  ck_write(fd, afl->virgin_bits, afl->fsrv.map_size, fname);

  close(fd);

}

/* Count the number of bits set in the provided bitmap. Used for the status
   screen several times every second, does not have to be fast. */

u32 count_bits(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = (afl->fsrv.map_size >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    /* This gets called on the inverse, virgin bitmap; optimize for sparse
       data. */

    if (v == 0xffffffff) {

      ret += 32;
      continue;

    }

    v -= ((v >> 1) & 0x55555555);
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    ret += (((v + (v >> 4)) & 0xF0F0F0F) * 0x01010101) >> 24;

  }

  return ret;

}

/* Count the number of bytes set in the bitmap. Called fairly sporadically,
   mostly to update the status screen or calibrate and examine confirmed
   new paths. */

u32 count_bytes(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = (afl->fsrv.map_size >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    if (!v) { continue; }
    if (v & 0x000000ffU) { ++ret; }
    if (v & 0x0000ff00U) { ++ret; }
    if (v & 0x00ff0000U) { ++ret; }
    if (v & 0xff000000U) { ++ret; }

  }

  return ret;

}

/* Count the number of non-255 bytes set in the bitmap. Used strictly for the
   status screen, several calls per second or so. */

u32 count_non_255_bytes(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = (afl->fsrv.map_size >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    /* This is called on the virgin bitmap, so optimize for the most likely
       case. */

    if (v == 0xffffffffU) { continue; }
    if ((v & 0x000000ffU) != 0x000000ffU) { ++ret; }
    if ((v & 0x0000ff00U) != 0x0000ff00U) { ++ret; }
    if ((v & 0x00ff0000U) != 0x00ff0000U) { ++ret; }
    if ((v & 0xff000000U) != 0xff000000U) { ++ret; }

  }

  return ret;

}

/* Destructively simplify trace by eliminating hit count information
   and replacing it with 0x80 or 0x01 depending on whether the tuple
   is hit or not. Called on every new crash or timeout, should be
   reasonably fast. */
#define TIMES4(x) x, x, x, x
#define TIMES8(x) TIMES4(x), TIMES4(x)
#define TIMES16(x) TIMES8(x), TIMES8(x)
#define TIMES32(x) TIMES16(x), TIMES16(x)
#define TIMES64(x) TIMES32(x), TIMES32(x)
#define TIMES255(x)                                                      \
  TIMES64(x), TIMES64(x), TIMES64(x), TIMES32(x), TIMES16(x), TIMES8(x), \
      TIMES4(x), x, x, x
const u8 simplify_lookup[256] = {

    [0] = 1, [1] = TIMES255(128)

};

/* Destructively classify execution counts in a trace. This is used as a
   preprocessing step for any newly acquired traces. Called on every exec,
   must be fast. */

const u8 count_class_lookup8[256] = {

        [0] = 0,
        [1] = 1,
        [2] = 2,
        [3] = 4,
        [4 ... 7] = 8,
        [8 ... 15] = 16,
        [16 ... 31] = 32,
        [32 ... 127] = 64,
        [128 ... 255] = 128

};

#undef TIMES255
#undef TIMES64
#undef TIMES32
#undef TIMES16
#undef TIMES8
#undef TIMES4

u16 count_class_lookup16[65536];

void init_count_class16(void) {
  u32 b1, b2;

  for (b1 = 0; b1 < 256; b1++) {

    for (b2 = 0; b2 < 256; b2++) {

      count_class_lookup16[(b1 << 8) + b2] =
          (count_class_lookup8[b1] << 8) | count_class_lookup8[b2];

    }

  }
}

/* Import coverage processing routines. */

#ifdef WORD_SIZE_64
  #include "coverage-64.h"
#else
  #include "coverage-32.h"
#endif

u8 is_interesting(afl_state_t *afl) {
  if (!afl->edge_entry_count) {
    printf("Skipping is_interesting as afl not yet inited\n");
    return 0;
  }

#ifdef WORD_SIZE_64
  u64 *current = (u64 *)afl->fsrv.trace_bits;
  u32 i = (afl->fsrv.map_size >> 3);
#else
  I HAVE NOT IMPLEMENTED 32 BIT sorry
#endif

  printf("is_interesting: input %020llu [map size: %u]\n",
         hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST), i);

  int edgeNum = 0;
  while (i--) {
    if (*current) {
      u16 mem16[4];
      memcpy(mem16, current, sizeof(mem16));

      mem16[0] = count_class_lookup16[mem16[0]];
      mem16[1] = count_class_lookup16[mem16[1]];
      mem16[2] = count_class_lookup16[mem16[2]];
      mem16[3] = count_class_lookup16[mem16[3]];

      for (int i = 0; i < 4; i++) {
        if (mem16[i]) {
          int reps = 0;
          u16 class = mem16[i];
          while ((class >> reps) > 1) reps++;

          u16 restored[4];
          memcpy(restored, current, sizeof(restored));

          u32 edge_entries_pos = 16 * (edgeNum + i) + reps;
          struct edge_entry *this_edge = &afl->edge_entries[edge_entries_pos];

          this_edge->hit_count++;
        }
      }
    }

    current++;
    edgeNum += 4;
  }

  return 1;

  return 0;
}

void move_queue_entry_to_correct_input_hash(afl_state_t *afl, struct queue_entry *evictee, struct queue_entry *new) {
  // First let's remove the evictee from it's current hash
  struct queue_input_hash input_hash = { .hash = evictee->input_hash };
  struct queue_input_hash *found = hashmap_get(afl->queue_input_hashmap, &input_hash);

  if (found->hash != evictee->input_hash) {
    FATAL("found->hash %020llu != evictee->input_hash %020llu", found->hash, evictee->input_hash);
  }

  if (!found) {
    FATAL("Failed to find queue_input_hash for %020llu\n", input_hash.hash);
  }

  bool removed = false;
  for (u32 i = 0; i < found->inputs_count; i++) {
    struct queue_entry *entry = found->inputs[i];
    entry->duplicates = found->inputs_count >= 2 ? found->inputs_count - 2 : 0;
    if (entry == evictee) { removed = true; }
    if (removed && i < found->inputs_count - 1) {
      found->inputs[i] = found->inputs[i + 1];
    }
  }
  found->inputs_count--;


  if (!removed) {
    int pos = -1;
    if (found->inputs_count < 10000) {
      for (u32 i = 0; i < afl->queued_paths; i++) {
        if (afl->queue_buf[i] == evictee) {
          pos = i;
          break;
        }
      }
    } else { printf("found->inputs_count = %d\n", found->inputs_count); }

    printf("Found for %020llu (%d): ", found->hash, found->inputs_count);
    for (u32 i = 0; i < found->inputs_count; i++) {
      printf("%p, ", found->inputs[i]);
    }
    printf("\n");
    FATAL("Failed to find this queue_entry[%d] (%p) in list of found->inputs %020llu\n", pos, evictee, input_hash.hash);
  }
//  printf("Found queue_entry[%d] (%p) in list of found->inputs %020llu\n", pos, evictee, hash);

  // Then let's insert this evictee into its new hash
  input_hash.hash = new->input_hash;
  evictee->input_hash = new->input_hash;
  found = hashmap_get(afl->queue_input_hashmap, &input_hash);

  if (!found) {
    input_hash.allocated_inputs = 8;
    input_hash.inputs = ck_alloc(sizeof(input_hash.inputs) * input_hash.allocated_inputs);
    input_hash.inputs[0] = evictee;
    evictee->duplicates = 0;
    input_hash.inputs_count = 1;
    hashmap_set(afl->queue_input_hashmap, &input_hash);
    found = hashmap_get(afl->queue_input_hashmap, &input_hash);

  } else {
    if (found->allocated_inputs == found->inputs_count) {
      found->allocated_inputs *= 2;
      found->inputs = ck_realloc(found->inputs, sizeof(found->inputs) * found->allocated_inputs);
    }
    found->inputs[found->inputs_count] = evictee;
    found->inputs_count++;

    for (u32 i = 0; i < found->inputs_count; i++) {
      found->inputs[i]->duplicates = found->inputs_count >= 1 ?
                                     found->inputs_count - 1 :
                                     0;
    }
  }
}

void swap_in_candidate(afl_state_t *afl, struct queue_entry *evictee, struct queue_entry *new) {
  move_queue_entry_to_correct_input_hash(afl, evictee, new);

  ck_free(evictee->testcase_buf);
  evictee->len = new->len;
  evictee->testcase_buf = ck_alloc(new->len);
  memcpy(evictee->testcase_buf, new->testcase_buf, new->len);
  evictee->compressed_len = new->compressed_len;
  memcpy(evictee->trace_mini, new->trace_mini, afl->fsrv.map_size >> 3);

  int fd = open(evictee->fname, O_WRONLY | O_TRUNC, DEFAULT_PERMISSION);
  if (unlikely(fd < 0)) { FATAL("Unable to open '%s'", evictee->fname); }
  ck_write(fd, evictee->testcase_buf, evictee->len, evictee->fname);
  close(fd);

  char *pathEnd = strrchr(evictee->fname, '/');
  size_t maxLen = NAME_MAX + (pathEnd - (char *)evictee->fname);
  char *newFilename = ck_alloc(maxLen);
  long newFilenameLen = 0;
  char *opPos = strstr(evictee->fname, ",op:");
  if (!opPos) {
    FATAL("Failed to find \"op:\" in %s\n", evictee->fname);
  }

  char *updatedPos = strstr(evictee->fname, ",updated:");
  if (updatedPos) {
    newFilenameLen = updatedPos - (char *)evictee->fname;
  } else {
    newFilenameLen = opPos - (char *)evictee->fname;
  }
  memcpy(newFilename, evictee->fname, newFilenameLen);

  newFilenameLen += snprintf(newFilename + newFilenameLen,
                             maxLen - newFilenameLen,
                             ",updated:%llu",
                             get_cur_time() + afl->prev_run_time - afl->start_time);
  snprintf(newFilename + newFilenameLen,
           maxLen - newFilenameLen,
           "%s", opPos);

  int ret = rename(evictee->fname, newFilename);
  if (ret) {
    FATAL("Failed to rename %s to %s\n", evictee->fname, newFilename);
  }

  ck_free(evictee->fname);
  evictee->fname = newFilename;
}

u8 *get_filename(afl_state_t *afl, u64 cksum, struct edge_entry *entry) {
  // If there's an eviction then no new file will be created
  return alloc_printf(
      "%s/queue/id:%06u,edge_num:%hu,edge_freq:%hu,cksum:%06llu,entry:%d,%s", afl->out_dir,
      afl->queued_paths,
      entry->edge_num, entry->edge_frequency,cksum,
      entry->entry_count,
      describe_op(afl, 0, entry->entry_count > 0, NAME_MAX - 35));
}

void fill_trace_mini_and_compressed_len(afl_state_t  *afl, struct queue_entry *q_entry) {
  u32 trace_map_len = (afl->fsrv.map_size >> 3);
#ifdef PATH_DIVERSITY
  if (2 * trace_map_len > prevLongest) {
    u32 bitcnt = 0, val = trace_map_len;
#else
  if (2 * q_entry->len > prevLongest) {
    u32 bitcnt = 0, val = q_entry->len;
#endif
    while (val > 1) { bitcnt++; val >>= 1; }
    prevLongest = 1 << (bitcnt + 2);  // round up to next power of 2

    uncompressedData = ck_realloc(uncompressedData, prevLongest);
    if (!uncompressedData) printf("Realloc FAILED!\n");

    maxCompressedLen = LZ4_compressBound((int)prevLongest);
    compressedData = ck_realloc(compressedData, maxCompressedLen);
    if (!compressedData) printf("Realloc FAILED!\n");
  }

  q_entry->trace_mini = ck_alloc(trace_map_len);
  minimize_bits(afl, q_entry->trace_mini, afl->fsrv.trace_bits);
#ifdef PATH_DIVERSITY
  q_entry->compressed_len = LZ4_compress_default(
      q_entry->trace_mini, compressedData,
      (int)trace_map_len, (int)maxCompressedLen
  );
#else
  q_entry->compressed_len = LZ4_compress_default(
      q_entry->testcase_buf, compressedData,
      (int)q_entry->len, (int) maxCompressedLen
  );
#endif
  if (!q_entry->compressed_len) {
    FATAL("compressedLen failed! (input len: %d)", q_entry->len);
  }
}

u8 save_to_edge_entries(afl_state_t *afl, struct queue_entry *q_entry, u8 new_bits) {
  if (unlikely(!afl->edge_entry_count)) {
    printf("Skipping is_interesting as afl not yet inited\n");
    return 0;
  }

#ifdef WORD_SIZE_64
  u64 *current = (u64 *)afl->fsrv.trace_bits;
  u32 i = (afl->fsrv.map_size >> 3);
#else
  I HAVE NOT IMPLEMENTED 32 BIT sorry
#endif

  bool calibration_complete = false;
  // Things calculated by calibration - can assume these won't change!
  u8 cal_failed;
  u64 exec_us;
  u64 exec_cksum = 0;
  u32 bitmap_size;
  u64 handicap;

  struct queue_input_hash input_hash = {};
  struct queue_input_hash *found = NULL;

  u64 hash = hash64(q_entry->testcase_buf, q_entry->len, HASH_CONST);
  q_entry->input_hash = hash;
  input_hash.hash = hash;
  found = hashmap_get(afl->queue_input_hashmap, &input_hash);
  bool is_duplicate = found != NULL;

  u8 inserted = false;

  int edgeNum = 0;
  while (i--) {
    if (*current) {
      u8 mem8[8];
      memcpy(mem8, current, sizeof(mem8));

      for (int i = 0; i < 8; i++) {
        if (!mem8[i]) continue;

        int reps = 0;
        u8 class = count_class_lookup8[mem8[i]];
        while ((class >> reps) > 1) reps++;

        u32 edge_entries_pos = 8 * (edgeNum + i) + reps;
        struct edge_entry *this_edge = &afl->edge_entries[edge_entries_pos];
        this_edge->hit_count++;
#ifdef NOISY
        printf("Hit edge: %hu, bucket: %hu\n", this_edge->edge_num, this_edge->edge_frequency);
#endif

        bool match = false;
        for (int i = 0; i < this_edge->entry_count; i++) {
          if (unlikely(this_edge->entries[i]->input_hash == q_entry->input_hash)) {
            match = true;
            break;
          }
        }

        // we already have this entry in the queue
        if (unlikely(match)) {
#ifdef NOISY
          printf("  Identical to existing queue entry, skipping\n");
#endif
          continue;
        }

        if (unlikely(this_edge->entry_count < afl->ncd_entries_per_edge)) {
          // Let's record what execution we were on when discovering this edge
          if (unlikely(this_edge->entry_count == 0)) {
            this_edge->discovery_execs = afl->fsrv.total_execs;
            afl->pending_edge_entries++;
            afl->discovered_edge_entries++;
          }

          // If there's already an entry for this edge then no point in storing
          // another copy of an input that is already in the queue
          if (likely(this_edge->entry_count > 0 && is_duplicate)) {
            continue;
          }
#ifdef NOISY
          printf("  Inserting candidate w checksum %020llu at pos %d\n",
                   q_entry->exec_cksum, this_edge->entry_count);
#endif

          // OK this is a new edge or unique entry so let's store it
          u8 *queue_fname = get_filename(afl, q_entry->exec_cksum, this_edge);
          int fd = open(queue_fname, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
          if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", queue_fname); }
          ck_write(fd, q_entry->testcase_buf, q_entry->len, queue_fname);
          close(fd);
          add_to_queue(afl,
                       queue_fname,
                       q_entry->len,
                       0,
                       this_edge->entry_count,
                       0, // can't use this cksum as it's not classified counts
                       new_bits);

          struct queue_entry *new = afl->queue_top;
          new->testcase_buf = ck_alloc(new->len);
          new->exec_cksum = 0;
          memcpy(new->testcase_buf, q_entry->testcase_buf, new->len);
          new->input_hash = q_entry->input_hash;
          new->trace_mini = ck_alloc(afl->fsrv.map_size >> 3);
          if (!q_entry->trace_mini)
            fill_trace_mini_and_compressed_len(afl, q_entry);
          memcpy(new->trace_mini, q_entry->trace_mini, afl->fsrv.map_size >> 3);

          if (found) {
            if (found->inputs_count == found->allocated_inputs) {
              found->allocated_inputs *= 2;
              found->inputs = ck_realloc(
                  found->inputs,
                  found->allocated_inputs * sizeof(found->inputs)
              );
            }

            found->inputs[found->inputs_count] = new;
            found->inputs_count++;

            u32 duplicates = found->inputs_count >= 1 ?
                             found->inputs_count - 1 :
                             0;
            for (u32 i = 0; i < found->inputs_count; i++) {
              found->inputs[i]->duplicates = duplicates;
            }

          } else {
            struct queue_input_hash new_hash = {};
            new_hash.hash = new->input_hash;
            new_hash.allocated_inputs = 8;
            new_hash.inputs = ck_alloc(new_hash.allocated_inputs *
                                         sizeof(new_hash.inputs));
            new_hash.inputs[0] = new;
            new_hash.inputs_count = 1;
            hashmap_set(afl->queue_input_hashmap, &new_hash);
            found = hashmap_get(afl->queue_input_hashmap, &new_hash);
            is_duplicate = true;
          }

          this_edge->entries[this_edge->entry_count] = new;
          new->edge_entry = this_edge;
          this_edge->entry_count++;

#ifdef LEVENSHTEIN_DIST
          this_edge->normalised_levenshtein_dist =
              calc_normalised_levenshtein_dist(afl,
                                               this_edge->entries[0],
                                               this_edge->entries[1]);
#else
          this_edge->normalised_compression_dist = calc_NCDm(afl, this_edge->entries, this_edge->entry_count);
#endif
          inserted = true;

          if (calibration_complete) {
            new->cal_failed = cal_failed;
            new->exec_us = exec_us;
            new->exec_cksum = exec_cksum;
            new->bitmap_size = bitmap_size;
            new->handicap = handicap;
          } else {
            calibrate_case(afl, new, new->testcase_buf, afl->queue_cycle - 1, 0);
            calibration_complete = true;
            cal_failed = new->cal_failed;
            exec_us = new->exec_us;
            exec_cksum = new->exec_cksum;
            bitmap_size = new->bitmap_size;
            handicap = new->handicap;
          }

          continue;
        }

        if (unlikely(is_duplicate)) {
          // This queue_entry already exists for another edge
          continue;
        }

        int evictionCandidate = -1;

        // let's see if any entries are duplicates and mark for eviction:
        for (u32 i = 0; i < this_edge->entry_count; i++) {
          struct queue_entry *entry = this_edge->entries[i];
          if (unlikely(entry->duplicates)) {
            evictionCandidate = (int)i;
            break;
          }
        }

        // We haven't found any duplicates to kick out, let's see if NCD wins
        if (evictionCandidate == -1) {
          bool should_calc_NCD =
              this_edge->hit_count <= 10 ||
              (this_edge->hit_count <= 100 && this_edge->hit_count % 10 == 0) ||
              (this_edge->hit_count <= 10000 && this_edge->hit_count % 100 == 0) ||
              (this_edge->hit_count % 1000 == 0);

          if (likely(!should_calc_NCD)) {
            continue;
          }

          if (!q_entry->trace_mini)
            fill_trace_mini_and_compressed_len(afl, q_entry);

          evictionCandidate = find_eviction_candidate(
              afl,
#ifdef LEVENSHTEIN_DIST
              this_edge->normalised_levenshtein_dist,
#else
              this_edge->normalised_compression_dist,
#endif
              this_edge->entries,
              this_edge->entry_count,
              q_entry,
              false
          );
          if (likely(evictionCandidate == -1)) {
            continue;
          }
        }

        if (unlikely(!q_entry->trace_mini))
          fill_trace_mini_and_compressed_len(afl, q_entry);

        // We have a real candidate to evict...
        struct queue_entry *evictee = this_edge->entries[evictionCandidate];
#ifdef NOISY
        printf("  Will evict candidate at pos %d, w checksum %020llu in favour of current w checksum %020llu\n",
                   evictionCandidate, evictee->exec_cksum, q_entry->exec_cksum);
#endif
        swap_in_candidate(afl, evictee, q_entry);
        evictee->exec_cksum = 0;
        evictee->input_hash = q_entry->input_hash;

        found = hashmap_get(afl->queue_input_hashmap, &input_hash);
        is_duplicate = true;

        this_edge->replacement_count++;
#ifdef LEVENSHTEIN_DIST
        this_edge->normalised_levenshtein_dist = calc_normalised_levenshtein_dist(afl, this_edge->entries[0], this_edge->entries[1]);
#else
        this_edge->normalised_compression_dist = calc_NCDm(afl, this_edge->entries, this_edge->entry_count);
#endif

        if (unlikely(evictee->favored)) {
          evictee->favored = false;

          for (u32 i = 0; i < afl->fsrv.map_size; i++) {
            if (afl->top_rated[i] == evictee) {
              // This new entry doesn't cover that edge - find a replacement!
              u64 best_fav_score = UINT64_MAX;
              struct queue_entry *best_entry = NULL;

              // Go through all the edge_entries by bin
              for (u32 reps = 0; reps < 8; reps++) {
                struct edge_entry *edgeEntry = &afl->edge_entries[8 * i + reps];

                // Go through all the queue_entries for this bin
                for (int entry = 0; entry < edgeEntry->entry_count; entry++) {
                  u64 fav_score = get_fav_factor(afl, edgeEntry->entries[entry]);
                  if (fav_score < best_fav_score) {
                    best_fav_score = fav_score;
                    best_entry = edgeEntry->entries[entry];
                  }
                }
              }

              if (likely(best_entry)) {
                afl->top_rated[i] = NULL;
                update_bitmap_score(afl, best_entry);
                if (!best_entry->was_fuzzed) {
                  best_entry->fuzz_level = evictee->fuzz_level;
                  best_entry->was_fuzzed = evictee->was_fuzzed;
                }
              } else {
                // This can happen occasionally if the entry that we're adding now is the
                // first entry for that edge (as calibrate_case adds it immediately)
                // This is no issue!
                evictee->favored = true;
              }

            }
          }
        }

        if (unlikely(calibration_complete)) {
          evictee->cal_failed = cal_failed;
          evictee->exec_us = exec_us;
          evictee->exec_cksum = exec_cksum;
          evictee->bitmap_size = bitmap_size;
          evictee->handicap = handicap;
        } else {
          calibrate_case(afl, evictee, evictee->testcase_buf, afl->queue_cycle - 1, 0);
          calibration_complete = true;
          cal_failed = evictee->cal_failed;
          exec_us = evictee->exec_us;
          exec_cksum = evictee->exec_cksum;
          bitmap_size = evictee->bitmap_size;
          handicap = evictee->handicap;
        }

        inserted = true;
      }
    }

    current++;
    edgeNum += 8;
  }

  ck_free(q_entry->trace_mini);

  return inserted;
}

/* Check if the current execution path brings anything new to the table.
   Update virgin bits to reflect the finds. Returns 1 if the only change is
   the hit-count for a particular tuple; 2 if there are new tuples seen.
   Updates the map, so subsequent calls will always return 0.

   This function is called after every exec() on a fairly large buffer, so
   it needs to be fast. We do this in 32-bit and 64-bit flavors. */

inline u8 has_new_bits(afl_state_t *afl, u8 *virgin_map) {

#ifdef WORD_SIZE_64

  u64 *current = (u64 *)afl->fsrv.trace_bits;
  u64 *virgin = (u64 *)virgin_map;

  u32 i = (afl->fsrv.map_size >> 3);

#else

  u32 *current = (u32 *)afl->fsrv.trace_bits;
  u32 *virgin = (u32 *)virgin_map;

  u32 i = (afl->fsrv.map_size >> 2);

#endif                                                     /* ^WORD_SIZE_64 */

  u8 ret = 0;
  while (i--) {

    if (unlikely(*current)) discover_word(&ret, current, virgin);

    current++;
    virgin++;

  }

  if (unlikely(ret) && likely(virgin_map == afl->virgin_bits))
    afl->bitmap_changed = 1;

  return ret;

}

/* A combination of classify_counts and has_new_bits. If 0 is returned, then the
 * trace bits are kept as-is. Otherwise, the trace bits are overwritten with
 * classified values.
 *
 * This accelerates the processing: in most cases, no interesting behavior
 * happen, and the trace bits will be discarded soon. This function optimizes
 * for such cases: one-pass scan on trace bits without modifying anything. Only
 * on rare cases it fall backs to the slow path: classify_counts() first, then
 * return has_new_bits(). */

inline u8 has_new_bits_unclassified(afl_state_t *afl, u8 *virgin_map) {

  /* Handle the hot path first: no new coverage */
  u8 *end = afl->fsrv.trace_bits + afl->fsrv.map_size;

#ifdef WORD_SIZE_64

  if (!skim((u64 *)virgin_map, (u64 *)afl->fsrv.trace_bits, (u64 *)end))
    return 0;

#else

  if (!skim((u32 *)virgin_map, (u32 *)afl->fsrv.trace_bits, (u32 *)end))
    return 0;

#endif                                                     /* ^WORD_SIZE_64 */
  classify_counts(&afl->fsrv);
  return has_new_bits(afl, virgin_map);

}

/* Compact trace bytes into a smaller bitmap. We effectively just drop the
   count information here. This is called only sporadically, for some
   new paths. */

void minimize_bits(afl_state_t *afl, u8 *dst, u8 *src) {

  u32 i = 0;

  while (i < afl->fsrv.map_size) {

    if (*(src++)) { dst[i >> 3] |= 1 << (i & 7); }
    ++i;

  }

}

#ifndef SIMPLE_FILES

/* Construct a file name for a new test case, capturing the operation
   that led to its discovery. Returns a ptr to afl->describe_op_buf_256. */

u8 *describe_op(afl_state_t *afl, u8 new_bits, u8 new_partition, size_t max_description_len) {

  size_t real_max_len =
      MIN(max_description_len, sizeof(afl->describe_op_buf_256));
  u8 *ret = afl->describe_op_buf_256;

  if (unlikely(afl->syncing_party)) {

    sprintf(ret, "sync:%s,src:%06u", afl->syncing_party, afl->syncing_case);

  } else {

    sprintf(ret, "src:%06u", afl->current_entry);

    if (afl->splicing_with >= 0) {

      sprintf(ret + strlen(ret), "+%06d", afl->splicing_with);

    }

    sprintf(ret + strlen(ret), ",time:%llu",
            get_cur_time() + afl->prev_run_time - afl->start_time);

    if (afl->current_custom_fuzz &&
        afl->current_custom_fuzz->afl_custom_describe) {

      /* We are currently in a custom mutator that supports afl_custom_describe,
       * use it! */

      size_t len_current = strlen(ret);
      ret[len_current++] = ',';
      ret[len_current] = '\0';

      ssize_t size_left = real_max_len - len_current - strlen(",+cov") - 2;
      if (unlikely(size_left <= 0)) FATAL("filename got too long");

      const char *custom_description =
          afl->current_custom_fuzz->afl_custom_describe(
              afl->current_custom_fuzz->data, size_left);
      if (!custom_description || !custom_description[0]) {

        DEBUGF("Error getting a description from afl_custom_describe");
        /* Take the stage name as description fallback */
        sprintf(ret + len_current, "op:%s", afl->stage_short);

      } else {

        /* We got a proper custom description, use it */
        strncat(ret + len_current, custom_description, size_left);

      }

    } else {

      /* Normal testcase descriptions start here */
      sprintf(ret + strlen(ret), ",op:%s", afl->stage_short);

      if (afl->stage_cur_byte >= 0) {

        sprintf(ret + strlen(ret), ",pos:%d", afl->stage_cur_byte);

        if (afl->stage_val_type != STAGE_VAL_NONE) {

          sprintf(ret + strlen(ret), ",val:%s%+d",
                  (afl->stage_val_type == STAGE_VAL_BE) ? "be:" : "",
                  afl->stage_cur_val);

        }

      } else {

        sprintf(ret + strlen(ret), ",rep:%d", afl->stage_cur_val);

      }

    }

  }

  if (new_bits == 2) { strcat(ret, ",+cov"); }
  else if (new_bits == 0 && new_partition) { strcat(ret, "+partition"); }

  if (unlikely(strlen(ret) >= max_description_len))
    FATAL("describe string is too long");

  return ret;

}

#endif                                                     /* !SIMPLE_FILES */

/* Write a message accompanying the crash directory :-) */

void write_crash_readme(afl_state_t *afl) {

  u8    fn[PATH_MAX];
  s32   fd;
  FILE *f;

  u8 val_buf[STRINGIFY_VAL_SIZE_MAX];

  sprintf(fn, "%s/crashes/README.txt", afl->out_dir);

  fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);

  /* Do not die on errors here - that would be impolite. */

  if (unlikely(fd < 0)) { return; }

  f = fdopen(fd, "w");

  if (unlikely(!f)) {

    close(fd);
    return;

  }

  fprintf(
      f,
      "Command line used to find this crash:\n\n"

      "%s\n\n"

      "If you can't reproduce a bug outside of afl-fuzz, be sure to set the "
      "same\n"
      "memory limit. The limit used for this fuzzing session was %s.\n\n"

      "Need a tool to minimize test cases before investigating the crashes or "
      "sending\n"
      "them to a vendor? Check out the afl-tmin that comes with the fuzzer!\n\n"

      "Found any cool bugs in open-source tools using afl-fuzz? If yes, please "
      "drop\n"
      "an mail at <afl-users@googlegroups.com> once the issues are fixed\n\n"

      "  https://github.com/AFLplusplus/AFLplusplus\n\n",

      afl->orig_cmdline,
      stringify_mem_size(val_buf, sizeof(val_buf),
                         afl->fsrv.mem_limit << 20));      /* ignore errors */

  fclose(f);

}


// Return the number of partitions found for this checksum before this one
s8 check_if_new_partition(u64 checksum, u8 partition) {
  u32 partitionBitmap = 1 << partition;

  struct path_partitions sought = { .checksum = checksum };
  struct path_partitions *found = hashmap_get(hashfuzzFoundPartitions, &sought);

  if (found) {
    if (found->foundPartitions & partitionBitmap) {
      // printf("Found identical partition %03hhu for checksum %020llu\n", partition, checksum);
      // We've already found an input in this partition
      return -1;
    } else {
      // This input discovers a new partition for this path
      u8 foundAlready = found->foundPartitionsCount;
      printf("Found new partition %03hhu for checksum %020llu\n", partition, checksum);

      found->foundPartitions |= partitionBitmap;
      found->foundPartitionsCount++;
      return foundAlready;
    }
  }

  printf("Found checksum %020llu with partition %03hhu, hashmap count: %lu\n", checksum, partition, hashmap_count(hashfuzzFoundPartitions));
  sought.foundPartitions = partitionBitmap;
  sought.foundPartitionsCount = 1;
  hashmap_set(hashfuzzFoundPartitions, &sought);

  return 0;
}


/* Check if the result of an execve() during routine fuzzing is interesting,
   save or queue the input test case for further analysis if so. Returns 1 if
   entry is saved, 0 otherwise. */

u8 __attribute__((hot))
save_if_interesting(afl_state_t *afl, void *mem, u32 len, u8 fault) {
  if (unlikely(len == 0)) { return 0; }

  u8 *queue_fn = "";
  u8  new_bits = '\0';
  s8  new_partition = 0;
  s32 fd;
  u8  keeping = 0, res, classified = 0;
  u64 cksum = 0;

  u8 fn[PATH_MAX];

  /* Update path frequency. */

  /* Generating a hash on every input is super expensive. Bad idea and should
     only be used for special schedules */
  if (unlikely(afl->schedule >= FAST && afl->schedule <= RARE)) {

    cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

    /* Saturated increment */
    if (afl->n_fuzz[cksum % N_FUZZ_SIZE] < 0xFFFFFFFF)
      afl->n_fuzz[cksum % N_FUZZ_SIZE]++;

  }

  if (likely(fault == afl->crash_mode)) {
    u8 interesting = 0;
    u8 hashfuzzClass = 0;

    /* Keep only if there are new bits in the map, add to queue for
       future fuzzing, etc. */

    new_bits = has_new_bits_unclassified(afl, afl->virgin_bits);
    interesting = new_bits;

    if (likely(afl->ncd_based_queue)) {
      if (new_bits)
        afl->discovering_q_entries++;

      cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      struct queue_entry new = {
          .testcase_buf = mem,
          .len = len,
          .exec_cksum = cksum,
      };

      save_to_edge_entries(afl, &new, new_bits);
    }

    if (afl->hashfuzz_enabled) {

      if (afl->hashfuzz_is_input_based) {
        hashfuzzClass = hashfuzzClassify(mem, len, afl->hashfuzz_partitions);
      } else {
        hashfuzzClass = afl->fsrv.last_run_output_hash_class;
      }

      if (afl->hashfuzz_mimic_transformation) {

        u64 partitionBit = 1 << hashfuzzClass;
        if (!(partitionBit & afl->hashfuzz_discovered_partitions)) {
          // enable this seed if it's the first one for that partition
          printf("Adding (and enabling) first seed for partition %hhu\n", hashfuzzClass);
          afl->hashfuzz_discovered_partitions |= partitionBit;
          interesting = true;
        }

      } else {  // Using new queue swapping method (performance penalty)
      
        cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

        struct path_partitions sought = { .checksum = cksum };
        struct path_partitions *found = hashmap_get(hashfuzzFoundPartitions, &sought);

        if (interesting || found) {
          // Find out if we have a matching path with this hashfuzz classification
          // IMPORTANT: this needs calling even for new inputs 
          //            (to build the map of covered partitions)
          new_partition = check_if_new_partition(cksum, hashfuzzClass);

          interesting = interesting || (new_partition >= 0); // We don't have this one in the queue yet
        }

      }

    }

    if (likely(!interesting)) {

      if (unlikely(afl->crash_mode)) { ++afl->total_crashes; }
      return 0;

    }

    classified = new_bits;


#ifndef SIMPLE_FILES

    if (unlikely(!afl->ncd_based_queue)) {
      queue_fn = alloc_printf("%s/queue/id:%06u,cksum:%020llu,%s", afl->out_dir,
                              afl->queued_paths, cksum,
                              describe_op(afl, new_bits, new_partition >= 0,NAME_MAX - strlen("id:000000,")));
    }

#else

    queue_fn =
        alloc_printf("%s/queue/id_%06u", afl->out_dir, afl->queued_paths);

#endif                                                    /* ^!SIMPLE_FILES */

    if (unlikely(!afl->ncd_based_queue)) {
#ifdef NOISY
      printf("Writing to NEW file\n");
#endif
      fd = open(queue_fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
      if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", queue_fn); }
      ck_write(fd, mem, len, queue_fn);
      close(fd);
      add_to_queue(afl, queue_fn, len, 0, hashfuzzClass, cksum, new_partition);
    }

#ifdef INTROSPECTION
    if (afl->custom_mutators_count && afl->current_custom_fuzz) {

      LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

        if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

          const char *ptr = el->afl_custom_introspection(el->data);

          if (ptr != NULL && *ptr != 0) {

            fprintf(afl->introspection_file, "QUEUE CUSTOM %s = %s\n", ptr,
                    afl->queue_top->fname);

          }

        }

      });

    } else if (afl->mutation[0] != 0) {

      fprintf(afl->introspection_file, "QUEUE %s = %s\n", afl->mutation,
              afl->queue_top->fname);

    }

#endif

    if (new_bits == 2) {

      afl->queue_top->has_new_cov = 1;
      ++afl->queued_with_cov;

    }

    if (unlikely((!afl->ncd_based_queue && !afl->hashfuzz_enabled) || afl->hashfuzz_mimic_transformation)) {

      /* AFLFast schedule? update the new queue entry */
      if (cksum) {

        afl->queue_top->n_fuzz_entry = cksum % N_FUZZ_SIZE;
        afl->n_fuzz[afl->queue_top->n_fuzz_entry] = 1;

      }

      /* due to classify counts we have to recalculate the checksum */
      cksum = afl->queue_top->exec_cksum =
          hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

    }

    if (unlikely(!afl->ncd_based_queue)) {
      /* Try to calibrate inline; this also calls update_bitmap_score() when
         successful. */

      res = calibrate_case(afl, afl->queue_top, mem, afl->queue_cycle - 1, 0);
    }

    if (unlikely(res == FSRV_RUN_ERROR)) {

      FATAL("Unable to execute target application");

    }

    if (unlikely(!afl->ncd_based_queue)) {
      if (likely(afl->q_testcase_max_cache_size)) {
        queue_testcase_store_mem(afl, afl->queue_top, mem);
      }
    }

    keeping = 1;

  }

  switch (fault) {

    case FSRV_RUN_TMOUT:

      /* Timeouts are not very interesting, but we're still obliged to keep
         a handful of samples. We use the presence of new bits in the
         hang-specific bitmap as a signal of uniqueness. In "non-instrumented"
         mode, we just keep everything. */

      ++afl->total_tmouts;

      if (afl->unique_hangs >= KEEP_UNIQUE_HANG) { return keeping; }

      if (likely(!afl->non_instrumented_mode)) {

        if (!classified) {

          classify_counts(&afl->fsrv);
          classified = 1;

        }

        simplify_trace(afl, afl->fsrv.trace_bits);

        if (!has_new_bits(afl, afl->virgin_tmout)) { return keeping; }

      }

      ++afl->unique_tmouts;
#ifdef INTROSPECTION
      if (afl->custom_mutators_count && afl->current_custom_fuzz) {

        LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

          if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

            const char *ptr = el->afl_custom_introspection(el->data);

            if (ptr != NULL && *ptr != 0) {

              fprintf(afl->introspection_file,
                      "UNIQUE_TIMEOUT CUSTOM %s = %s\n", ptr,
                      afl->queue_top->fname);

            }

          }

        });

      } else if (afl->mutation[0] != 0) {

        fprintf(afl->introspection_file, "UNIQUE_TIMEOUT %s\n", afl->mutation);

      }

#endif

      /* Before saving, we make sure that it's a genuine hang by re-running
         the target with a more generous timeout (unless the default timeout
         is already generous). */

      if (afl->fsrv.exec_tmout < afl->hang_tmout) {

        u8 new_fault;
        write_to_testcase(afl, mem, len);
        new_fault = fuzz_run_target(afl, &afl->fsrv, afl->hang_tmout);
        classify_counts(&afl->fsrv);

        /* A corner case that one user reported bumping into: increasing the
           timeout actually uncovers a crash. Make sure we don't discard it if
           so. */

        if (!afl->stop_soon && new_fault == FSRV_RUN_CRASH) {

          goto keep_as_crash;

        }

        if (afl->stop_soon || new_fault != FSRV_RUN_TMOUT) { return keeping; }

      }

#ifndef SIMPLE_FILES

      snprintf(fn, PATH_MAX, "%s/hangs/id:%06llu,%s", afl->out_dir,
               afl->unique_hangs,
               describe_op(afl, 0, 0, NAME_MAX - strlen("id:000000,")));

#else

      snprintf(fn, PATH_MAX, "%s/hangs/id_%06llu", afl->out_dir,
               afl->unique_hangs);

#endif                                                    /* ^!SIMPLE_FILES */

      ++afl->unique_hangs;

      afl->last_hang_time = get_cur_time();

      break;

    case FSRV_RUN_CRASH:

    keep_as_crash:

      /* This is handled in a manner roughly similar to timeouts,
         except for slightly different limits and no need to re-run test
         cases. */

      ++afl->total_crashes;

      if (afl->unique_crashes >= KEEP_UNIQUE_CRASH) { return keeping; }

      if (likely(!afl->non_instrumented_mode)) {

        if (!classified) { classify_counts(&afl->fsrv); }

        simplify_trace(afl, afl->fsrv.trace_bits);

        if (!has_new_bits(afl, afl->virgin_crash)) { return keeping; }

      }

      if (unlikely(!afl->unique_crashes)) { write_crash_readme(afl); }

#ifndef SIMPLE_FILES

      snprintf(fn, PATH_MAX, "%s/crashes/id:%06llu,sig:%02u,%s", afl->out_dir,
               afl->unique_crashes, afl->fsrv.last_kill_signal,
               describe_op(afl, 0, 0, NAME_MAX - strlen("id:000000,sig:00,")));

#else

      snprintf(fn, PATH_MAX, "%s/crashes/id_%06llu_%02u", afl->out_dir,
               afl->unique_crashes, afl->last_kill_signal);

#endif                                                    /* ^!SIMPLE_FILES */

      ++afl->unique_crashes;
#ifdef INTROSPECTION
      if (afl->custom_mutators_count && afl->current_custom_fuzz) {

        LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

          if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

            const char *ptr = el->afl_custom_introspection(el->data);

            if (ptr != NULL && *ptr != 0) {

              fprintf(afl->introspection_file, "UNIQUE_CRASH CUSTOM %s = %s\n",
                      ptr, afl->queue_top->fname);

            }

          }

        });

      } else if (afl->mutation[0] != 0) {

        fprintf(afl->introspection_file, "UNIQUE_CRASH %s\n", afl->mutation);

      }

#endif
      if (unlikely(afl->infoexec)) {

        // if the user wants to be informed on new crashes - do that
#if !TARGET_OS_IPHONE
        // we dont care if system errors, but we dont want a
        // compiler warning either
        // See
        // https://stackoverflow.com/questions/11888594/ignoring-return-values-in-c
        (void)(system(afl->infoexec) + 1);
#else
        WARNF("command execution unsupported");
#endif

      }

      afl->last_crash_time = get_cur_time();
      afl->last_crash_execs = afl->fsrv.total_execs;

      break;

    case FSRV_RUN_ERROR:
      FATAL("Unable to execute target application");

    default:
      return keeping;

  }

  /* If we're here, we apparently want to save the crash or hang
     test case, too. */

  fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
  if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", fn); }
  ck_write(fd, mem, len, fn);
  close(fd);

  return keeping;

}

