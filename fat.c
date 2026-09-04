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

/*  FAT12 parsing, consistency checks, and file recovery.  */
#include "fdchk.h"

static WORD rd16(const BYTE * p) { return (WORD) (p[0] | (p[1] << 8)); }
static DWORD rd32(const BYTE * p) {
  return (DWORD) p[0]        | ((DWORD) p[1] << 8)
       | ((DWORD) p[2] << 16) | ((DWORD) p[3] << 24);
}
int parse_bpb(const BYTE * s) {
  int i;
  int bps       = rd16(s + 0x0B);
  int spc       = s[0x0D];
  int reserved  = rd16(s + 0x0E);
  int nfats     = s[0x10];
  int root_ent  = rd16(s + 0x11);
  int total     = rd16(s + 0x13);
  int fat_size  = rd16(s + 0x16);
  int spt       = rd16(s + 0x18);
  int heads     = rd16(s + 0x1A);
  if (total == 0) total = (int) rd32(s + 0x20);
  if (bps != SECTOR_SIZE)                 return 0;
  if (spc == 0)                           return 0;
  if (nfats == 0)                         return 0;
  if (fat_size == 0)                      return 0;
  if (reserved == 0)                      return 0;
  if (total == 0 || total > MAX_SECTORS)   return 0;
  int root_sec   = (root_ent * 32 + bps - 1) / bps;
  int data_start = reserved + nfats * fat_size + root_sec;
  if (data_start >= total)                return 0;
  int clusters = (total - data_start) / spc;
  if (clusters < 1)                       return 0;
  /*  Require room for every 12-bit cluster entry.  */
  if (fat_size * SECTOR_SIZE < ((clusters + 2) * 3 + 1) / 2) return 0;
  /*  Prefer a known geometry to damaged BPB layout fields.  */
  const floppy_geom * g = geom_for_bpb(total, spt, heads);
  if (!g) g = geom_for_size(total);
  int cyls;
  if (g) {
    cyls = g->cyls; heads = g->heads; spt = g->spt;
  } else {
    if (spt <= 0 || spt > MAX_SPT)       spt = 18;
    if (heads <= 0 || heads > MAX_HEADS) heads = 2;
    cyls = total / (spt * heads);
    if (cyls <= 0 || cyls > MAX_CYLS)    cyls = MAX_CYLS;
  }
  G.bytes_per_sec   = bps;
  G.sec_per_cluster = spc;
  G.reserved_sec    = reserved;
  G.num_fats        = nfats;
  G.root_entries    = root_ent;
  G.total_sec       = total;
  G.media_byte      = s[0x15];
  G.fat_size        = fat_size;
  G.spt             = spt;
  G.heads           = heads;
  G.cyls            = cyls;
  G.data_start_sec  = data_start;
  G.total_clusters  = clusters;
  G.vol_id = rd32(s + 0x27);
  int label_ok = 1;
  Fi(11,
    BYTE c = s[0x2B + i];
    if (c < 0x20 || c > 0x7E) { label_ok = 0;  break; }
  );
  if (label_ok) {
    memcpy(G.vol_label, s + 0x2B, 11);
    G.vol_label[11] = 0;
    for (i = 10; i >= 0 && G.vol_label[i] == ' '; i--)
      G.vol_label[i] = 0;
  } else G.vol_label[0] = 0;

  int fs_ok = 1;
  Fi(8,
    BYTE c = s[0x36 + i];
    if (c < 0x20 || c > 0x7E) { fs_ok = 0;  break; }
  );
  if (fs_ok && (memcmp(s + 0x36, "FAT12",  5) == 0 ||
                memcmp(s + 0x36, "FAT16",  5) == 0 ||
                memcmp(s + 0x36, "FAT   ", 6) == 0)) {
    memcpy(G.fs_type, s + 0x36, 8);
    G.fs_type[8] = 0;
    for (i = 7; i >= 0 && G.fs_type[i] == ' '; i--)
      G.fs_type[i] = 0;
  } else {
    /*  Infer the FAT width from the cluster count.  */
    if (G.total_clusters < 4085)       lstrcpyA(G.fs_type, "FAT12");
    else if (G.total_clusters < 65525) lstrcpyA(G.fs_type, "FAT16");
    else                               lstrcpyA(G.fs_type, "FAT32");
  }
  return 1;
}
/*  Map an LBA to a cluster, or -1 outside the data area.  */
int lba_to_cluster(DWORD lba) {
  if (G.sec_per_cluster <= 0) return -1;
  if ((int) lba < G.data_start_sec) return -1;
  return 2 + ((int) lba - G.data_start_sec) / G.sec_per_cluster;
}
DWORD cluster_to_lba(int cl) {
  return G.data_start_sec + (cl - 2) * G.sec_per_cluster;
}
void name83_to_str(const fat_dirent * e, char * out) {
  int i, n = 0;
  for (i = 0; i < 8 && e->name[i] != ' '; i++) out[n++] = e->name[i];
  int has_ext = 0;
  for (i = 8; i < 11; i++)
    if (e->name[i] != ' ') { has_ext = 1;  break; }
  if (has_ext) {
    out[n++] = '.';
    for (i = 8; i < 11 && e->name[i] != ' '; i++) out[n++] = e->name[i];
  }
  out[n] = 0;
}
/*  FAT12 packs two little-endian entries into three bytes.  */
WORD fat12_get(const BYTE * fat, int n) {
  int ofs = n + (n >> 1);
  WORD w = (WORD) (fat[ofs] | (fat[ofs + 1] << 8));
  return (WORD) ((n & 1) ? (w >> 4) : (w & 0x0FFF));
}
void fat12_set(BYTE * fat, int n, WORD v) {
  int ofs = n + (n >> 1);
  WORD w = (WORD) (fat[ofs] | (fat[ofs + 1] << 8));
  if (n & 1) w = (WORD) ((w & 0x000F) | ((v & 0x0FFF) << 4));
  else       w = (WORD) ((w & 0xF000) | (v & 0x0FFF));
  fat[ofs] = (BYTE) w;
  fat[ofs + 1] = (BYTE) (w >> 8);
}

