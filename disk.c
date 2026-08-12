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

/*  Raw sector I/O on Win9x via VWIN32 (Int 25h/26h with DIOC_REGISTERS),
    generic-IOCTL access to the block driver's device parameters and track
    formatter, and the FAT12 formatter.  */

#include "fdchk.h"

static const FloppyGeom geom_table[] = {
  /*                name                   total cyl hd spt spc rsv nf fsz root  media    dev     5" gap  rate */
  { "160 KB  -  5.25\" single-sided DD",     320, 40, 1,  8,  1,  1, 2,  1,  64, 0xFE, DEV_360K,  1, 0x50, 2 },
  { "180 KB  -  5.25\" single-sided DD",     360, 40, 1,  9,  1,  1, 2,  2,  64, 0xFC, DEV_360K,  1, 0x50, 2 },
  { "320 KB  -  5.25\" double-sided DD",     640, 40, 2,  8,  2,  1, 2,  1, 112, 0xFF, DEV_360K,  1, 0x50, 2 },
  { "360 KB  -  5.25\" double-sided DD",     720, 40, 2,  9,  2,  1, 2,  2, 112, 0xFD, DEV_360K,  1, 0x50, 2 },
  { "1.2 MB  -  5.25\" high density",       2400, 80, 2, 15,  1,  1, 2,  7, 224, 0xF9, DEV_1200K, 1, 0x54, 0 },
  { "720 KB  -  3.5\" double density",      1440, 80, 2,  9,  2,  1, 2,  3, 112, 0xF9, DEV_720K,  0, 0x50, 2 },
  { "1.44 MB -  3.5\" high density",        2880, 80, 2, 18,  1,  1, 2,  9, 224, 0xF0, DEV_1440K, 0, 0x6C, 0 },
  { "2.88 MB -  3.5\" extra density",       5760, 80, 2, 36,  2,  1, 2,  9, 240, 0xF0, DEV_2880K, 0, 0x53, 3 }
};
BYTE geom_rate_in_drive(const FloppyGeom * g, BYTE dev_type) {
  return dev_type == DEV_1200K && g->rate == 2 ? 1 : g->rate;
}
#define GEOM_N ((int) (sizeof geom_table / sizeof geom_table[0]))
#define GEOM_1440 (&geom_table[6])
int geom_count(void) { return GEOM_N; }
const FloppyGeom * geom_at(int i) {
  return i >= 0 && i < GEOM_N ? &geom_table[i] : NULL;
}
const FloppyGeom * geom_for_size(int total_sec) {
  for (int i = 0; i < GEOM_N; ++i)
    if (geom_table[i].total_sec == total_sec) return &geom_table[i];
  return NULL;
}
const FloppyGeom * geom_for_bpb(int total_sec, int spt, int heads) {
  for (int i = 0; i < GEOM_N; ++i) {
    const FloppyGeom * g = &geom_table[i];
    if (g->total_sec != total_sec) continue;
    if (spt > 0 && spt != g->spt) continue;
    if (heads > 0 && heads != g->heads) continue;
    return g;
  }
  return NULL;
}
const FloppyGeom * geom_for_drive_type(BYTE dev_type) {
  switch (dev_type) {
    case DEV_360K:  return &geom_table[3];   /*  360 KB  */
    case DEV_1200K: return &geom_table[4];   /*  1.2 MB  */
    case DEV_720K:  return &geom_table[5];   /*  720 KB  */
    case DEV_1440K: return &geom_table[6];   /*  1.44 MB  */
    case DEV_2880K: return &geom_table[7];   /*  2.88 MB  */
    default:        return NULL;
  }
}
const char * drive_type_str(BYTE dev_type) {
  switch (dev_type) {
    case DEV_360K:  return "5.25\" 360 KB";
    case DEV_1200K: return "5.25\" 1.2 MB";
    case DEV_720K:  return "3.5\" 720 KB";
    case DEV_1440K: return "3.5\" 1.44 MB";
    case DEV_2880K: return "3.5\" 2.88 MB";
    default:        return "Unknown Floppy";
  }
}
int geom_fits_drive(const FloppyGeom * g, BYTE dev_type) {
  if (!g) return 0;
  switch (dev_type) {
    case DEV_360K:  return  g->inch5 && g->total_sec <= 720;
    case DEV_1200K: return  g->inch5;
    case DEV_720K:  return !g->inch5 && g->total_sec <= 1440;
    case DEV_1440K: return !g->inch5 && g->total_sec <= 2880;
    case DEV_2880K: return !g->inch5;
    default:        return 1;   /*  drive type unknown: offer everything  */
  }
}
void geom_apply(const FloppyGeom * g) {
  if (!g) return;
  int root_sec = (g->root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
  G.bytes_per_sec   = SECTOR_SIZE;
  G.sec_per_cluster = g->sec_per_cluster;
  G.reserved_sec    = g->reserved_sec;
  G.num_fats        = g->num_fats;
  G.fat_size        = g->fat_size;
  G.root_entries    = g->root_entries;
  G.total_sec       = g->total_sec;
  G.media_byte      = g->media_byte;
  G.cyls            = g->cyls;
  G.heads           = g->heads;
  G.spt             = g->spt;
  G.data_start_sec  = g->reserved_sec + g->num_fats * g->fat_size + root_sec;
  G.total_clusters  = (g->total_sec - G.data_start_sec) / g->sec_per_cluster;
}
int disk_open(DiskHandle * d, int drive) {
  memzero(d, sizeof *d);
  d->drive = drive;
  d->hVwin32 = CreateFileA("\\\\.\\vwin32", 0, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_DELETE_ON_CLOSE, NULL);
  return d->hVwin32 != INVALID_HANDLE_VALUE;
}
void disk_close(DiskHandle * d) {
  if (d->hVwin32 && d->hVwin32 != INVALID_HANDLE_VALUE)
    CloseHandle(d->hVwin32);
  d->hVwin32 = NULL;
}
/*  Issue Int 21h AX=440Dh CX=08??h to VWIN32.  edx reaches DX verbatim: a
    flag for lock/unlock, a parameter-block pointer for the rest.  */
static int vwin32_ioctl_440d(DiskHandle * d, WORD cx, BYTE bh, DWORD edx) {
  DIOC_REGISTERS r;  memzero(&r, sizeof r);
  DWORD cb = 0;
  r.reg_EAX   = 0x440D;
  r.reg_EBX   = (DWORD) (BYTE) (d->drive + 1);   /*  1 = A:, 2 = B:  */
  r.reg_EBX  |= (DWORD) bh << 8;
  r.reg_ECX   = cx;
  r.reg_EDX   = edx;
  r.reg_Flags = 1;
  if (!DeviceIoControl(d->hVwin32, VWIN32_DIOC_DOS_IOCTL,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.reg_Flags & 1) return -(int) (r.reg_EAX & 0xFF);
  return 0;
}
/*  Lock the logical volume (Int 21h 440Dh, CX=084Ah; BH = lock level).  */
int disk_lock(DiskHandle * d, int level) {
  int r = vwin32_ioctl_440d(d, DOS_IOCTL_LOCK_VOLUME, (BYTE) level, 1);
  if (r == 0) d->locked++;
  return r;
}
/*  Lock for writing: level 1 -> 0.  Returns the level held, or the DOS error.
    Doing this wrong apparently fucks up the whole OS.  */
int disk_lock_tiered(DiskHandle * d) {
  int rc = disk_lock(d, 1);
  if (rc == 0) return 1;
  rc = disk_lock(d, 0);
  return rc < 0 ? rc : 0;
}
/*  Release every nested level.  */
int disk_unlock(DiskHandle * d) {
  int r = 0;
  while (d->locked > 0) {
    r = vwin32_ioctl_440d(d, DOS_IOCTL_UNLOCK_VOLUME, 0, 0);
    d->locked--;
    if (r < 0) { d->locked = 0; break; }
  }
  return r;
}
int disk_get_dev_params(DiskHandle * d, DosDevParams * p, int want_default) {
  memzero(p, sizeof *p);
  p->spec_func = want_default ? 0x01 : 0x00;
  return vwin32_ioctl_440d(d, DOS_IOCTL_GET_DEV_PARAMS, 0,
                           (DWORD) (ULONG_PTR) p);
}
int disk_set_dev_params(DiskHandle * d, const FloppyGeom * g, BYTE dev_type) {
  DosDevParams p;  memzero(&p, sizeof p);
  /*  bit 0 clear: adopt the BPB below as the drive's default.
      bit 2 set:   every sector on a track is the same size.  */
  p.spec_func = 0x04;
  if (dev_type == DEV_1200K && g->total_sec <= 720) {
    p.dev_type   = DEV_1200K;
    p.media_type = 1;              /*  360 KB media in a 1.2 MB drive  */
  } else {
    p.dev_type   = g->dos_dev_type;
    p.media_type = 0;
  }
  p.dev_attr      = 0;             /*  removable  */
  p.cyls          = (WORD) g->cyls;
  p.bytes_per_sec = SECTOR_SIZE;
  p.sec_per_clus  = (BYTE) g->sec_per_cluster;
  p.reserved_sec  = (WORD) g->reserved_sec;
  p.num_fats      = (BYTE) g->num_fats;
  p.root_entries  = (WORD) g->root_entries;
  p.total_sec     = (WORD) g->total_sec;
  p.media_byte    = g->media_byte;
  p.fat_size      = (WORD) g->fat_size;
  p.spt           = (WORD) g->spt;
  p.heads         = (WORD) g->heads;
  p.hidden_sec    = 0;
  p.total_sec32   = 0;
  p.track_spt     = (WORD) g->spt;
  for (int i = 0; i < g->spt && i < MAX_SPT; ++i) {
    p.track[i].sec  = (WORD) (i + 1);
    p.track[i].size = SECTOR_SIZE;
  }
  return vwin32_ioctl_440d(d, DOS_IOCTL_SET_DEV_PARAMS, 0,
                           (DWORD) (ULONG_PTR) &p);
}
/*  Run the FDC's FORMAT TRACK command over one track.  */
int disk_format_track(DiskHandle * d, int cyl, int head) {
  DosTrackParams t;  memzero(&t, sizeof t);
  t.head = (WORD) head;  t.cyl  = (WORD) cyl;
  return vwin32_ioctl_440d(d, DOS_IOCTL_FORMAT_TRACK, 0,
                           (DWORD) (ULONG_PTR) &t);
}
/*  Read back every sector of a track and check its CRC.  */
int disk_verify_track(DiskHandle * d, int cyl, int head) {
  DosTrackParams t;  memzero(&t, sizeof t);
  t.head = (WORD) head;  t.cyl  = (WORD) cyl;
  return vwin32_ioctl_440d(d, DOS_IOCTL_VERIFY_TRACK, 0,
                           (DWORD) (ULONG_PTR) &t);
}
/*  Ask CMOS for drive type..  */
BYTE disk_probe_drive_type(DiskHandle * d) {
  DosDevParams p;
  BYTE t = vxd_drive_type(d->drive);
  if (t != DEV_UNKNOWN) return t;
  if (disk_get_dev_params(d, &p, 1) != 0) return DEV_UNKNOWN;
  switch (p.dev_type) {
    case DEV_360K: case DEV_1200K: case DEV_720K:
    case DEV_1440K: case DEV_2880K: return p.dev_type;
    default:
      return DEV_UNKNOWN;
  }
}
int disk_resync_media(DiskHandle * d) {
  DosDevParams p;
  return disk_get_dev_params(d, &p, 0) == 0;
}
static int int25_err(const DIOC_REGISTERS * r) {
  int e = (int) (r->reg_EAX & 0xFF);
  if (e == 0) e = (int) ((r->reg_EAX >> 8) & 0xFF);
  if (e == 0) e = DOS_ERR_CTRL_FAIL;
  return -e;
}
/*  Read/write an absolute sector range.  0 on success, negative DOS error.  */
int disk_read(DiskHandle * d, DWORD lba, WORD count, void * buf) {
  DISKIO io;
  DIOC_REGISTERS r;  memzero(&r, sizeof r);
  DWORD cb = 0;
  io.dwStartSector = lba;
  io.wSectors      = count;
  io.dwBuffer      = (DWORD) (ULONG_PTR) buf;
  r.reg_EAX   = (DWORD) (BYTE) d->drive;
  r.reg_EBX   = (DWORD) (ULONG_PTR) &io;
  r.reg_ECX   = 0xFFFF;
  r.reg_Flags = 1;
  if (!DeviceIoControl(d->hVwin32, VWIN32_DIOC_DOS_INT25,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.reg_Flags & 1) return int25_err(&r);
  return 0;
}
int disk_write(DiskHandle * d, DWORD lba, WORD count, const void * buf) {
  DISKIO io;
  DIOC_REGISTERS r;  memzero(&r, sizeof r);
  DWORD cb = 0;
  io.dwStartSector = lba;
  io.wSectors      = count;
  io.dwBuffer      = (DWORD) (ULONG_PTR) buf;
  r.reg_EAX   = (DWORD) (BYTE) d->drive;
  r.reg_EBX   = (DWORD) (ULONG_PTR) &io;
  r.reg_ECX   = 0xFFFF;
  r.reg_Flags = 1;
  if (!DeviceIoControl(d->hVwin32, VWIN32_DIOC_DOS_INT26,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.reg_Flags & 1) return int25_err(&r);
  return 0;
}
static const FloppyGeom * geom_smallest_for_drive(BYTE dev_type) {
  for (int i = 0; i < GEOM_N; ++i)
    if (geom_fits_drive(&geom_table[i], dev_type)) return &geom_table[i];
  return GEOM_1440;
}

/*  Tell "no disk" from "broken disk".  */
int probe_disk_present(DiskHandle * d) {
  BYTE buf[SECTOR_SIZE];
  const FloppyGeom * g = geom_smallest_for_drive(disk_probe_drive_type(d));
  /*  Stay inside the smallest medium the drive takes.  */
  DWORD probes[4];
  probes[0] = 0;
  probes[1] = (DWORD) (g->reserved_sec + g->num_fats * g->fat_size);
  probes[2] = (DWORD) (g->total_sec / 2);
  probes[3] = (DWORD) (g->total_sec - 1);
  int not_ready = 0;
  for (int i = 0; i < 4; ++i) {
    int rc = disk_read(d, probes[i], 1, buf);
    if (rc == -DOS_ERR_CHANGE_LINE || rc == -DOS_ERR_TIMEOUT ||
        rc == -0xAA)
      { Sleep(150);  rc = disk_read(d, probes[i], 1, buf); }
    if (rc == 0) return MEDIA_PRESENT;      /*  one good read is enough  */
    if (rc == -DOS_ERR_TIMEOUT || rc == -DOS_ERR_CHANGE_LINE || rc == -0xAA)
      { not_ready = 1;  break; }
  }
  int fdc = vxd_sense_media(d->drive, NULL);
  if (fdc != -2) return fdc;
  return not_ready ? MEDIA_NONE : MEDIA_UNREADABLE;
}
/*  Int 21h extended error codes, as returned by a failed 440Dh call.  */
const char * dos_ioctl_err_str(int e) {
  switch (-e) {
    case 0x01: return "function not supported by the driver";
    case 0x05: return "access denied (volume not locked?)";
    case 0x06: return "invalid handle";
    case 0x0D: return "invalid data in the parameter block";
    case 0x0F: return "invalid drive";
    case 0x13: return "write-protected";
    case 0x14: return "unknown unit";
    case 0x15: return "drive not ready";
    case 0x16: return "unknown command";
    case 0x17: return "data error (CRC)";
    case 0x18: return "bad request structure length";
    case 0x19: return "seek error";
    case 0x1A: return "unknown media type";
    case 0x1B: return "sector not found";
    case 0x1D: return "write fault";
    case 0x1E: return "read fault";
    case 0x1F: return "general failure";
    default:   return "IOCTL refused";
  }
}
const char * dos_err_str(int e) {
  switch (-e) {
    case DOS_ERR_BAD_CMD:        return "bad command";
    case DOS_ERR_ADDR_MARK:      return "address mark not found";
    case DOS_ERR_WRITE_PROTECT:  return "write-protected";
    case DOS_ERR_SECTOR_NOT_FND: return "sector not found";
    case DOS_ERR_CHANGE_LINE:    return "media changed";
    case DOS_ERR_DMA_OVERRUN:    return "DMA overrun";
    case DOS_ERR_INVALID_MEDIA:  return "invalid media";
    case DOS_ERR_DATA:           return "data error (CRC)";
    case DOS_ERR_CTRL_FAIL:      return "controller failure";
    case DOS_ERR_SEEK_FAIL:      return "seek failure";
    case DOS_ERR_TIMEOUT:        return "drive not ready";
    default:                     return "I/O error";
  }
}

/*  FAT12 formatter.  FMT_QUICK rewrites only the boot sector, FATs and root
    directory, so it needs every track to already carry valid sector IDs - a
    logical write can only replace the data field of a sector the controller
    can find.  FMT_FULL re-lays and verifies every track first, which is what
    Windows' non-quick format does and the only thing that revives a disk
    whose address marks are gone.  If the driver refuses the format-track
    IOCTL, FMT_FULL degrades to overwriting every sector by hand.  */
static void fmt_build_boot(BYTE * sec, const FloppyGeom * g,
                           const char * label) {
  memzero(sec, SECTOR_SIZE);
  sec[0x00] = 0xEB; sec[0x01] = 0x3C; sec[0x02] = 0x90;   /*  jmpBoot  */
  memcpy(sec + 0x03, "MSWIN4.1", 8);
  sec[0x0B] = (BYTE) (SECTOR_SIZE & 0xFF);
  sec[0x0C] = (BYTE) (SECTOR_SIZE >> 8);
  sec[0x0D] = (BYTE) g->sec_per_cluster;
  sec[0x0E] = (BYTE) g->reserved_sec;
  sec[0x0F] = 0;
  sec[0x10] = (BYTE) g->num_fats;
  sec[0x11] = (BYTE) (g->root_entries & 0xFF);
  sec[0x12] = (BYTE) (g->root_entries >> 8);
  sec[0x13] = (BYTE) (g->total_sec & 0xFF);
  sec[0x14] = (BYTE) (g->total_sec >> 8);
  sec[0x15] = g->media_byte;
  sec[0x16] = (BYTE) g->fat_size;
  sec[0x17] = 0;
  sec[0x18] = (BYTE) g->spt;
  sec[0x19] = 0;
  sec[0x1A] = (BYTE) g->heads;
  sec[0x1B] = 0;
  /*  0x1C hidden sectors and 0x20 total_sec_32 stay zero.  */
  sec[0x24] = 0x00;                         /*  drive number  */
  sec[0x26] = 0x29;                         /*  extended boot signature  */
  DWORD serial = GetTickCount() ^ 0xCAFE0000u;
  sec[0x27] = (BYTE) serial;
  sec[0x28] = (BYTE) (serial >> 8);
  sec[0x29] = (BYTE) (serial >> 16);
  sec[0x2A] = (BYTE) (serial >> 24);
  /*  11-byte label, space-padded, upper-case A-Z 0-9 only.  */
  for (int i = 0; i < 11; ++i) sec[0x2B + i] = ' ';
  if (label) {
    for (int i = 0; i < 11 && label[i]; ++i) {
      char c = label[i];
      if (c >= 'a' && c <= 'z') c -= 32;
      if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
          c == ' ' || c == '_' || c == '-')
        sec[0x2B + i] = (BYTE) c;
    }
  }
  memcpy(sec + 0x36, "FAT12   ", 8);
  sec[0x1FE] = 0x55;
  sec[0x1FF] = 0xAA;
}
static int fmt_is_refusal(int rc) {
  switch (-rc) {
    case 0x01:   /*  not supported (also VWIN32 IOCTL refused, -1)  */
    case 0x05:   /*  access denied - lock level insufficient  */
    case 0x0D:   /*  invalid parameter block  */
    case 0x13:   /*  write-protected  */
    case 0x15:   /*  drive not ready  */
    case 0x16:   /*  unknown command  */
    case 0x1A:   /*  unknown media type  */
    case 0x1F:   /*  general failure  */
      return 1;
    default:
      return 0;
  }
}

/*  Re-lay every track.  bad_track[] is set per failing (cyl, head) so the FAT
    can mark those clusters afterwards.  */
static int fmt_low_level(DiskHandle * d, const FloppyGeom * g,
                         BYTE * bad_track, HANDLE log, int * failed_out) {
  int failed = 0, last_err = 0;
  int n_track = g->cyls * g->heads;
  int by_fdc = (G.fmt_method == FMT_BY_FDC && G.hVxd != NULL);
  BYTE dt = G.drive_type[d->drive & 1];
  char msg[96];
  LOG_FMT(log, "track formatter: %s\r\n\r\n",
          by_fdc ? "raw FDC (fdchk.vxd, command 4Dh, non-DMA)"
                 : "block driver (Int 21h 440Dh CX=0842h)");
  *failed_out = 0;
  for (int cyl = 0; cyl < g->cyls; ++cyl) {
    for (int head = 0; head < g->heads; ++head) {
      if (G.abort_req) return -DOS_ERR_BAD_CMD;
      int idx = cyl * g->heads + head;
      DWORD first = (DWORD) (idx * g->spt);
      wsprintfA(msg, "Formatting track %d, head %d of %d...",
                cyl, head, g->cyls - 1);
      ui_status(msg);
      for (int s = 0; s < g->spt; ++s)
        ui_set_state((int) first + s, ST_WRITING);
      FdcOut fo, vo;
      memzero(&fo, sizeof fo);
      memzero(&vo, sizeof vo);
      int frc = by_fdc ? vxd_format_track(d->drive, g, cyl, head, dt, &fo)
                       : disk_format_track(d, cyl, head);
      /*  Always record the first track in full.  */
      if (idx == 0 && by_fdc)
        LOG_FMT(log,
            "  first track: rc=%d stage=%d msr=%02x "
            "ST=%02x %02x %02x status=%02x n=%d\r\n",
            frc, fo.stage, fo.msr, fo.st0, fo.st1, fo.st2,
            fo.status, fo.result_n);
      /*  Track 0 tells us whether this can work at all.  */
      if (idx == 0 && frc != 0 && (by_fdc || fmt_is_refusal(frc))) {
        if (!by_fdc)
          LOG_FMT(log, "  refused by the driver (%s) - giving up\r\n",
                  dos_ioctl_err_str(frc));
        return frc;
      }
      int vrc = 0;
      if (frc == 0) {
        for (int s = 0; s < g->spt; ++s)
          ui_set_state((int) first + s, ST_VERIFY);
        vrc = by_fdc ? vxd_verify_track(d->drive, cyl, head, &vo)
                     : disk_verify_track(d, cyl, head);
      }
      int bad = frc != 0 || vrc != 0;
      BYTE st = bad ? ST_BAD_NEW : ST_GOOD;
      for (int s = 0; s < g->spt; ++s) ui_set_state((int) first + s, st);
      if (bad) {
        bad_track[idx] = 1;
        failed++;
        last_err = frc ? frc : vrc;
        if (by_fdc)
          LOG_FMT(log,
              "  cyl %2d head %d  fmt(st=%02x %02x %02x n=%d) "
              "verify(rc=%02x st=%02x %02x %02x c=%d n=%d status=%02x)\r\n",
              cyl, head, fo.st0, fo.st1, fo.st2, fo.result_n,
              (BYTE) -vrc, vo.st0, vo.st1, vo.st2, vo.c, vo.result_n,
              vo.status);
        else
          LOG_FMT(log, "  cyl %2d head %d  format=%s  verify=%02x\r\n",
                  cyl, head, dos_ioctl_err_str(frc), (BYTE) -vrc);
      }
      G.scanned += (DWORD) g->spt;
      G.current_sec = (int) first;
      ui_progress(G.scanned, (DWORD) g->total_sec);
    }
  }
  /*  Nothing bit at all.  */
  if (failed == n_track) {
    LOG_FMT(log, "  every track failed (code %02x) - treating as a refusal\r\n",
            (BYTE) -last_err);
    return last_err ? last_err : -0x1F;
  }
  *failed_out = failed;
  return 0;
}
int format_disk(DiskHandle * d, const FloppyGeom * g,
                const char * label, int style) {
  if (!g) g = GEOM_1440;
  int root_sec   = (g->root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
  int fat_bytes  = g->fat_size * SECTOR_SIZE;
  int data_start = g->reserved_sec + g->num_fats * g->fat_size + root_sec;
  int n_clusters = (g->total_sec - data_start) / g->sec_per_cluster;
  int n_track    = g->cyls * g->heads;
  BYTE * sec  = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  BYTE * fat  = (BYTE *) LocalAlloc(LPTR, fat_bytes);
  BYTE * root = (BYTE *) LocalAlloc(LPTR, root_sec * SECTOR_SIZE);
  BYTE * fill = NULL;
  BYTE bad_track[MAX_CYLS * MAX_HEADS];
  DosDevParams saved;
  int saved_ok = 0, rc = 0;
  G.fmt_lowlevel_ok = 0;
  G.fmt_bad_tracks  = 0;
  G.fmt_sys_bad     = 0;
  memzero(bad_track, sizeof bad_track);
  HANDLE log = log_create("fdchk-format.log");
  {
    SYSTEMTIME t;
    GetLocalTime(&t);
    LOG_FMT(log,
        "fdchk Format Log\r\n"
        "Drive: %c: (%s)   Target: %s\r\n"
        "Style: %s   Date: %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
        'A' + d->drive, drive_type_str(G.drive_type[d->drive & 1]), g->name,
        style == FMT_FULL ? "full (low-level)" : "quick",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  }
  if (!sec || !fat || !root) { rc = -1; goto done; }
  fmt_build_boot(sec, g, label);
  /*  FAT: entry 0 = media byte | 0xF00, entry 1 = end-of-chain reserve.  */
  fat[0] = g->media_byte; fat[1] = 0xFF; fat[2] = 0xFF;
  /*  Root directory: if a label was given, stamp it as a volume entry.  */
  if (label && label[0]) {
    FatDirEntry * e = (FatDirEntry *) root;
    memcpy(e->name, sec + 0x2B, 11);
    e->attr = 0x08;                         /*  volume label  */
    SYSTEMTIME t;  GetLocalTime(&t);
    e->wrt_t = (WORD) ((t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2));
    e->wrt_d = (WORD) (((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay);
  }
  /*  0840h installs its BPB as the drive's default.  */
  saved_ok = disk_get_dev_params(d, &saved, 1) == 0;
  /*  Arm the driver for this geometry.  */
  ui_status("Setting drive parameters...");
  int set_rc = disk_set_dev_params(d, g, G.drive_type[d->drive & 1]);
  if (set_rc != 0) {
    LOG_FMT(log, "set-device-parameters: %s; retrying\r\n",
            dos_ioctl_err_str(set_rc));
    set_rc = disk_set_dev_params(d, g, DEV_UNKNOWN);
  }
  LOG_FMT(log, "set-device-parameters: %s\r\n\r\n",
          set_rc ? dos_ioctl_err_str(set_rc) : "ok");
  if (set_rc != 0 && style == FMT_FULL) {
    wsprintfA(G.fmt_msg,
        "Note: the driver refused set-device-parameters (%s), so the track "
        "layout may not match the requested capacity.",
        dos_ioctl_err_str(set_rc));
  }

  if (style == FMT_FULL) {
    int failed = 0;
    int ll = fmt_low_level(d, g, bad_track, log, &failed);
    LOG_FMT(log, "low-level pass: rc=%d, %d of %d tracks failed\r\n",
            ll, failed, g->cyls * g->heads);
    if (G.abort_req) { rc = -1; goto done; }
    if (ll == 0) {
      G.fmt_lowlevel_ok = 1;
      G.fmt_bad_tracks  = failed;
      /*  Mark every sector of a failed track as a bad cluster.  */
      for (int t = 0; t < n_track; ++t) {
        if (!bad_track[t]) continue;
        for (int s = 0; s < g->spt; ++s) {
          int lba = t * g->spt + s;
          if (lba < data_start) { G.fmt_sys_bad = 1; continue; }
          int cl = 2 + (lba - data_start) / g->sec_per_cluster;
          if (cl >= 2 && cl < n_clusters + 2) fat12_set(fat, cl, 0xFF7);
        }
      }
      if (failed)
        LOG_FMT(log, "\r\n%d track(s) failed; their clusters marked 0xFF7\r\n",
                failed);
    } else if (G.fmt_method == FMT_BY_FDC && G.hVxd) {
      rc = -DOS_ERR_CTRL_FAIL;
      lstrcpyA(G.fmt_msg,
          "The raw FDC formatter could not lay down the first track.\r\n\r\n"
          "fdchk-format.log has the ST0/ST1/ST2 the controller reported.\r\n"
          "ST1 bit 4 (over-run) means this machine cannot feed the sector\r\n"
          "table fast enough without DMA - use the block-driver method.");
      goto done;
    } else if (ll == -0x13 || ll == -0x15) {
      /*  Write-protected or empty: no point trying it the long way.  */
      rc = (ll == -0x13) ? -DOS_ERR_WRITE_PROTECT : -DOS_ERR_TIMEOUT;
      goto done;
    } else {
      ui_status("Track format unavailable; overwriting sectors instead...");
      LOG_FMT(log, "falling back to a plain sector overwrite\r\n");
      fill = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE * g->spt);
      if (!fill) { rc = -1; goto done; }
      for (int i = 0; i < SECTOR_SIZE * g->spt; ++i) fill[i] = 0xF6;
      for (DWORD lba = 0; lba < (DWORD) g->total_sec; ) {
        if (G.abort_req) { rc = -1; goto done; }
        WORD n = (WORD) g->spt;
        if (lba + n > (DWORD) g->total_sec) n = (WORD) (g->total_sec - lba);
        rc = disk_write(d, lba, n, fill);
        if (rc) goto done;
        for (WORD s = 0; s < n; ++s) ui_set_state((int) (lba + s), ST_GOOD);
        lba += n;
        G.scanned = lba;
        G.current_sec = (int) lba;
        ui_progress(G.scanned, (DWORD) g->total_sec);
      }
    }
  }
  /*  Write the metadata.  */
  DWORD fat1 = (DWORD) g->reserved_sec;
  DWORD rootlba = fat1 + (DWORD) (g->num_fats * g->fat_size);
  ui_status("Writing boot sector...");
  rc = disk_write(d, 0, 1, sec);
  if (rc) goto done;
  for (int f = 0; f < g->num_fats; ++f) {
    char m[40];
    wsprintfA(m, "Writing FAT #%d...", f + 1);
    ui_status(m);
    rc = disk_write(d, fat1 + (DWORD) (f * g->fat_size),
                    (WORD) g->fat_size, fat);
    if (rc) goto done;
  }
  ui_status("Writing root directory...");
  rc = disk_write(d, rootlba, (WORD) root_sec, root);
  if (rc) goto done;
  ui_status("Format complete.");
done:
  /*  Put the drive's own parameters back.  */
  if (saved_ok) {
    saved.spec_func = 0x04;
    vwin32_ioctl_440d(d, DOS_IOCTL_SET_DEV_PARAMS, 0,
                      (DWORD) (ULONG_PTR) &saved);
  }
  if (log != INVALID_HANDLE_VALUE) {
    LOG_FMT(log, "\r\nresult: %s\r\n", rc ? dos_err_str(rc) : "ok");
    CloseHandle(log);
  }
  if (fill) LocalFree(fill);
  if (sec)  LocalFree(sec);
  if (fat)  LocalFree(fat);
  if (root) LocalFree(root);
  return rc;
}
/*  Format worker.  */
DWORD WINAPI format_thread_proc(LPVOID arg) {
  (void) arg;
  DiskHandle dh;
  const FloppyGeom * g = G.fmt_geom ? G.fmt_geom : GEOM_1440;
  G.fmt_rc = -1;
  G.fmt_msg[0] = 0;
  geom_apply(g);
  for (int i = 0; i < MAX_SECTORS; ++i) G.state[i] = ST_UNTESTED;
  SendMessageA(G.hMain, WM_APP_REPAINT, 0, 0);
  if (!disk_open(&dh, G.drive)) {
    lstrcpyA(G.fmt_msg, "Cannot open the drive (no VWIN32 / not Win9x).");
    ui_status("ERROR: cannot open the drive.");
    goto finish;
  }
  /*  Lock before touching the device.  */
  ui_status("Locking the volume...");
  int lock = disk_lock_tiered(&dh);
  if (lock < 0) {
    wsprintfA(G.fmt_msg,
        "Cannot lock drive %c: for format (%s).\r\n\r\n"
        "Close anything that might be using the disk and try again.",
        'A' + G.drive, dos_ioctl_err_str(lock));
    ui_status("Lock failed.");
    goto close;
  }
  BYTE dt = disk_probe_drive_type(&dh);
  if (dt != DEV_UNKNOWN) {
    G.drive_type[G.drive & 1] = dt;
    ui_drive_labels();
  }
  /*  Absorb the media-changed report that the first access after an
      insertion always returns, and notice an empty drive.  */
  ui_status("Checking for a disk...");
  if (probe_disk_present(&dh) == MEDIA_NONE) {
    char m[220];
    wsprintfA(m,
        "Drive %c: reports that it is empty or not ready.\r\n\r\n"
        "Insert a disk and click Retry to format anyway - a disk damaged\r\n"
        "badly enough can report this too.",
        'A' + G.drive);
    if (ui_prompt(m, "Format Floppy",
                  MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) {
      lstrcpyA(G.fmt_msg, "No disk in the drive.");
      ui_status("No disk in the drive.");
      goto close;
    }
  }
  int rc = format_disk(&dh, g, G.fmt_label, G.fmt_style);
  if (rc == 0) {
    G.fmt_rc = 0;
  } else if (G.abort_req) {
    G.fmt_rc = 1;
    lstrcpyA(G.fmt_msg, "Format stopped part-way through.");
  } else {
    G.fmt_rc = -1;
    if (G.fmt_style == FMT_QUICK)
      wsprintfA(G.fmt_msg,
          "The disk could not be written: %s.\r\n\r\n"
          "Try a Full format - it re-lays the sector address marks that a\r\n"
          "quick format cannot create.",
          dos_err_str(rc));
    else
      wsprintfA(G.fmt_msg,
          "The disk could not be written: %s.\r\n\r\n"
          "Every track was attempted; see fdchk-format.log for the\r\n"
          "per-track result.  The disk is most likely beyond repair.",
          dos_err_str(rc));
  }
  /*  Lock level reached is worth knowing when the driver refused work.  */
  if (G.fmt_rc != 0 && lock < 1) {
    char m[96];
    wsprintfA(m, "\r\n\r\n(Volume lock reached level %d.)", lock);
    lstrcpynA(G.fmt_msg + lstrlenA(G.fmt_msg), m,
              (int) (sizeof G.fmt_msg) - lstrlenA(G.fmt_msg));
  }
close:
  disk_unlock(&dh);
  disk_close(&dh);
finish:
  InterlockedExchange(&G.running, 0);
  PostMessageA(G.hMain, WM_APP_DONE, 0, 0);
  return 0;
}
