/*  Copyright (C) 2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

/*  FAT12 defragmentation.  */

#include "fdchk.h"

typedef struct {
  BYTE * fat;
  BYTE * new_fat;
  BYTE * fixed;        /*  Bad clusters do not move.  */
  WORD * plan;         /*  plan[target] = source.  */
  WORD * new_pos;      /*  new_pos[source] = target.  */
  BYTE * src;
  int    cluster_bytes;
  int    n_total;
  int    next_target;
  BYTE * root_buf;
  int    root_bytes;
} defrag_ctx;

/*  Return the next movable target, or zero.  */
static int alloc_target(defrag_ctx * c) {
  while (c->next_target < c->n_total && c->fixed[c->next_target])
    c->next_target++;
  if (c->next_target >= c->n_total) return 0;
  return c->next_target++;
}

/*  Sort live directory entries by 8.3 name.  */
static void sort_dir(const BYTE * dir_buf, int n_entries, int * order) {
  int i, j, n = 0;
  Fi(n_entries,
    const fat_dirent * e = (const fat_dirent *) (dir_buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5 || e->attr == 0x0F || (e->attr & 0x08)) continue;
    if (e->name[0] == '.') continue;
    if (e->start_lo < 2 && e->size == 0) continue;
    order[n++] = i;
  );
  Fi(n,
    for (j = i + 1; j < n; j++) {
      const fat_dirent * a = (const fat_dirent *) (dir_buf + order[i] * 32);
      const fat_dirent * b = (const fat_dirent *) (dir_buf + order[j] * 32);
      if (memcmp(a->name, b->name, 11) > 0) {
        int t = order[i];  order[i] = order[j];  order[j] = t;
      }
    });
  order[n] = -1;
}

