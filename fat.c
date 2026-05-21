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

/*  FAT12: BPB parsing, cluster arithmetic, the read-only consistency
    checker, and file recovery (copy out files, zero-filling bad spots).  */

#include "fdchk.h"

static WORD rd16(const BYTE * p) { return (WORD) (p[0] | (p[1] << 8)); }
static DWORD rd32(const BYTE * p) {
  return (DWORD) p[0]        | ((DWORD) p[1] << 8)
       | ((DWORD) p[2] << 16) | ((DWORD) p[3] << 24);
}

int parse_bpb(const BYTE * s) {
  G.bytes_per_sec   = rd16(s + 0x0B);
  G.sec_per_cluster = s[0x0D];
  G.reserved_sec    = rd16(s + 0x0E);
  G.num_fats        = s[0x10];
  G.root_entries    = rd16(s + 0x11);
  G.total_sec       = rd16(s + 0x13);
  G.media_byte      = s[0x15];
  G.fat_size        = rd16(s + 0x16);
  if (G.total_sec == 0) G.total_sec = (int) rd32(s + 0x20);

  if (G.bytes_per_sec != SECTOR_SIZE)               return 0;
  if (G.sec_per_cluster == 0)                       return 0;
  if (G.num_fats == 0)                              return 0;
  if (G.fat_size == 0)                              return 0;
  if (G.total_sec == 0 || G.total_sec > MAX_SECTORS) return 0;

  int root_sec = (G.root_entries * 32 + G.bytes_per_sec - 1)
               / G.bytes_per_sec;
  G.data_start_sec = G.reserved_sec + G.num_fats * G.fat_size + root_sec;
  G.total_clusters = (G.total_sec - G.data_start_sec) / G.sec_per_cluster;
  G.vol_id = rd32(s + 0x27);

  /*  The 11-byte volume label at 0x2B and the 8-byte FS-type label at
      0x36 are informational.  Validate both as printable 7-bit ASCII;
      if either is gibberish, fall back to something sensible.  Geometry,
      validated above, is the authoritative answer.  */
  int label_ok = 1;
  for (int i = 0; i < 11; ++i) {
    BYTE c = s[0x2B + i];
    if (c < 0x20 || c > 0x7E) { label_ok = 0; break; }
  }
  if (label_ok) {
    memcpy(G.vol_label, s + 0x2B, 11);
    G.vol_label[11] = 0;
    for (int i = 10; i >= 0 && G.vol_label[i] == ' '; --i)
      G.vol_label[i] = 0;
  } else {
    G.vol_label[0] = 0;
  }

  int fs_ok = 1;
  for (int i = 0; i < 8; ++i) {
    BYTE c = s[0x36 + i];
    if (c < 0x20 || c > 0x7E) { fs_ok = 0; break; }
  }
  if (fs_ok && (memcmp(s + 0x36, "FAT12", 5) == 0 ||
                memcmp(s + 0x36, "FAT16", 5) == 0 ||
                memcmp(s + 0x36, "FAT   ", 6) == 0)) {
    memcpy(G.fs_type, s + 0x36, 8);
    G.fs_type[8] = 0;
    for (int i = 7; i >= 0 && G.fs_type[i] == ' '; --i)
      G.fs_type[i] = 0;
  } else {
    /*  Infer from the cluster count per the FAT spec.  */
    if (G.total_clusters < 4085)       lstrcpyA(G.fs_type, "FAT12");
    else if (G.total_clusters < 65525) lstrcpyA(G.fs_type, "FAT16");
    else                               lstrcpyA(G.fs_type, "FAT32");
  }
  return 1;
}

/*  Clamp LBA -> cluster number; -1 if before the data area.  */
int lba_to_cluster(DWORD lba) {
  if ((int) lba < G.data_start_sec) return -1;
  return 2 + ((int) lba - G.data_start_sec) / G.sec_per_cluster;
}
DWORD cluster_to_lba(int cl) {
  return (DWORD) (G.data_start_sec + (cl - 2) * G.sec_per_cluster);
}

void name83_to_str(const FatDirEntry * e, char * out) {
  int n = 0;
  for (int i = 0; i < 8 && e->name[i] != ' '; ++i) out[n++] = e->name[i];
  int has_ext = 0;
  for (int i = 8; i < 11; ++i)
    if (e->name[i] != ' ') { has_ext = 1; break; }
  if (has_ext) {
    out[n++] = '.';
    for (int i = 8; i < 11 && e->name[i] != ' '; ++i) out[n++] = e->name[i];
  }
  out[n] = 0;
}