#define CHK_MAX_ISSUES 256

typedef struct {
  int  kind;       /*  Cross-link, lost, size, bad, FAT, or cycle.  */
  int  cluster;
  char what[224];
} chk_issue;

static void chk_add(chk_issue * issues, int * n, int kind, int cluster,
                    const char * what) {
  if (*n >= CHK_MAX_ISSUES) return;
  chk_issue * iss = &issues[(*n)++];
  iss->kind = kind;
  iss->cluster = cluster;
  lstrcpynA(iss->what, what, sizeof iss->what);
}
/*  Check one chain and record which file uses each cluster.  */
static int chk_walk_chain(const BYTE * fat, WORD start, WORD * owner,
                          int my_id, chk_issue * issues, int * n_issues,
                          const char * path) {
  int cnt = 0, cl = start;
  char w[224];
  while (cl >= 2 && cl < G.total_clusters + 2 && cnt < G.total_clusters) {
    if (owner[cl] != 0 && owner[cl] != my_id) {
      wsprintfA(w, "cluster %d is also used by another file or directory "
                   "(%s)", cl, path);
      chk_add(issues, n_issues, 0, cl, w);
    } else if (owner[cl] == my_id) {
      wsprintfA(w, "the cluster chain for %s loops at cluster %d", path, cl);
      chk_add(issues, n_issues, 6, cl, w);
      return -1;
    }
    owner[cl] = (WORD) my_id;  cnt++;
    WORD nx = fat12_get(fat, cl);
    if (nx == 0xFF7) {
      wsprintfA(w, "%s contains bad cluster %d", path, cl);
      chk_add(issues, n_issues, 3, cl, w);
      return -3;
    }
    if (nx >= 0xFF8) break;
    if (nx >= 0xFF0 && nx <= 0xFF6) {
      wsprintfA(w, "%s points to reserved FAT value 0x%03X",
                path, nx);
      chk_add(issues, n_issues, 4, cl, w);
      return -2;
    }
    cl = nx;
  }
  return cnt;
}

