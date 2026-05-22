/*  Copyright (C) 2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

/*  Defragmenter.  Repacks every file and subdirectory into contiguous low
    clusters, depth-first and alphabetical at each directory level, then
    zeroes every free cluster.  Bad clusters are preserved in place.

    The data move is a plan + bulk-copy: pass 1 assigns new positions,
    pass 2 rewrites directory entries, pass 3 reads the whole data area
    into RAM, rearranges it and writes it back in track-sized chunks.  */

#include "fdchk.h"

typedef struct {
  BYTE * fat;          /*  original FAT  */
  BYTE * new_fat;      /*  FAT being built  */
  BYTE * fixed;        /*  [n_total]: clusters that must not move (bad)  */
  WORD * plan;         /*  plan[T]    = original cluster now placed at T  */
  WORD * new_pos;      /*  new_pos[O] = new cluster for original O  */
  BYTE * src;          /*  in-memory image of the whole data area  */
  int    cluster_bytes;
  int    n_total;
  int    next_target;  /*  next eligible (non-fixed) cluster to assign  */
  BYTE * root_buf;     /*  root directory image, modified in place  */
  int    root_bytes;
} DefragCtx;

/*  Allocate the next non-fixed target cluster; 0 when exhausted.  */
static int defrag_alloc(DefragCtx * c) {
  while (c->next_target < c->n_total && c->fixed[c->next_target])
    c->next_target++;
  if (c->next_target >= c->n_total) return 0;
  return c->next_target++;
}

/*  Collect a directory's live entry indices into order[], sorted by 8.3
    name; skips deleted / LFN / volume-label / '.' / empty slots.  The
    list is terminated with -1.  */
static void defrag_sort_dir(const BYTE * dir_buf, int n_entries,
                            int * order) {
  int n = 0;
  for (int i = 0; i < n_entries; ++i) {
    const FatDirEntry * e = (const FatDirEntry *) (dir_buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5 || e->attr == 0x0F || (e->attr & 0x08)) continue;
    if (e->name[0] == '.') continue;
    if (e->start_lo < 2 && e->size == 0) continue;
    order[n++] = i;
  }
  /*  Bubble sort - FAT short names are uppercase, so memcmp orders them.  */
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j) {
      const FatDirEntry * a = (const FatDirEntry *) (dir_buf + order[i] * 32);
      const FatDirEntry * b = (const FatDirEntry *) (dir_buf + order[j] * 32);
      if (memcmp(a->name, b->name, 11) > 0) {
        int t = order[i]; order[i] = order[j]; order[j] = t;
      }
    }
  order[n] = -1;
}