/*  FAT12 entry get/set: 12-bit, packed two per three bytes, little-endian.  */
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

/*  Filesystem checker - walks FAT12 for cross-linked clusters, lost
    chains, truncated/excess chains and bad directory entries.  Read-only;
    reports via a popup and a log file.  */

#define CHK_MAX_ISSUES 256

typedef struct {
  int  kind;       /*  0 cross-link, 1 lost, 2 size, 3 bad-in-chain,
                       4 invalid FAT, 6 cycle  */
  int  cluster;
  char what[224];  /*  wide enough for a deep path plus the message  */
} ChkIssue;

/*  Append an issue if there is room.  */
static void chk_add(ChkIssue * issues, int * n, int kind, int cluster,
                    const char * what) {
  if (*n >= CHK_MAX_ISSUES) return;
  ChkIssue * iss = &issues[(*n)++];
  iss->kind = kind;
  iss->cluster = cluster;
  lstrcpynA(iss->what, what, sizeof iss->what);
}

/*  Walk one cluster chain, marking each cluster's owner.  Returns the
    chain length, or -1 cycle / -2 out-of-range / -3 bad cluster hit.  */
static int chk_walk_chain(const BYTE * fat, WORD start, WORD * owner,
                          int my_id, ChkIssue * issues, int * n_issues,
                          const char * path) {
  int cnt = 0, cl = start;
  char w[224];
  while (cl >= 2 && cl < G.total_clusters + 2 && cnt < G.total_clusters) {
    if (owner[cl] != 0 && owner[cl] != my_id) {
      wsprintfA(w, "cluster %d shared between two chains (%s vs id %d)",
                cl, path, owner[cl]);
      chk_add(issues, n_issues, 0, cl, w);
    } else if (owner[cl] == my_id) {
      wsprintfA(w, "cluster %d is in a chain cycle (%s)", cl, path);
      chk_add(issues, n_issues, 6, cl, w);
      return -1;
    }
    owner[cl] = (WORD) my_id;
    cnt++;
    WORD nx = fat12_get(fat, cl);
    if (nx == 0xFF7) {
      wsprintfA(w, "chain (%s) contains BAD cluster %d", path, cl);
      chk_add(issues, n_issues, 3, cl, w);
      return -3;
    }
    if (nx >= 0xFF8) break;                  /*  normal end of chain  */
    if (nx >= 0xFF0 && nx <= 0xFF6) {
      wsprintfA(w, "chain (%s) points to reserved FAT value 0x%03X",
                path, nx);
      chk_add(issues, n_issues, 4, cl, w);
      return -2;
    }
    cl = nx;
  }
  return cnt;
}

/*  Recursively walk a directory.  start_cluster == 0 means the root.  */
static int chk_walk_dir(DiskHandle * dh, const BYTE * fat,
                        WORD start_cluster, const char * path,
                        WORD * owner, int * file_id_ctr,
                        ChkIssue * issues, int * n_issues, int depth) {
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
      LocalFree(buf);
      return 0;
    }
    n_entries = G.root_entries;
  } else {
    /*  Subdir cluster chain - walk it with a guard so a corrupt chain
        can't run away.  */
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
    for (int k = 0; k < n_clusters; ++k) {
      DWORD lba = cluster_to_lba(cl);
      if (disk_read(dh, lba, (WORD) G.sec_per_cluster, buf + off) != 0)
        memzero(buf + off, G.sec_per_cluster * SECTOR_SIZE);
      off += G.sec_per_cluster * SECTOR_SIZE;
      WORD nx = fat12_get(fat, cl);
      if (nx >= 0xFF8 || nx == 0xFF7) break;
      cl = nx;
    }
    n_entries = dir_bytes / 32;
  }

  int n_files = 0;
  for (int i = 0; i < n_entries; ++i) {
    FatDirEntry * e = (FatDirEntry *) (buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5) continue;
    if (e->attr == 0x0F) continue;        /*  LFN  */
    if (e->attr & 0x08) continue;         /*  volume label  */

    char fname[16];
    name83_to_str(e, fname);

    if (e->attr & 0x10) {
      /*  subdirectory  */
      if (e->name[0] == '.') continue;    /*  '.' and '..'  */
      if (e->start_lo < 2) continue;
      int id = ++(*file_id_ctr);
      char sub_path[160];
      int pn = lstrlenA(path);
      if (pn > 100) pn = 100;
      memcpy(sub_path, path, pn);
      wsprintfA(sub_path + pn, "/%s", fname);
      int len = chk_walk_chain(fat, e->start_lo, owner, id,
                               issues, n_issues, sub_path);
      if (len >= 0)
        chk_walk_dir(dh, fat, e->start_lo, sub_path, owner,
                     file_id_ctr, issues, n_issues, depth + 1);
      continue;
    }

    /*  regular file  */
    if (e->size == 0 && e->start_lo == 0) continue;
    int id = ++(*file_id_ctr);
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
        wsprintfA(w, "%s: chain has %d clusters but size needs %d",
                  file_path, len, expected);
        chk_add(issues, n_issues, 2, e->start_lo, w);
      }
      n_files++;
    }
  }

  LocalFree(buf);
  return n_files;
}