/*  Check a directory and its subdirectories.  */
static int chk_walk_dir(disk_handle * dh, const BYTE * fat,
                        WORD start_cluster, const char * path,
                        WORD * owner, int * next_id,
                        chk_issue * issues, int * n_issues, int depth) {
  int i, k;
  if (depth > 16) return 0;
  BYTE * buf = NULL;
  int n_entries = 0, dir_bytes = 0;
  if (start_cluster == 0) {
    int root_sec = (G.root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    dir_bytes = root_sec * SECTOR_SIZE;
    buf = (BYTE *) LocalAlloc(LPTR, dir_bytes);
    if (!buf) return 0;
    if (disk_read(dh, (DWORD) (G.reserved_sec + G.num_fats * G.fat_size),
                  (WORD) root_sec, buf) != 0) {
      LocalFree(buf);  return 0;
    }
    n_entries = G.root_entries;
  } else {
    /*  Bound corrupt directory chains by the volume size.  */
    int n_clusters = 0, cl = start_cluster;
    while (cl >= 2 && cl < G.total_clusters + 2
           && n_clusters < G.total_clusters) {
      n_clusters++;
      WORD nx = fat12_get(fat, cl);
      if (nx >= 0xFF8 || nx == 0xFF7) break;
      cl = nx;
    }
    if (n_clusters == 0) return 0;
    dir_bytes = n_clusters * G.sec_per_cluster * SECTOR_SIZE;
    buf = (BYTE *) LocalAlloc(LPTR, dir_bytes);
    if (!buf) return 0;
    int off = 0;
    cl = start_cluster;
    Fk(n_clusters,
      DWORD lba = cluster_to_lba(cl);
      if (disk_read(dh, lba, (WORD) G.sec_per_cluster, buf + off) != 0)
        memzero(buf + off, G.sec_per_cluster * SECTOR_SIZE);
      off += G.sec_per_cluster * SECTOR_SIZE;
      WORD nx = fat12_get(fat, cl);
      if (nx >= 0xFF8 || nx == 0xFF7) break;
      cl = nx;
    );
    n_entries = dir_bytes / 32;
  }
  int n_files = 0;
  Fi(n_entries,
    fat_dirent * e = (fat_dirent *) (buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5) continue;
    if (e->attr == 0x0F) continue;
    if (e->attr & 0x08) continue;
    char fname[16];  name83_to_str(e, fname);
    if (e->attr & 0x10) {
      if (e->name[0] == '.') continue;
      if (e->start_lo < 2) continue;
      int id = ++(*next_id);
      char sub_path[160];
      int pn = lstrlenA(path);
      if (pn > 100) pn = 100;
      memcpy(sub_path, path, pn);
      wsprintfA(sub_path + pn, "/%s", fname);
      int len = chk_walk_chain(fat, e->start_lo, owner, id,
                               issues, n_issues, sub_path);
      if (len >= 0)
        chk_walk_dir(dh, fat, e->start_lo, sub_path, owner,
                     next_id, issues, n_issues, depth + 1);
      continue;
    }
    if (e->size == 0 && e->start_lo == 0) continue;
    int id = ++(*next_id);
    char file_path[160];
    int pn = lstrlenA(path);
    if (pn > 100) pn = 100;
    memcpy(file_path, path, pn);
    wsprintfA(file_path + pn, "/%s", fname);
    if (e->start_lo == 0 && e->size > 0) {
      char w[224];
      wsprintfA(w, "%s has size %lu but no start cluster",
                file_path, (unsigned long) e->size);
      chk_add(issues, n_issues, 2, 0, w);
      continue;
    }
    int len = chk_walk_chain(fat, e->start_lo, owner, id,
                             issues, n_issues, file_path);
    if (len > 0) {
      int cluster_bytes = G.sec_per_cluster * SECTOR_SIZE;
      int expected = (e->size + cluster_bytes - 1) / cluster_bytes;
      if (expected != len) {
        char w[224];
        wsprintfA(w, "%s uses %d clusters but its size needs %d",
                  file_path, len, expected);
        chk_add(issues, n_issues, 2, e->start_lo, w);
      }
      n_files++;
    }
  );
  LocalFree(buf);
  return n_files;
}

int check_filesystem(disk_handle * dh) {
  int i, j, n;
  if (!G.has_fat) {
    /*  The checker may run before a surface scan reads the BPB.  */
    BYTE bpb[SECTOR_SIZE];
    if (disk_read(dh, 0, 1, bpb) == 0 && parse_bpb(bpb)) {
      G.has_fat = 1;
      SendMessageA(G.statusbar, SB_SETTEXTA, 1, (LPARAM) G.fs_type);
    } else {
      MessageBoxA(G.main,
          "fdchk did not find a valid FAT12 filesystem. It could not read "
          "the BPB, or the BPB is invalid.\r\n\r\nRun a standard or thorough "
          "surface scan instead.",
          "Filesystem check", MB_OK | MB_ICONWARNING);
      return 0;
    }
  }
  ui_status("Reading FAT...");
  int fat_bytes = G.fat_size * SECTOR_SIZE;
  BYTE * fat   = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  BYTE * fat2  = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  WORD * owner = (WORD *) LocalAlloc(LPTR,
      (G.total_clusters + 2) * sizeof(WORD));
  chk_issue * issues = (chk_issue *) LocalAlloc(LPTR,
      CHK_MAX_ISSUES * sizeof(chk_issue));
  int n_issues = 0, next_id = 0, n_lost = 0;
  if (!fat || !fat2 || !owner || !issues) goto done;
  if (disk_read(dh, (DWORD) G.reserved_sec, (WORD) G.fat_size, fat) != 0) {
    chk_add(issues, &n_issues, 4, 0, "cannot read the primary FAT");  goto report;
  }
  /*  Compare the first two FATs.  */
  if (G.num_fats >= 2 &&
      disk_read(dh, (DWORD) (G.reserved_sec + G.fat_size),
                (WORD) G.fat_size, fat2) == 0) {
    Fi(fat_bytes,
      if (fat[i] != fat2[i]) {
        chk_add(issues, &n_issues, 4, 0,
                "FAT #1 and FAT #2 disagree");
        break;
      });
  }
  /*  Show system sectors before checking data chains.  */
  Fi(MAX_SECTORS, G.state[i] = ST_UNTESTED);
  mark_system_sectors();
  SendMessageA(G.main, WM_APP_REPAINT, 0, 0);
  ui_status("Checking files and directories...");
  chk_walk_dir(dh, fat, 0, "/", owner, &next_id, issues, &n_issues, 0);
  /*  Show each allocated cluster and the file that uses it.  */
  int spc = G.sec_per_cluster;
  for (n = 2; n < G.total_clusters + 2; n++) {
    WORD v = fat12_get(fat, n);
    BYTE st;
    if (v == 0xFF7)    st = ST_BAD_OLD;
    else if (v == 0)   continue;
    else if (owner[n]) st = (BYTE) (ST_DATA_0 + (owner[n] & 3));
    else               st = ST_BAD_NEW;
    DWORD lba = cluster_to_lba(n);
    Fj(spc, if ((int) lba + j < MAX_SECTORS) G.state[lba + j] = st);
  }
  /*  Highlight cross-links and cycles.  */
  Fi(n_issues,
    if (issues[i].kind != 0 && issues[i].kind != 6) continue;
    int n = issues[i].cluster;
    if (n < 2 || n >= G.total_clusters + 2) continue;
    DWORD lba = cluster_to_lba(n);
    Fj(spc, if ((int) lba + j < MAX_SECTORS)
              G.state[lba + j] = ST_BAD_NEW);
  );
  SendMessageA(G.main, WM_APP_REPAINT, 0, 0);
  /*  Find allocated clusters without an owner.  */
  ui_status("Checking for lost clusters...");
  for (n = 2; n < G.total_clusters + 2; n++) {
    WORD v = fat12_get(fat, n);
    if (v == 0 || v == 0xFF7 || owner[n] != 0) continue;
    n_lost++;
  }
  if (n_lost > 0) {
    char w[224];
    wsprintfA(w, "%d lost cluster%s: allocated but not used by a file or "
                  "directory",
              n_lost, n_lost == 1 ? "" : "s");
    chk_add(issues, &n_issues, 1, 0, w);
  }
report: {
  char log_full[MAX_PATH];
  log_path(log_full, "fdchk-chkfs.log");
  HANDLE log = log_create("fdchk-chkfs.log");
  if (log != INVALID_HANDLE_VALUE) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    LOG_FMT(log,
        "fdchk filesystem check\r\n"
        "drive: %c:  filesystem: %s  volume: %s  serial: %08lX\r\n"
        "date: %04d-%02d-%02d %02d:%02d:%02d\r\n"
        "files: %d   lost clusters: %d   issues: %d\r\n\r\n",
        'A' + G.drive, G.fs_type, G.vol_label,
        (unsigned long) G.vol_id, t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, next_id, n_lost, n_issues);
    Fi(n_issues,
      int k = issues[i].kind;
      const char * tag = k == 0 ? "CROSS-LINK" : k == 1 ? "LOST"
                       : k == 2 ? "SIZE"       : k == 3 ? "BAD-CL"
                       : k == 4 ? "FAT"        : k == 6 ? "CYCLE"
                       : "OTHER";
      LOG_FMT(log, "  [%-10s] %s\r\n", tag, issues[i].what);
    );
    CloseHandle(log);
  }
  char summary[600];
  if (n_issues == 0) {
    wsprintfA(summary,
        "Filesystem check complete.\r\n\r\n"
        "Drive: %c:  Filesystem: %s\r\nFiles: %d\r\n"
        "No FAT errors, cross-links, or lost clusters.\r\n\r\n"
        "Log: %s",
        'A' + G.drive, G.fs_type, next_id, log_full);
    MessageBoxA(G.main, summary, "Filesystem check",
                MB_OK | MB_ICONINFORMATION);
  } else {
    int cl = 0, sz = 0, badcl = 0, fat_iss = 0, cycle = 0;
    Fi(n_issues,
      switch (issues[i].kind) {
        case 0: cl++;      break;
        case 1:            break;
        case 2: sz++;      break;
        case 3: badcl++;   break;
        case 4: fat_iss++; break;
        case 6: cycle++;   break;
      });
    wsprintfA(summary,
        "Filesystem errors: %d\r\n\r\n"
        "  Cross-linked clusters:    %d\r\n"
        "  Lost clusters:            %d\r\n"
        "  Wrong file sizes:         %d\r\n"
        "  Loops in cluster chains:  %d\r\n"
        "  Bad clusters in chains:   %d\r\n"
        "  Invalid FAT entries:      %d\r\n\r\n"
        "See %s for the full list.",
        n_issues, cl, n_lost, sz, cycle, badcl, fat_iss, log_full);
    MessageBoxA(G.main, summary, "Filesystem check",
                MB_OK | MB_ICONWARNING);
  }
}
done:
  if (fat)    LocalFree(fat);
  if (fat2)   LocalFree(fat2);
  if (owner)  LocalFree(owner);
  if (issues) LocalFree(issues);
  return 0;
}