/*  Count the clusters in a chain (capped at cap).  */
static int defrag_count_chain(const BYTE * fat, int start, int cap) {
  int n = 0, cl = start;
  while (cl >= 2 && cl < cap && n < cap) {
    n++;
    WORD nx = fat12_get(fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
  return n;
}

/*  Copy a directory's chain out of the in-memory data image into a fresh
    contiguous buffer; caller LocalFree's.  */
static BYTE * defrag_read_subdir(DefragCtx * c, WORD start, int * out_bytes) {
  int n_clu = defrag_count_chain(c->fat, start, c->n_total);
  if (n_clu == 0) { *out_bytes = 0; return NULL; }
  int bytes = n_clu * c->cluster_bytes;
  BYTE * buf = (BYTE *) LocalAlloc(LPTR, bytes);
  if (!buf) { *out_bytes = 0; return NULL; }
  int off = 0, cl = start;
  for (int k = 0; k < n_clu; ++k) {
    memcpy(buf + off, c->src + (cl - 2) * c->cluster_bytes, c->cluster_bytes);
    off += c->cluster_bytes;
    WORD nx = fat12_get(c->fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
  *out_bytes = bytes;
  return buf;
}

/*  Store a directory's chain back into the in-memory data image, at its
    original cluster positions.  */
static void defrag_store_subdir(DefragCtx * c, WORD start, const BYTE * buf) {
  int off = 0, cl = start, safety = 0;
  while (cl >= 2 && cl < c->n_total && safety++ < c->n_total) {
    memcpy(c->src + (cl - 2) * c->cluster_bytes, buf + off, c->cluster_bytes);
    off += c->cluster_bytes;
    WORD nx = fat12_get(c->fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
}

/*  Assign new positions for a chain; returns the new start cluster and
    fills in new_fat links plus plan[] / new_pos[].  */
static WORD defrag_assign_chain(DefragCtx * c, int old_start) {
  WORD new_start = 0;
  int prev_t = 0, cl = old_start, safety = 0;
  while (cl >= 2 && cl < c->n_total && safety++ < c->n_total) {
    int t = defrag_alloc(c);
    if (!t) break;
    c->plan[t] = (WORD) cl;
    c->new_pos[cl] = (WORD) t;
    if (prev_t) fat12_set(c->new_fat, prev_t, (WORD) t);
    else        new_start = (WORD) t;
    prev_t = t;
    WORD nx = fat12_get(c->fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
  if (prev_t) fat12_set(c->new_fat, prev_t, 0xFFF);
  return new_start;
}

/*  Pass 1: depth-first, assign new positions - root files first
    (alphabetical), then each subdir (alphabetical, recursing in).  The
    directory buffers are not modified yet; that happens in pass 2.  */
static void defrag_plan_dir(DefragCtx * c, const BYTE * dir_buf,
                            int n_entries, int depth) {
  if (depth > 16) return;
  int * order = (int *) LocalAlloc(LPTR, (n_entries + 1) * sizeof(int));
  if (!order) return;
  defrag_sort_dir(dir_buf, n_entries, order);

  for (int oi = 0; order[oi] >= 0; ++oi) {
    const FatDirEntry * e = (const FatDirEntry *) (dir_buf + order[oi] * 32);
    if (e->attr & 0x10) continue;
    if (e->size == 0 || e->start_lo < 2) continue;
    defrag_assign_chain(c, e->start_lo);
  }

  for (int oi = 0; order[oi] >= 0; ++oi) {
    const FatDirEntry * e = (const FatDirEntry *) (dir_buf + order[oi] * 32);
    if (!(e->attr & 0x10)) continue;
    if (e->start_lo < 2) continue;
    defrag_assign_chain(c, e->start_lo);
    int sub_bytes = 0;
    BYTE * sub = defrag_read_subdir(c, e->start_lo, &sub_bytes);
    if (sub) {
      defrag_plan_dir(c, sub, sub_bytes / 32, depth + 1);
      LocalFree(sub);
    }
  }
  LocalFree(order);
}

/*  Pass 2: rewrite every directory entry's start_lo from new_pos[],
    including '.' and '..'.  Subdirs are written back to their OLD disk
    positions so the bulk read picks up the updated content; the root
    stays in c->root_buf and is committed at the end.  */
static void defrag_update_dir(DefragCtx * c, WORD start_cluster,
                              WORD self_new, WORD parent_new, int depth) {
  if (depth > 16) return;
  int is_root = (start_cluster == 0);

  BYTE * dir_buf;
  int dir_bytes, n_entries;
  if (is_root) {
    dir_buf   = c->root_buf;
    dir_bytes = c->root_bytes;
    n_entries = G.root_entries;
  } else {
    dir_buf = defrag_read_subdir(c, start_cluster, &dir_bytes);
    if (!dir_buf) return;
    n_entries = dir_bytes / 32;
  }

  /*  Recurse first, using the OLD start_lo values still in the buffer.  */
  for (int i = 0; i < n_entries; ++i) {
    FatDirEntry * e = (FatDirEntry *) (dir_buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5 || e->attr == 0x0F || (e->attr & 0x08)) continue;
    if (!(e->attr & 0x10)) continue;
    if (e->name[0] == '.') continue;
    if (e->start_lo < 2) continue;
    defrag_update_dir(c, e->start_lo, c->new_pos[e->start_lo],
                      self_new, depth + 1);
  }

  /*  Now update every entry's start_lo.  */
  for (int i = 0; i < n_entries; ++i) {
    FatDirEntry * e = (FatDirEntry *) (dir_buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5 || e->attr == 0x0F) continue;
    if (e->attr & 0x08) continue;             /*  volume label, untouched  */

    if (!is_root && c0 == '.') {
      e->start_lo = ((BYTE) e->name[1] == '.') ? parent_new : self_new;
      continue;
    }
    if (e->start_lo >= 2 && e->start_lo < c->n_total &&
        c->new_pos[e->start_lo] != 0)
      e->start_lo = c->new_pos[e->start_lo];
  }

  if (!is_root) {
    defrag_store_subdir(c, start_cluster, dir_buf);
    LocalFree(dir_buf);
  }
}

/*  The defragmenter proper.  It is transactional: the whole data area is
    read up front, and a single unreadable sector aborts the run before
    anything is written - the disk is left untouched.  Pass 1 and pass 2
    work only on the in-memory image; nothing reaches the disk until the
    commit phase, after which a write error is reported as a failure.  */
static int defrag_worker(DiskHandle * dh, int * moved_out, int * zeroed_out) {
  int rc = 1;     /*  1 aborted (disk untouched); 0 ok; -1 write failed  */
  int fat_bytes      = G.fat_size * SECTOR_SIZE;
  int root_sec_count = (G.root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
  int root_bytes     = root_sec_count * SECTOR_SIZE;
  int n_total        = G.total_clusters + 2;
  int cluster_bytes  = G.sec_per_cluster * SECTOR_SIZE;
  int data_start_sec = G.data_start_sec;
  int data_sec_count = G.total_sec - data_start_sec;
  int data_bytes     = data_sec_count * SECTOR_SIZE;
  const WORD chunk   = 18;                    /*  one 1.44 MB track  */

  BYTE * fat     = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  BYTE * new_fat = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  BYTE * root    = (BYTE *) LocalAlloc(LPTR, root_bytes);
  BYTE * src     = (BYTE *) LocalAlloc(LPTR, data_bytes);   /*  ~1.4 MB  */
  BYTE * dst     = (BYTE *) LocalAlloc(LPTR, data_bytes);   /*  ~1.4 MB  */
  WORD * plan    = (WORD *) LocalAlloc(LPTR, n_total * sizeof(WORD));
  WORD * new_pos = (WORD *) LocalAlloc(LPTR, n_total * sizeof(WORD));
  BYTE * fixed   = (BYTE *) LocalAlloc(LPTR, n_total);
  int n_moved = 0, n_zeroed = 0;

  if (!fat || !new_fat || !root || !src || !dst ||
      !plan || !new_pos || !fixed) goto done;

  ui_status("Defrag: reading FAT...");
  if (disk_read(dh, (DWORD) G.reserved_sec, (WORD) G.fat_size, fat) != 0)
    goto done;
  ui_status("Defrag: reading root directory...");
  if (disk_read(dh, (DWORD) (G.reserved_sec + G.num_fats * G.fat_size),
                (WORD) root_sec_count, root) != 0) goto done;

  /*  Read the entire data area first.  A single unreadable sector aborts
      here, before a byte is written - defragging a disk we cannot fully
      read would scramble every file.  */
  ui_status("Defrag: reading data area...");
  for (DWORD lba = (DWORD) data_start_sec; lba < (DWORD) G.total_sec; ) {
    if (G.abort_req) goto done;
    WORD c = chunk;
    if (lba + c > (DWORD) G.total_sec) c = (WORD) (G.total_sec - lba);
    for (WORD i = 0; i < c; ++i) ui_set_state((int) lba + i, ST_SCANNING);
    if (disk_read(dh, lba, c,
                  src + (lba - data_start_sec) * SECTOR_SIZE) != 0) {
      ui_status("Defrag aborted - disk has unreadable sectors; "
                "nothing was changed.");
      goto done;
    }
    G.current_sec = (int) lba;
    G.scanned = (DWORD) (lba - data_start_sec);
    PostMessageA(G.hMain, WM_APP_PROGRESS,
                 (WPARAM) G.scanned, (LPARAM) (2 * data_sec_count));
    lba += c;
  }

  /*  Bad clusters and the reserved FAT[0]/FAT[1] are pinned; the new FAT
      starts with the media-byte entries and the bad-cluster markers.  */
  fixed[0] = fixed[1] = 1;
  for (int n = 2; n < n_total; ++n)
    if (fat12_get(fat, n) == 0xFF7) fixed[n] = 1;
  new_fat[0] = fat[0]; new_fat[1] = fat[1]; new_fat[2] = fat[2];
  for (int n = 2; n < n_total; ++n)
    if (fixed[n]) fat12_set(new_fat, n, 0xFF7);

  DefragCtx ctx;
  memzero(&ctx, sizeof ctx);
  ctx.fat = fat;          ctx.new_fat = new_fat;  ctx.fixed = fixed;
  ctx.plan = plan;        ctx.new_pos = new_pos;  ctx.src = src;
  ctx.cluster_bytes = cluster_bytes;
  ctx.n_total = n_total;  ctx.next_target = 2;
  ctx.root_buf = root;    ctx.root_bytes = root_bytes;

  /*  Pass 1 + 2 plan the move and rewrite directory entries entirely in
      the in-memory image (src / root) - no disk writes yet.  */
  ui_status("Defrag: planning new layout...");
  defrag_plan_dir(&ctx, root, G.root_entries, 0);
  if (G.abort_req) goto done;
  ui_status("Defrag: updating directory entries...");
  defrag_update_dir(&ctx, 0, 0, 0, 0);
  if (G.abort_req) goto done;

  /*  Rearrange src -> dst.  dst was zeroed by LPTR, so a cluster the plan
      never touches stays zero - the free-cluster wipe for free.  */
  for (int t = 2; t < n_total; ++t) {
    int dst_off = (t - 2) * cluster_bytes;
    if (fixed[t]) {
      memcpy(dst + dst_off, src + dst_off, cluster_bytes);
      continue;
    }
    WORD src_cl = plan[t];
    if (src_cl >= 2 && src_cl < n_total) {
      memcpy(dst + dst_off, src + (src_cl - 2) * cluster_bytes,
             cluster_bytes);
      if (src_cl != t) n_moved++;
    } else {
      n_zeroed++;                             /*  no source: stays zero  */
    }
  }

  /*  Commit: data area, then both FATs, then the root.  Past this point a
      write error leaves the disk inconsistent (rc = -1), but the up-front
      read pass makes a sudden write failure unlikely.  */
  rc = -1;
  ui_status("Defrag: writing data area...");
  for (DWORD lba = (DWORD) data_start_sec; lba < (DWORD) G.total_sec; ) {
    WORD c = chunk;
    if (lba + c > (DWORD) G.total_sec) c = (WORD) (G.total_sec - lba);
    for (WORD i = 0; i < c; ++i) ui_set_state((int) lba + i, ST_WRITING);
    if (disk_write(dh, lba, c,
                   dst + (lba - data_start_sec) * SECTOR_SIZE) != 0)
      goto done;
    /*  Settle each sector into its final colour.  */
    for (WORD i = 0; i < c; ++i) {
      int cl = lba_to_cluster(lba + i);
      BYTE st;
      if (cl < 2 || cl >= n_total) {
        st = ST_SYSTEM;
      } else {
        WORD v = fat12_get(new_fat, cl);
        st = v == 0xFF7 ? ST_BAD_OLD : v == 0 ? ST_UNTESTED : ST_GOOD;
      }
      ui_set_state((int) lba + i, st);
    }
    G.current_sec = (int) lba;
    G.scanned = (DWORD) data_sec_count + (DWORD) (lba - data_start_sec);
    PostMessageA(G.hMain, WM_APP_PROGRESS,
                 (WPARAM) G.scanned, (LPARAM) (2 * data_sec_count));
    lba += c;
  }

  ui_status("Defrag: writing FAT...");
  for (int f = 0; f < G.num_fats; ++f)
    if (disk_write(dh, (DWORD) (G.reserved_sec + f * G.fat_size),
                   (WORD) G.fat_size, new_fat) != 0) goto done;
  ui_status("Defrag: writing root directory...");
  if (disk_write(dh, (DWORD) (G.reserved_sec + G.num_fats * G.fat_size),
                 (WORD) root_sec_count, root) != 0) goto done;

  if (moved_out)  *moved_out  = n_moved;
  if (zeroed_out) *zeroed_out = n_zeroed;
  rc = 0;

done:
  if (fat)     LocalFree(fat);
  if (new_fat) LocalFree(new_fat);
  if (root)    LocalFree(root);
  if (src)     LocalFree(src);
  if (dst)     LocalFree(dst);
  if (plan)    LocalFree(plan);
  if (new_pos) LocalFree(new_pos);
  if (fixed)   LocalFree(fixed);
  return rc;
}

/*  Worker-thread entry point for defrag.  Mirrors worker_proc: open and
    lock the disk, run defrag_worker, post WM_APP_DONE.  Results land in
    the G.defrag_* fields for on_done to report.  */
DWORD WINAPI defrag_thread_proc(LPVOID arg) {
  (void) arg;
  DiskHandle dh;
  G.defrag_rc = -1;
  G.defrag_moved = 0;
  G.defrag_zeroed = 0;

  if (!disk_open(&dh, G.drive)) goto done;

  if (probe_disk_present(&dh) == 0) {
    ui_status("Defrag aborted - no disk in drive.");
    disk_close(&dh);
    goto done;
  }

  /*  Defrag needs a valid FAT12 filesystem.  */
  BYTE bpb[SECTOR_SIZE];
  if (disk_read(&dh, 0, 1, bpb) != 0 || !parse_bpb(bpb)) {
    ui_status("Defrag aborted - no valid FAT12 filesystem.");
    disk_close(&dh);
    goto done;
  }
  G.has_fat = 1;
  SendMessageA(G.hStatus, SB_SETTEXTA, 1, (LPARAM) G.fs_type);

  /*  Full exclusive lock: level 0 first, then escalate.  */
  if (disk_lock(&dh, 3) < 0) {
    ui_status("Defrag aborted - cannot lock drive.");
    disk_close(&dh);
    goto done;
  }

  G.defrag_rc = defrag_worker(&dh, &G.defrag_moved, &G.defrag_zeroed);
  disk_unlock(&dh);
  disk_close(&dh);

done:
  InterlockedExchange(&G.running, 0);
  PostMessageA(G.hMain, WM_APP_DONE, 0, 0);
  return 0;
}