static int chain_len(const BYTE * fat, int start, int cap) {
  int n = 0, cl = start;
  while (cl >= 2 && cl < cap && n < cap) {
    n++;
    WORD nx = fat12_get(fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
  return n;
}

/*  Read a directory chain.  */
static BYTE * read_subdir(defrag_ctx * c, WORD start, int * out_bytes) {
  int n_clu = chain_len(c->fat, start, c->n_total);
  if (n_clu == 0) { *out_bytes = 0;  return NULL; }
  int bytes = n_clu * c->cluster_bytes;
  BYTE * buf = (BYTE *) LocalAlloc(LPTR, bytes);
  if (!buf) { *out_bytes = 0;  return NULL; }
  int k, off = 0, cl = start;
  Fk(n_clu,
    memcpy(buf + off, c->src + (cl - 2) * c->cluster_bytes, c->cluster_bytes);
    off += c->cluster_bytes;
    WORD nx = fat12_get(c->fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  );
  *out_bytes = bytes;  return buf;
}

/*  Write a directory back to its old clusters.  */
static void store_subdir(defrag_ctx * c, WORD start, const BYTE * buf) {
  int off = 0, cl = start, safety = 0;
  while (cl >= 2 && cl < c->n_total && safety++ < c->n_total) {
    memcpy(c->src + (cl - 2) * c->cluster_bytes, buf + off, c->cluster_bytes);
    off += c->cluster_bytes;
    WORD nx = fat12_get(c->fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
}

/*  Assign a chain and return its new start.  */
static WORD assign_chain(defrag_ctx * c, int old_start) {
  WORD new_start = 0;
  int prev_t = 0, cl = old_start, safety = 0;
  while (cl >= 2 && cl < c->n_total && safety++ < c->n_total) {
    int t = alloc_target(c);
    if (!t) break;
    c->plan[t] = (WORD) cl;  c->new_pos[cl] = (WORD) t;
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

/*  Assign files, then subdirectories, depth-first.  */
static void plan_dir(defrag_ctx * c, const BYTE * dir_buf,
                     int n_entries, int depth) {
  int oi;
  if (depth > 16) return;
  int * order = (int *) LocalAlloc(LPTR, (n_entries + 1) * sizeof(int));
  if (!order) return;
  sort_dir(dir_buf, n_entries, order);
  for (oi = 0; order[oi] >= 0; oi++) {
    const fat_dirent * e = (const fat_dirent *) (dir_buf + order[oi] * 32);
    if (e->attr & 0x10) continue;
    if (e->size == 0 || e->start_lo < 2) continue;
    assign_chain(c, e->start_lo);
  }
  for (oi = 0; order[oi] >= 0; oi++) {
    const fat_dirent * e = (const fat_dirent *) (dir_buf + order[oi] * 32);
    if (!(e->attr & 0x10)) continue;
    if (e->start_lo < 2) continue;
    assign_chain(c, e->start_lo);
    int sub_bytes = 0;
    BYTE * sub = read_subdir(c, e->start_lo, &sub_bytes);
    if (sub) {
      plan_dir(c, sub, sub_bytes / 32, depth + 1);
      LocalFree(sub);
    }
  }
  LocalFree(order);
}

/*  Rewrite directory starts, including '.' and '..'.  */
static void update_dir(defrag_ctx * c, WORD start_cluster,
                       WORD self_new, WORD parent_new, int depth) {
  if (depth > 16) return;
  int is_root = start_cluster == 0;
  BYTE * dir_buf;
  int i, dir_bytes, n_entries;
  if (is_root) {
    dir_buf   = c->root_buf;
    dir_bytes = c->root_bytes;
    n_entries = G.root_entries;
  } else {
    dir_buf = read_subdir(c, start_cluster, &dir_bytes);
    if (!dir_buf) return;
    n_entries = dir_bytes / 32;
  }
  /*  Recurse while entries still hold old starts.  */
  Fi(n_entries,
    fat_dirent * e = (fat_dirent *) (dir_buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5 || e->attr == 0x0F || (e->attr & 0x08)) continue;
    if (!(e->attr & 0x10)) continue;
    if (e->name[0] == '.') continue;
    if (e->start_lo < 2 || e->start_lo >= c->n_total) continue;
    update_dir(c, e->start_lo, c->new_pos[e->start_lo], self_new, depth + 1);
  );
  /*  Then replace old starts.  */
  Fi(n_entries,
    fat_dirent * e = (fat_dirent *) (dir_buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5 || e->attr == 0x0F) continue;
    if (e->attr & 0x08) continue;
    if (!is_root && c0 == '.') {
      e->start_lo = ((BYTE) e->name[1] == '.') ? parent_new : self_new;
      continue;
    }
    if (e->start_lo >= 2 && e->start_lo < c->n_total &&
        c->new_pos[e->start_lo] != 0)
      e->start_lo = c->new_pos[e->start_lo];
  );

  if (!is_root) {
    store_subdir(c, start_cluster, dir_buf);
    LocalFree(dir_buf);
  }
}

/*  Build the full output before writing anything.  */
static int run_defrag(disk_handle * dh, int * moved_out, int * zeroed_out) {
  int i, n, t, rc = 1;  /*  1 stopped, 0 done, -1 write failure.  */
  DWORD lba;
  int fat_bytes      = G.fat_size * SECTOR_SIZE;
  int root_sec_count = (G.root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
  int root_bytes     = root_sec_count * SECTOR_SIZE;
  int n_total        = G.total_clusters + 2;
  int cluster_bytes  = G.sec_per_cluster * SECTOR_SIZE;
  int data_start_sec = G.data_start_sec;
  int data_sec_count = G.total_sec - data_start_sec;
  int data_bytes     = data_sec_count * SECTOR_SIZE;
  const WORD chunk   = (WORD) (G.spt > 0 ? G.spt : 18);
  BYTE * fat     = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  BYTE * new_fat = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  BYTE * root    = (BYTE *) LocalAlloc(LPTR, root_bytes);
  BYTE * src     = (BYTE *) LocalAlloc(LPTR, data_bytes);
  BYTE * dst     = (BYTE *) LocalAlloc(LPTR, data_bytes);
  WORD * plan    = (WORD *) LocalAlloc(LPTR, n_total * sizeof(WORD));
  WORD * new_pos = (WORD *) LocalAlloc(LPTR, n_total * sizeof(WORD));
  BYTE * fixed   = (BYTE *) LocalAlloc(LPTR, n_total);
  int n_moved = 0, n_zeroed = 0;
  if (!fat || !new_fat || !root || !src || !dst ||
      !plan || !new_pos || !fixed) goto done;
  ui_status("Reading FAT...");
  if (disk_read(dh, (DWORD) G.reserved_sec, (WORD) G.fat_size, fat) != 0)
    goto done;
  ui_status("Reading root directory...");
  if (disk_read(dh, (DWORD) (G.reserved_sec + G.num_fats * G.fat_size),
                (WORD) root_sec_count, root) != 0) goto done;
  ui_status("Reading data area...");
  for (lba = (DWORD) data_start_sec; lba < (DWORD) G.total_sec; ) {
    if (G.abort_req) goto done;
    WORD c = chunk;
    if (lba + c > (DWORD) G.total_sec) c = (WORD) (G.total_sec - lba);
    Fi(c, ui_set_state((int) lba + i, ST_SCANNING));
    if (disk_read(dh, lba, c,
                  src + (lba - data_start_sec) * SECTOR_SIZE) != 0) {
      ui_status("Cannot read a sector. fdchk did not write any data.");
      goto done;
    }
    G.current_sec = (int) lba;
    G.scanned = (DWORD) (lba - data_start_sec);
    PostMessageA(G.main, WM_APP_PROGRESS,
                 (WPARAM) G.scanned, (LPARAM) (2 * data_sec_count));
    lba += c;
  }
  /*  Do not move bad or reserved clusters.  */
  fixed[0] = fixed[1] = 1;
  for (n = 2; n < n_total; n++)
    if (fat12_get(fat, n) == 0xFF7) fixed[n] = 1;
  new_fat[0] = fat[0];  new_fat[1] = fat[1];  new_fat[2] = fat[2];
  for (n = 2; n < n_total; n++)
    if (fixed[n]) fat12_set(new_fat, n, 0xFF7);
  defrag_ctx ctx;
  memzero(&ctx, sizeof ctx);
  ctx.fat = fat;          ctx.new_fat = new_fat;  ctx.fixed = fixed;
  ctx.plan = plan;        ctx.new_pos = new_pos;  ctx.src = src;
  ctx.cluster_bytes = cluster_bytes;
  ctx.n_total = n_total;  ctx.next_target = 2;
  ctx.root_buf = root;    ctx.root_bytes = root_bytes;
  ui_status("Finding new cluster positions...");
  plan_dir(&ctx, root, G.root_entries, 0);
  if (G.abort_req) goto done;
  ui_status("Updating directories...");
  update_dir(&ctx, 0, 0, 0, 0);
  if (G.abort_req) goto done;
  for (t = 2; t < n_total; t++) {
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
    } else n_zeroed++;
  }
  rc = -1;
  ui_status("Writing data area...");
  for (lba = (DWORD) data_start_sec; lba < (DWORD) G.total_sec; ) {
    WORD c = chunk;
    if (lba + c > (DWORD) G.total_sec) c = (WORD) (G.total_sec - lba);
    Fi(c, ui_set_state((int) lba + i, ST_WRITING));
    if (disk_write(dh, lba, c,
                   dst + (lba - data_start_sec) * SECTOR_SIZE) != 0)
      goto done;
    /*  Paint the final allocation.  */
    Fi(c,
      int cl = lba_to_cluster(lba + i);
      BYTE st;
      if (cl < 2 || cl >= n_total) st = ST_SYSTEM;  else {
        WORD v = fat12_get(new_fat, cl);
        st = v == 0xFF7 ? ST_BAD_OLD : v == 0 ? ST_UNTESTED : ST_GOOD;
      }
      ui_set_state((int) lba + i, st);
    );
    G.current_sec = (int) lba;
    G.scanned = (DWORD) data_sec_count + (DWORD) (lba - data_start_sec);
    PostMessageA(G.main, WM_APP_PROGRESS,
                 (WPARAM) G.scanned, (LPARAM) (2 * data_sec_count));
    lba += c;
  }
  ui_status("Writing FAT...");
  Fi(G.num_fats,
    if (disk_write(dh, (DWORD) (G.reserved_sec + i * G.fat_size),
                   (WORD) G.fat_size, new_fat) != 0) goto done;
  );
  ui_status("Writing root directory...");
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

DWORD WINAPI defrag_thread(LPVOID arg) {
  (void) arg;
  disk_handle dh;
  int lvl;
  G.defrag_rc = -1;
  G.defrag_moved = 0;
  G.defrag_zeroed = 0;
  if (!disk_open(&dh, G.drive)) goto done;
  if (probe_disk_present(&dh) == MEDIA_NONE) {
    ui_status("Defragmentation stopped: no disk.");
    disk_close(&dh);
    goto done;
  }
  BYTE bpb[SECTOR_SIZE];
  if (disk_read(&dh, 0, 1, bpb) != 0 || !parse_bpb(bpb)) {
    ui_status("Defragmentation stopped: no valid FAT12 filesystem.");
    disk_close(&dh);
    goto done;
  }
  G.has_fat = 1;
  SendMessageA(G.statusbar, SB_SETTEXTA, 1, (LPARAM) G.fs_type);
  /*  Try lock levels 2, 1 and 0.  */
  int lock_rc = -1;
  for (lvl = 2; lvl >= 0; lvl--) {
    lock_rc = disk_lock(&dh, lvl);
    if (lock_rc == 0) break;
  }
  if (lock_rc < 0) {
    ui_status("Defragmentation stopped: cannot lock the drive.");
    disk_close(&dh);
    goto done;
  }
  G.defrag_rc = run_defrag(&dh, &G.defrag_moved, &G.defrag_zeroed);
  disk_unlock(&dh);  disk_close(&dh);
done:
  InterlockedExchange(&G.running, 0);
  PostMessageA(G.main, WM_APP_DONE, 0, 0);
  return 0;
}