/*  Recovery writes zeros for file data it cannot read.  */
#define MAX_REC_DEPTH 16
static int sector_is_bad(DWORD lba) {
  if (lba >= MAX_SECTORS) return 0;
  BYTE s = G.state[lba];
  return s == ST_BAD_NEW || s == ST_BAD_OLD;
}
/*  Count bad and total clusters in one file.  */
static int bad_clusters(const BYTE * fat, WORD start, int * n_clusters) {
  int i, bad = 0, cnt = 0, cl = start;
  while (cl >= 2 && cl < G.total_clusters + 2 && cnt < G.total_clusters) {
    DWORD lba = cluster_to_lba(cl);
    Fi(G.sec_per_cluster, if (sector_is_bad(lba + i)) { bad++;  break; });
    cnt++;
    WORD nx = fat12_get(fat, cl);
    if (nx >= 0xFF8) break;
    if (nx == 0xFF7) { bad++;  break; }
    cl = nx;
  }
  if (n_clusters) *n_clusters = cnt;
  return bad;
}
/*  Recover one file and return the number of missing parts.  */
static int recover_file(disk_handle * d, const BYTE * fat, WORD start,
                        DWORD size, const char * out_path, HANDLE log) {
  HANDLE h = CreateFileA(out_path, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  BYTE * buf = (BYTE *) LocalAlloc(LPTR, 8 * SECTOR_SIZE);
  if (!buf) { CloseHandle(h); return -1; }
  int i, cl = start, missing = 0, cnt = 0;
  DWORD remaining = size;
  while (cl >= 2 && cl < G.total_clusters + 2
         && cnt < G.total_clusters && remaining > 0) {
    DWORD lba = cluster_to_lba(cl);
    int bytes = G.sec_per_cluster * SECTOR_SIZE;
    if ((DWORD) bytes > remaining) bytes = remaining;
    int any_bad = 0;
    Fi(G.sec_per_cluster,
      if (sector_is_bad(lba + i)) { any_bad = 1;  break; });
    if (any_bad) {
      memzero(buf, bytes);
      missing++;
      if (log)
        LOG_FMT(log, "  MISSING @ cluster %d (LBA %lu) size %d bytes\r\n",
                cl, (unsigned long) lba, bytes);
    } else if (disk_read(d, lba, (WORD) G.sec_per_cluster, buf) != 0) {
      memzero(buf, bytes);
      missing++;
    }
    DWORD cb;
    WriteFile(h, buf, (DWORD) bytes, &cb, NULL);
    remaining -= bytes;
    cnt++;
    WORD nx = fat12_get(fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
  LocalFree(buf);  CloseHandle(h);
  return missing;
}

typedef struct {
  disk_handle * dh;
  const BYTE * fat;
  HANDLE       log;
  int          recovered, partial, subdirs;
} rec_ctx;
static void recover_dir(rec_ctx * ctx, WORD start_cluster,
                        const char * out_path, int depth);
static void recover_dir_buf(rec_ctx * ctx, BYTE * buf, int n_entries,
                            const char * out_path, int depth) {
  int i;
  if (depth > MAX_REC_DEPTH) return;
  Fi(n_entries,
    fat_dirent * e = (fat_dirent *) (buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5) continue;
    if (c0 == 0x05) e->name[0] = (char) 0xE5;
    if (e->attr == 0x0F) continue;
    if (e->attr & 0x08) continue;
    char fname[16];
    if (e->attr & 0x10) {
      if (e->name[0] == '.') continue;
      name83_to_str(e, fname);
      if (!fname[0]) continue;
      char sub_path[MAX_PATH];
      wsprintfA(sub_path, "%s\\%s", out_path, fname);
      CreateDirectoryA(sub_path, NULL);
      ctx->subdirs++;
      if (ctx->log != INVALID_HANDLE_VALUE)
        LOG_FMT(ctx->log, "  [DIR] %s\\\r\n", sub_path);
      recover_dir(ctx, e->start_lo, sub_path, depth + 1);
      continue;
    }
    if (e->size == 0) continue;
    name83_to_str(e, fname);
    if (!fname[0]) continue;
    int n_cl = 0;
    int n_bad = bad_clusters(ctx->fat, e->start_lo, &n_cl);
    char file_path[MAX_PATH];
    wsprintfA(file_path, "%s\\%s", out_path, fname);
    int missing = recover_file(ctx->dh, ctx->fat, e->start_lo, e->size,
                               file_path, ctx->log);
    if (missing > 0) ctx->partial++;
    else             ctx->recovered++;
    if (ctx->log != INVALID_HANDLE_VALUE)
      LOG_FMT(ctx->log, "  %-48s sz=%lu cl=%d bad=%d missing=%d\r\n",
              file_path, (unsigned long) e->size, n_cl, n_bad, missing);
  );
}

/*  Read and recover a directory. Cluster zero names the root.  */
static void recover_dir(rec_ctx * ctx, WORD start_cluster,
                        const char * out_path, int depth) {
  int k;
  if (depth > MAX_REC_DEPTH) return;
  if (start_cluster == 0) {
    int root_sec = (G.root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    BYTE * buf = (BYTE *) LocalAlloc(LPTR, root_sec * SECTOR_SIZE);
    if (!buf) return;
    if (disk_read(ctx->dh,
                  (DWORD) (G.reserved_sec + G.num_fats * G.fat_size),
                  (WORD) root_sec, buf) == 0)
      recover_dir_buf(ctx, buf, G.root_entries, out_path, depth);
    LocalFree(buf);
    return;
  }
  int n_clusters = 0, cl = start_cluster;
  while (cl >= 2 && cl < G.total_clusters + 2
         && n_clusters < G.total_clusters) {
    n_clusters++;
    WORD nx = fat12_get(ctx->fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
  if (n_clusters == 0) return;
  int bytes = n_clusters * G.sec_per_cluster * SECTOR_SIZE;
  BYTE * buf = (BYTE *) LocalAlloc(LPTR, bytes);
  if (!buf) return;
  int off = 0;
  cl = start_cluster;
  Fk(n_clusters,
    DWORD lba = cluster_to_lba(cl);
    /*  Use zeros for a directory cluster that cannot be read.  */
    if (disk_read(ctx->dh, lba, (WORD) G.sec_per_cluster, buf + off) != 0)
      memzero(buf + off, G.sec_per_cluster * SECTOR_SIZE);
    off += G.sec_per_cluster * SECTOR_SIZE;
    WORD nx = fat12_get(ctx->fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  );
  recover_dir_buf(ctx, buf, bytes / 32, out_path, depth);
  LocalFree(buf);
}

int recover_files(HWND parent, const char * out_dir) {
  disk_handle dh;
  if (!disk_open(&dh, G.drive)) {
    MessageBoxA(parent, "Cannot open the drive.", APP_NAME,
                MB_OK | MB_ICONERROR);
    return 0;
  }
  if (disk_lock(&dh, 0) < 0) {
    MessageBoxA(parent, "Cannot lock the drive for reading.", APP_NAME,
                MB_OK | MB_ICONERROR);
    disk_close(&dh);
    return 0;
  }
  BYTE * fat = (BYTE *) LocalAlloc(LPTR, G.fat_size * SECTOR_SIZE);
  if (!fat) { disk_unlock(&dh); disk_close(&dh); return 0; }
  if (disk_read(&dh, (DWORD) G.reserved_sec, (WORD) G.fat_size, fat) != 0) {
    MessageBoxA(parent, "Cannot read the FAT.", APP_NAME, MB_OK | MB_ICONERROR);
    LocalFree(fat); disk_unlock(&dh); disk_close(&dh);
    return 0;
  }
  char log_full[MAX_PATH];
  log_path(log_full, "fdchk-recovery.log");
  HANDLE log = log_create("fdchk-recovery.log");
  if (log != INVALID_HANDLE_VALUE) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    LOG_FMT(log,
        "fdchk recovery log\r\n"
        "drive: %c:  filesystem: %s  volume: %s  serial: %08lX\r\n"
        "date: %04d-%02d-%02d %02d:%02d:%02d\r\n"
        "bad sectors: %lu\r\n\r\nfiles and directories:\r\n",
        'A' + G.drive, G.fs_type, G.vol_label, (unsigned long) G.vol_id,
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        (unsigned long) G.bad_count);
  }
  rec_ctx ctx;  memzero(&ctx, sizeof ctx);
  ctx.dh  = &dh;  ctx.fat = fat;  ctx.log = log;
  recover_dir(&ctx, 0, out_dir, 0);
  if (log != INVALID_HANDLE_VALUE) {
    LOG_FMT(log,
        "\r\ndone: %d files copied in full, %d missing data, "
        "%d directories\r\n",
        ctx.recovered, ctx.partial, ctx.subdirs);
    CloseHandle(log);
  }
  LocalFree(fat);  disk_unlock(&dh);  disk_close(&dh);
  char msg[300];
  wsprintfA(msg,
      "Recovery complete.\r\n\r\n"
      "Files copied in full: %d\r\n"
      "Files missing data:   %d\r\n"
      "Directories:          %d\r\n\r\n"
      "fdchk wrote zeros in place of file data it could not read.\r\nLog: %s",
      ctx.recovered, ctx.partial, ctx.subdirs, log_full);
  MessageBoxA(parent, msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
  return 1;
}