int chkfs_worker(DiskHandle * dh) {
  if (!G.has_fat) {
    /*  chkfs may run before the normal BPB step; (re-)read it now.  */
    BYTE bpb[SECTOR_SIZE];
    if (disk_read(dh, 0, 1, bpb) == 0 && parse_bpb(bpb)) {
      G.has_fat = 1;
      SendMessageA(G.hStatus, SB_SETTEXTA, 1, (LPARAM) G.fs_type);
    } else {
      MessageBoxA(G.hMain,
          "No valid FAT12 filesystem found on this disk.\r\n\r\n"
          "(BPB unreadable or invalid.  Use Standard or Thorough\r\n"
          "mode for a surface scan.)",
          "Filesystem Check", MB_OK | MB_ICONWARNING);
      return 0;
    }
  }

  ui_status("Reading FAT...");
  int fat_bytes = G.fat_size * SECTOR_SIZE;
  BYTE * fat   = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  BYTE * fat2  = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  WORD * owner = (WORD *) LocalAlloc(LPTR,
      (G.total_clusters + 2) * sizeof(WORD));
  ChkIssue * issues = (ChkIssue *) LocalAlloc(LPTR,
      CHK_MAX_ISSUES * sizeof(ChkIssue));
  int n_issues = 0, file_id_ctr = 0, n_lost = 0;

  if (!fat || !fat2 || !owner || !issues) goto done;

  if (disk_read(dh, (DWORD) G.reserved_sec, (WORD) G.fat_size, fat) != 0) {
    chk_add(issues, &n_issues, 4, 0, "Primary FAT unreadable");
    goto report;
  }

  /*  Cross-check FAT #1 against FAT #2.  */
  if (G.num_fats >= 2 &&
      disk_read(dh, (DWORD) (G.reserved_sec + G.fat_size),
                (WORD) G.fat_size, fat2) == 0) {
    for (int i = 0; i < fat_bytes; ++i)
      if (fat[i] != fat2[i]) {
        chk_add(issues, &n_issues, 4, 0,
                "FAT #1 and FAT #2 disagree (rewrite suggested)");
        break;
      }
  }

  /*  Pre-paint: untested + system area; the walk recolours the rest.  */
  for (int i = 0; i < MAX_SECTORS; ++i) G.state[i] = ST_UNTESTED;
  mark_system_sectors();
  SendMessageA(G.hMain, WM_APP_REPAINT, 0, 0);

  ui_status("Walking directory tree...");
  chk_walk_dir(dh, fat, 0, "/", owner, &file_id_ctr, issues, &n_issues, 0);

  /*  Paint each data cluster by its FAT state and ownership:
        marked bad      -> ST_BAD_OLD
        free            -> left ST_UNTESTED
        allocated+owned -> ST_DATA_*
        allocated+lost  -> ST_BAD_NEW  */
  int spc = G.sec_per_cluster;
  for (int n = 2; n < G.total_clusters + 2; ++n) {
    WORD v = fat12_get(fat, n);
    BYTE st;
    if (v == 0xFF7)    st = ST_BAD_OLD;
    else if (v == 0)   continue;
    else if (owner[n]) st = (BYTE) (ST_DATA_0 + (owner[n] & 3));
    else               st = ST_BAD_NEW;
    DWORD lba = cluster_to_lba(n);
    for (int s = 0; s < spc; ++s)
      if ((int) lba + s < MAX_SECTORS) G.state[lba + s] = st;
  }
  /*  Cross-linked / cycle clusters: force bright red.  */
  for (int i = 0; i < n_issues; ++i) {
    if (issues[i].kind != 0 && issues[i].kind != 6) continue;
    int n = issues[i].cluster;
    if (n < 2 || n >= G.total_clusters + 2) continue;
    DWORD lba = cluster_to_lba(n);
    for (int s = 0; s < spc; ++s)
      if ((int) lba + s < MAX_SECTORS) G.state[lba + s] = ST_BAD_NEW;
  }
  SendMessageA(G.hMain, WM_APP_REPAINT, 0, 0);

  /*  Lost chains: any allocated cluster no walked file claimed.  */
  ui_status("Checking for lost clusters...");
  for (int n = 2; n < G.total_clusters + 2; ++n) {
    WORD v = fat12_get(fat, n);
    if (v == 0 || v == 0xFF7 || owner[n] != 0) continue;
    n_lost++;
  }
  if (n_lost > 0) {
    char w[224];
    wsprintfA(w, "%d lost cluster%s (allocated in FAT but unreferenced)",
              n_lost, n_lost == 1 ? "" : "s");
    chk_add(issues, &n_issues, 1, 0, w);
  }

report: {
  char log_full[MAX_PATH];
  log_path(log_full, "fdchk-chkfs.log");
  HANDLE log = CreateFileA(log_full, GENERIC_WRITE, 0, NULL,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (log != INVALID_HANDLE_VALUE) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    LOG_FMT(log,
        "fdchk filesystem check\r\n"
        "Drive: %c:  FS: %s  Vol: %s  Serial: %08lX\r\n"
        "Date:  %04d-%02d-%02d %02d:%02d:%02d\r\n"
        "Files walked: %d   Lost clusters: %d   Issues: %d\r\n\r\n",
        'A' + G.drive, G.fs_type, G.vol_label,
        (unsigned long) G.vol_id, t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, file_id_ctr, n_lost, n_issues);
    for (int i = 0; i < n_issues; ++i) {
      int k = issues[i].kind;
      const char * tag = k == 0 ? "CROSS-LINK" : k == 1 ? "LOST"
                       : k == 2 ? "SIZE"       : k == 3 ? "BAD-CL"
                       : k == 4 ? "FAT"        : k == 6 ? "CYCLE"
                       : "OTHER";
      LOG_FMT(log, "  [%-10s] %s\r\n", tag, issues[i].what);
    }
    CloseHandle(log);
  }

  char summary[600];
  if (n_issues == 0) {
    wsprintfA(summary,
        "Filesystem check complete - NO ISSUES.\r\n\r\n"
        "Drive: %c:  FS: %s\r\nFiles walked: %d\r\n"
        "All FAT entries consistent; no cross-linked or lost clusters.\r\n"
        "\r\nLog: %s",
        'A' + G.drive, G.fs_type, file_id_ctr, log_full);
    MessageBoxA(G.hMain, summary, "Filesystem Check",
                MB_OK | MB_ICONINFORMATION);
  } else {
    int cl = 0, lost = 0, sz = 0, badcl = 0, fat_iss = 0, cycle = 0;
    for (int i = 0; i < n_issues; ++i)
      switch (issues[i].kind) {
        case 0: cl++;      break;
        case 1: lost++;    break;
        case 2: sz++;      break;
        case 3: badcl++;   break;
        case 4: fat_iss++; break;
        case 6: cycle++;   break;
      }
    wsprintfA(summary,
        "Filesystem check found %d issue(s):\r\n\r\n"
        "  Cross-linked clusters:    %d\r\n"
        "  Lost cluster reports:     %d\r\n"
        "  Size mismatches:          %d\r\n"
        "  Chain-cycle errors:       %d\r\n"
        "  Bad clusters in chains:   %d\r\n"
        "  Invalid FAT entries:      %d\r\n\r\n"
        "See %s for the full list.",
        n_issues, cl, lost, sz, cycle, badcl, fat_iss, log_full);
    MessageBoxA(G.hMain, summary, "Filesystem Check",
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

/*  Recovery - copy out files, zero-filling bad regions, write a log.  */

#define MAX_REC_DEPTH 16

static int sector_is_bad(DWORD lba) {
  if (lba >= MAX_SECTORS) return 0;
  BYTE s = G.state[lba];
  return s == ST_BAD_NEW || s == ST_BAD_OLD;
}

/*  Walk a file's chain; return the number of clusters touching a bad
    sector, and the cluster count via n_clusters.  */
static int file_bad_clusters(const BYTE * fat, WORD start, int * n_clusters) {
  int bad = 0, cnt = 0, cl = start;
  while (cl >= 2 && cl < G.total_clusters + 2 && cnt < G.total_clusters) {
    DWORD lba = cluster_to_lba(cl);
    for (int s = 0; s < G.sec_per_cluster; ++s)
      if (sector_is_bad(lba + s)) { bad++; break; }
    cnt++;
    WORD nx = fat12_get(fat, cl);
    if (nx >= 0xFF8) break;
    if (nx == 0xFF7) { bad++; break; }
    cl = nx;
  }
  if (n_clusters) *n_clusters = cnt;
  return bad;
}

/*  Copy a file out, zero-filling bad clusters.  Returns the hole count.  */
static int copy_file_skipping_bad(DiskHandle * d, const BYTE * fat,
                                  WORD start, DWORD size,
                                  const char * out_path, HANDLE log) {
  HANDLE h = CreateFileA(out_path, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  BYTE * buf = (BYTE *) LocalAlloc(LPTR, 8 * SECTOR_SIZE);
  if (!buf) { CloseHandle(h); return -1; }

  int cl = start, holes = 0, cnt = 0;
  DWORD remaining = size;
  while (cl >= 2 && cl < G.total_clusters + 2
         && cnt < G.total_clusters && remaining > 0) {
    DWORD lba = cluster_to_lba(cl);
    int bytes = G.sec_per_cluster * SECTOR_SIZE;
    if ((DWORD) bytes > remaining) bytes = remaining;

    int any_bad = 0;
    for (int s = 0; s < G.sec_per_cluster; ++s)
      if (sector_is_bad(lba + s)) { any_bad = 1; break; }

    if (any_bad) {
      memzero(buf, bytes);
      holes++;
      if (log)
        LOG_FMT(log, "  HOLE @ cluster %d (LBA %lu) size %d bytes\r\n",
                cl, (unsigned long) lba, bytes);
    } else if (disk_read(d, lba, (WORD) G.sec_per_cluster, buf) != 0) {
      memzero(buf, bytes);
      holes++;
    }
    DWORD cb;
    WriteFile(h, buf, (DWORD) bytes, &cb, NULL);
    remaining -= bytes;
    cnt++;
    WORD nx = fat12_get(fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
  LocalFree(buf);
  CloseHandle(h);
  return holes;
}

/*  Recovery context threaded through the recursive walk.  */
typedef struct {
  DiskHandle * dh;
  const BYTE * fat;
  HANDLE       log;
  int          recovered, partial, subdirs;
} RecCtx;

static void recover_dir_at_cluster(RecCtx * ctx, WORD start_cluster,
                                   const char * out_path, int depth);

/*  Walk an in-memory directory buffer; start_cluster == 0 means root.  */
static void recover_dir_buf(RecCtx * ctx, BYTE * buf, int n_entries,
                            const char * out_path, int depth) {
  if (depth > MAX_REC_DEPTH) return;
  for (int i = 0; i < n_entries; ++i) {
    FatDirEntry * e = (FatDirEntry *) (buf + i * 32);
    BYTE c0 = (BYTE) e->name[0];
    if (c0 == 0x00) break;
    if (c0 == 0xE5) continue;
    if (c0 == 0x05) e->name[0] = (char) 0xE5;
    if (e->attr == 0x0F) continue;        /*  LFN  */
    if (e->attr & 0x08) continue;         /*  volume label  */

    char fname[16];
    if (e->attr & 0x10) {
      /*  subdirectory  */
      if (e->name[0] == '.') continue;
      name83_to_str(e, fname);
      if (!fname[0]) continue;
      char sub_path[MAX_PATH];
      wsprintfA(sub_path, "%s\\%s", out_path, fname);
      CreateDirectoryA(sub_path, NULL);
      ctx->subdirs++;
      if (ctx->log != INVALID_HANDLE_VALUE)
        LOG_FMT(ctx->log, "  [DIR] %s\\\r\n", sub_path);
      recover_dir_at_cluster(ctx, e->start_lo, sub_path, depth + 1);
      continue;
    }

    if (e->size == 0) continue;
    name83_to_str(e, fname);
    if (!fname[0]) continue;
    int n_cl = 0;
    int n_bad = file_bad_clusters(ctx->fat, e->start_lo, &n_cl);
    char file_path[MAX_PATH];
    wsprintfA(file_path, "%s\\%s", out_path, fname);
    int holes = copy_file_skipping_bad(ctx->dh, ctx->fat, e->start_lo,
                                       e->size, file_path, ctx->log);
    if (holes > 0) ctx->partial++;
    else           ctx->recovered++;
    if (ctx->log != INVALID_HANDLE_VALUE)
      LOG_FMT(ctx->log, "  %-48s sz=%lu cl=%d bad=%d holes=%d\r\n",
              file_path, (unsigned long) e->size, n_cl, n_bad, holes);
  }
}

/*  Read the directory at start_cluster (0 = root) and walk it.  */
static void recover_dir_at_cluster(RecCtx * ctx, WORD start_cluster,
                                   const char * out_path, int depth) {
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
  for (int k = 0; k < n_clusters; ++k) {
    DWORD lba = cluster_to_lba(cl);
    /*  A bad directory cluster: zero-fill it, lose its entries, walk on.  */
    if (disk_read(ctx->dh, lba, (WORD) G.sec_per_cluster, buf + off) != 0)
      memzero(buf + off, G.sec_per_cluster * SECTOR_SIZE);
    off += G.sec_per_cluster * SECTOR_SIZE;
    WORD nx = fat12_get(ctx->fat, cl);
    if (nx >= 0xFF8 || nx == 0xFF7) break;
    cl = nx;
  }
  recover_dir_buf(ctx, buf, bytes / 32, out_path, depth);
  LocalFree(buf);
}

/*  Recovery driver - synchronous, run from the UI thread (a floppy walk
    is quick enough to not need its own worker).  */
int do_recovery(HWND parent, const char * out_dir) {
  DiskHandle dh;
  if (!disk_open(&dh, G.drive)) {
    MessageBoxA(parent, "Cannot open drive.", APP_NAME,
                MB_OK | MB_ICONERROR);
    return 0;
  }
  if (disk_lock(&dh, 0) < 0) {
    MessageBoxA(parent, "Cannot lock drive for read.", APP_NAME,
                MB_OK | MB_ICONERROR);
    disk_close(&dh);
    return 0;
  }
  BYTE * fat = (BYTE *) LocalAlloc(LPTR, G.fat_size * SECTOR_SIZE);
  if (!fat) { disk_unlock(&dh); disk_close(&dh); return 0; }
  if (disk_read(&dh, (DWORD) G.reserved_sec, (WORD) G.fat_size, fat) != 0) {
    MessageBoxA(parent, "FAT unreadable.", APP_NAME, MB_OK | MB_ICONERROR);
    LocalFree(fat); disk_unlock(&dh); disk_close(&dh);
    return 0;
  }

  char log_full[MAX_PATH];
  log_path(log_full, "fdchk-recovery.log");
  HANDLE log = CreateFileA(log_full, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (log != INVALID_HANDLE_VALUE) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    LOG_FMT(log,
        "fdchk recovery log\r\n"
        "Drive: %c:  FS: %s  Volume: %s  Serial: %08lX\r\n"
        "Date:  %04d-%02d-%02d %02d:%02d:%02d\r\n"
        "Bad sectors found: %lu\r\n\r\nFiles & directories:\r\n",
        'A' + G.drive, G.fs_type, G.vol_label, (unsigned long) G.vol_id,
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        (unsigned long) G.bad_count);
  }

  RecCtx ctx;
  memzero(&ctx, sizeof ctx);
  ctx.dh  = &dh;
  ctx.fat = fat;
  ctx.log = log;
  recover_dir_at_cluster(&ctx, 0, out_dir, 0);

  if (log != INVALID_HANDLE_VALUE) {
    LOG_FMT(log,
        "\r\nDone.  %d files clean, %d with holes, %d subdirectories.\r\n",
        ctx.recovered, ctx.partial, ctx.subdirs);
    CloseHandle(log);
  }

  LocalFree(fat);
  disk_unlock(&dh);
  disk_close(&dh);

  char msg[300];
  wsprintfA(msg,
      "Recovery complete.\r\n\r\n"
      "%d files copied cleanly.\r\n"
      "%d files copied with zero-filled gaps where bad sectors were.\r\n"
      "%d subdirectories descended into.\r\n\r\nLog: %s",
      ctx.recovered, ctx.partial, ctx.subdirs, log_full);
  MessageBoxA(parent, msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
  return 1;
}
