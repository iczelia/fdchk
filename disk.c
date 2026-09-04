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

/*  Raw sector I/O, media geometry, and FAT12 formatting.  */

#include "fdchk.h"

static const floppy_geom GEOM[] = {
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
BYTE geom_rate_in_drive(const floppy_geom * g, BYTE dev_type) {
  return dev_type == DEV_1200K && g->rate == 2 ? 1 : g->rate;
}
#define GEOM_N ((int) (sizeof GEOM / sizeof GEOM[0]))
#define GEOM_1440 (&GEOM[6])
int geom_count(void) { return GEOM_N; }
const floppy_geom * geom_at(int i) {
  return i >= 0 && i < GEOM_N ? &GEOM[i] : NULL;
}
const floppy_geom * geom_for_size(int total_sec) {
  int i;
  Fi(GEOM_N, if (GEOM[i].total_sec == total_sec) return &GEOM[i]);
  return NULL;
}
const floppy_geom * geom_for_bpb(int total_sec, int spt, int heads) {
  int i;
  Fi(GEOM_N,
    const floppy_geom * g = &GEOM[i];
    if (g->total_sec != total_sec) continue;
    if (spt > 0 && spt != g->spt) continue;
    if (heads > 0 && heads != g->heads) continue;
    return g;
  );
  return NULL;
}
const floppy_geom * geom_for_drive_type(BYTE dev_type) {
  switch (dev_type) {
    case DEV_360K:  return &GEOM[3];   /*  360 KB  */
    case DEV_1200K: return &GEOM[4];   /*  1.2 MB  */
    case DEV_720K:  return &GEOM[5];   /*  720 KB  */
    case DEV_1440K: return &GEOM[6];   /*  1.44 MB  */
    case DEV_2880K: return &GEOM[7];   /*  2.88 MB  */
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
    default:        return "unknown floppy";
  }
}
int geom_fits_drive(const floppy_geom * g, BYTE dev_type) {
  if (!g) return 0;
  switch (dev_type) {
    case DEV_360K:  return  g->inch5 && g->total_sec <= 720;
    case DEV_1200K: return  g->inch5;
    case DEV_720K:  return !g->inch5 && g->total_sec <= 1440;
    case DEV_1440K: return !g->inch5 && g->total_sec <= 2880;
    case DEV_2880K: return !g->inch5;
    default:        return 1;   /*  Unknown drive: list every format.  */
  }
}
void geom_apply(const floppy_geom * g) {
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
int disk_open(disk_handle * d, int drive) {
  memzero(d, sizeof *d);
  d->drive = drive;
  d->vwin32 = CreateFileA("\\\\.\\vwin32", 0, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_DELETE_ON_CLOSE, NULL);
  return d->vwin32 != INVALID_HANDLE_VALUE;
}
void disk_close(disk_handle * d) {
  if (d->vwin32 && d->vwin32 != INVALID_HANDLE_VALUE)
    CloseHandle(d->vwin32);
  d->vwin32 = NULL;
}
/*  Issue Int 21h AX=440Dh through VWIN32; EDX becomes DX verbatim.  */
static int vwin32_ioctl_440d(disk_handle * d, WORD cx, BYTE bh, DWORD edx) {
  dioc_regs r;  memzero(&r, sizeof r);
  DWORD cb = 0;
  r.eax   = 0x440D;
  r.ebx   = (DWORD) (BYTE) (d->drive + 1);   /*  1 A:, 2 B:.  */
  r.ebx  |= (DWORD) bh << 8;
  r.ecx   = cx;
  r.edx   = edx;
  r.flags = 1;
  if (!DeviceIoControl(d->vwin32, VWIN32_DIOC_DOS_IOCTL,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.flags & 1) return -(int) (r.eax & 0xFF);
  return 0;
}
/*  Lock the logical volume at one DOS lock level.  */
int disk_lock(disk_handle * d, int level) {
  int r = vwin32_ioctl_440d(d, DOS_IOCTL_LOCK_VOLUME, (BYTE) level, 1);
  if (r == 0) d->locked++;
  return r;
}
/*  Lock for writes at level 1, then 0. Return the held level or DOS error.  */
int disk_lock_tiered(disk_handle * d) {
  int rc = disk_lock(d, 1);
  if (rc == 0) return 1;
  rc = disk_lock(d, 0);
  return rc < 0 ? rc : 0;
}
/*  Release every held level.  */
int disk_unlock(disk_handle * d) {
  int r = 0;
  while (d->locked > 0) {
    r = vwin32_ioctl_440d(d, DOS_IOCTL_UNLOCK_VOLUME, 0, 0);
    d->locked--;
    if (r < 0) { d->locked = 0; break; }
  }
  return r;
}
int disk_get_dev_params(disk_handle * d, dos_dev_params * p, int want_default) {
  memzero(p, sizeof *p);
  p->spec_func = want_default ? 0x01 : 0x00;
  return vwin32_ioctl_440d(d, DOS_IOCTL_GET_DEV_PARAMS, 0,
                           (DWORD) (ULONG_PTR) p);
}
int disk_set_dev_params(disk_handle * d, const floppy_geom * g, BYTE dev_type) {
  dos_dev_params p;  memzero(&p, sizeof p);
  int i;
  /*  Copy the geometry into the driver parameters.  */
  p.spec_func = 0x04;
  if (dev_type == DEV_1200K && g->total_sec <= 720) {
    p.dev_type   = DEV_1200K;
    p.media_type = 1;              /*  360 KB media in a 1.2 MB drive.  */
  } else {
    p.dev_type   = g->dos_dev_type;
    p.media_type = 0;
  }
  p.dev_attr      = 0;             /*  Removable.  */
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
  Fi(MIN(g->spt, MAX_SPT),
    p.track[i].sec  = (WORD) (i + 1);
    p.track[i].size = SECTOR_SIZE;
  );
  return vwin32_ioctl_440d(d, DOS_IOCTL_SET_DEV_PARAMS, 0,
                           (DWORD) (ULONG_PTR) &p);
}
/*  Format one track through the DOS block driver.  */
int disk_format_track(disk_handle * d, int cyl, int head) {
  dos_track_params t;  memzero(&t, sizeof t);
  t.head = (WORD) head;  t.cyl  = (WORD) cyl;
  return vwin32_ioctl_440d(d, DOS_IOCTL_FORMAT_TRACK, 0,
                           (DWORD) (ULONG_PTR) &t);
}
/*  Check one track through the DOS block driver.  */
int disk_verify_track(disk_handle * d, int cyl, int head) {
  dos_track_params t;  memzero(&t, sizeof t);
  t.head = (WORD) head;  t.cyl  = (WORD) cyl;
  return vwin32_ioctl_440d(d, DOS_IOCTL_VERIFY_TRACK, 0,
                           (DWORD) (ULONG_PTR) &t);
}
/*  Read the drive type from CMOS or the block driver.  */
BYTE disk_probe_drive_type(disk_handle * d) {
  dos_dev_params p;
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
int disk_resync_media(disk_handle * d) {
  dos_dev_params p;
  return disk_get_dev_params(d, &p, 0) == 0;
}
static int int25_err(const dioc_regs * r) {
  int e = (int) (r->eax & 0xFF);
  if (e == 0) e = (int) ((r->eax >> 8) & 0xFF);
  if (e == 0) e = DOS_ERR_CTRL_FAIL;
  return -e;
}
/*  Read or write absolute sectors. Return 0 or a negative DOS error.  */
int disk_read(disk_handle * d, DWORD lba, WORD count, void * buf) {
  disk_io io;
  dioc_regs r;  memzero(&r, sizeof r);
  DWORD cb = 0;
  io.start_sector = lba;
  io.sectors      = count;
  io.buffer      = (DWORD) (ULONG_PTR) buf;
  r.eax   = (DWORD) (BYTE) d->drive;
  r.ebx   = (DWORD) (ULONG_PTR) &io;
  r.ecx   = 0xFFFF;
  r.flags = 1;
  if (!DeviceIoControl(d->vwin32, VWIN32_DIOC_DOS_INT25,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.flags & 1) return int25_err(&r);
  return 0;
}
int disk_write(disk_handle * d, DWORD lba, WORD count, const void * buf) {
  disk_io io;
  dioc_regs r;  memzero(&r, sizeof r);
  DWORD cb = 0;
  io.start_sector = lba;
  io.sectors      = count;
  io.buffer      = (DWORD) (ULONG_PTR) buf;
  r.eax   = (DWORD) (BYTE) d->drive;
  r.ebx   = (DWORD) (ULONG_PTR) &io;
  r.ecx   = 0xFFFF;
  r.flags = 1;
  if (!DeviceIoControl(d->vwin32, VWIN32_DIOC_DOS_INT26,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.flags & 1) return int25_err(&r);
  return 0;
}
static const floppy_geom * geom_smallest_for_drive(BYTE dev_type) {
  int i;
  Fi(GEOM_N, if (geom_fits_drive(&GEOM[i], dev_type)) return &GEOM[i]);
  return GEOM_1440;
}

/*  Distinguish an empty drive from unreadable media.  */
int probe_disk_present(disk_handle * d) {
  BYTE buf[SECTOR_SIZE];
  const floppy_geom * g = geom_smallest_for_drive(disk_probe_drive_type(d));
  int i;
  /*  Stay inside the smallest accepted geometry.  */
  DWORD probes[4];
  probes[0] = 0;
  probes[1] = (DWORD) (g->reserved_sec + g->num_fats * g->fat_size);
  probes[2] = (DWORD) (g->total_sec / 2);
  probes[3] = (DWORD) (g->total_sec - 1);
  int not_ready = 0;
  Fi(4,
    int rc = disk_read(d, probes[i], 1, buf);
    if (rc == -DOS_ERR_CHANGE_LINE || rc == -DOS_ERR_TIMEOUT ||
        rc == -0xAA) { Sleep(150);  rc = disk_read(d, probes[i], 1, buf); }
    if (rc == 0) return MEDIA_PRESENT;
    if (rc == -DOS_ERR_TIMEOUT || rc == -DOS_ERR_CHANGE_LINE || rc == -0xAA) {
      not_ready = 1;  break;
    }
  );
  int fdc = vxd_sense_media(d->drive, NULL);
  if (fdc != -2) return fdc;
  return not_ready ? MEDIA_NONE : MEDIA_UNREADABLE;
}
/*  Int 21h extended errors from a failed 440Dh call.  */
const char * dos_ioctl_err_str(int e) {
  switch (-e) {
    case 0x01: return "the driver does not support this function";
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
    default:   return "I/O control failed";
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

/*  Build a FAT12 image. Full format formats tracks first; quick format writes
    only the filesystem data.  */
static void fmt_build_boot(BYTE * sec, const floppy_geom * g,
                           const char * label) {
  DWORD serial;
  int i;
  memzero(sec, SECTOR_SIZE);
  sec[0x00] = 0xEB; sec[0x01] = 0x3C; sec[0x02] = 0x90;   /*  jmpBoot.  */
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
  /*  Hidden sectors and total_sec_32 remain zero.  */
  sec[0x24] = 0x00;                         /*  Drive number.  */
  sec[0x26] = 0x29;                         /*  Extended BPB.  */
  serial = GetTickCount() ^ 0xCAFE0000u;
  sec[0x27] = (BYTE) serial;
  sec[0x28] = (BYTE) (serial >> 8);
  sec[0x29] = (BYTE) (serial >> 16);
  sec[0x2A] = (BYTE) (serial >> 24);
  /*  Space-pad and sanitize the 11-byte label.  */
  Fi(11, sec[0x2B + i] = ' ');
  if (label) {
    for (i = 0; i < 11 && label[i]; i++) {
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
    case 0x01:   /*  Not supported, including VWIN32 failure.  */
    case 0x05:   /*  Access denied.  */
    case 0x0D:   /*  Invalid parameter block.  */
    case 0x13:   /*  Write-protected.  */
    case 0x15:   /*  Not ready.  */
    case 0x16:   /*  Unknown command.  */
    case 0x1A:   /*  Unknown media.  */
    case 0x1F:   /*  General failure.  */
      return 1;
    default:
      return 0;
  }
}

/*  Format every track and mark failures in bad_track.  */
static int fmt_low_level(disk_handle * d, const floppy_geom * g,
                         BYTE * bad_track, HANDLE log, int * failed_out) {
  int i, cyl, head, failed = 0, last_err = 0, rc = 0, held = 0;
  int n_track = g->cyls * g->heads;
  int by_fdc = (G.fmt_method == FMT_BY_FDC && G.vxd != NULL);
  BYTE dt = G.drive_type[d->drive & 1];
  char msg[96];
  LOG_FMT(log, "track formatter: %s\r\n\r\n",
          by_fdc ? "raw FDC (fdchk.vxd, command 4Dh, non-DMA)"
                 : "block driver (Int 21h 440Dh CX=0842h)");
  *failed_out = 0;
  /*  Keep the motor on while formatting.  */
  if (by_fdc) held = vxd_begin(d->drive);
  if (by_fdc && !held) by_fdc = 0;
  for (cyl = 0; cyl < g->cyls; cyl++) {
    for (head = 0; head < g->heads; head++) {
      if (G.abort_req) { rc = -DOS_ERR_BAD_CMD;  goto out; }
      int idx = cyl * g->heads + head;
      DWORD first = (DWORD) (idx * g->spt);
      wsprintfA(msg, "Formatting cylinder %d/%d, head %d...",
                cyl, g->cyls - 1, head);
      ui_status(msg);
      Fi(g->spt, ui_set_state((int) first + i, ST_WRITING));
      fdc_out fo, vo;
      memzero(&fo, sizeof fo);
      memzero(&vo, sizeof vo);
      int frc = by_fdc ? vxd_format_track(d->drive, g, cyl, head, dt, &fo)
                       : disk_format_track(d, cyl, head);
      /*  Log why the raw FDC method failed.  */
      if (idx == 0 && by_fdc)
        LOG_FMT(log,
            "  first track: rc=%d stage=%d msr=%02x "
            "ST=%02x %02x %02x status=%02x n=%d\r\n",
            frc, fo.stage, fo.msr, fo.st0, fo.st1, fo.st2,
            fo.status, fo.result_n);
      /*  Use track zero to test the format method.  */
      if (idx == 0 && frc != 0 && (by_fdc || fmt_is_refusal(frc))) {
        if (!by_fdc)
          LOG_FMT(log, "  block driver could not format the track: %s\r\n",
                  dos_ioctl_err_str(frc));
        rc = frc;  goto out;
      }
      int vrc = 0;
      if (frc == 0) {
        Fi(g->spt, ui_set_state((int) first + i, ST_VERIFY));
        vrc = by_fdc ? vxd_verify_track(d->drive, cyl, head, &vo)
                     : disk_verify_track(d, cyl, head);
      }
      int bad = frc != 0 || vrc != 0;
      BYTE st = bad ? ST_BAD_NEW : ST_GOOD;
      Fi(g->spt, ui_set_state((int) first + i, st));
      if (bad) {
        bad_track[idx] = 1;
        failed++;
        last_err = frc ? frc : vrc;
        if (by_fdc)
          LOG_FMT(log,
              "  cyl %2d head %d  fmt(st=%02x %02x %02x n=%d) "
              "check(rc=%02x st=%02x %02x %02x c=%d n=%d status=%02x)\r\n",
              cyl, head, fo.st0, fo.st1, fo.st2, fo.result_n,
              (BYTE) -vrc, vo.st0, vo.st1, vo.st2, vo.c, vo.result_n,
              vo.status);
        else
          LOG_FMT(log, "  cyl %2d head %d  format=%s  check=%02x\r\n",
                  cyl, head, dos_ioctl_err_str(frc), (BYTE) -vrc);
      }
      G.scanned += (DWORD) g->spt;
      G.current_sec = (int) first;
      ui_progress(G.scanned, (DWORD) g->total_sec);
    }
  }
  /*  The format method failed if every track failed.  */
  if (failed == n_track) {
    LOG_FMT(log, "  every track failed (code %02x); method failed\r\n",
            (BYTE) -last_err);
    rc = last_err ? last_err : -0x1F;  goto out;
  }
  *failed_out = failed;
out:
  if (held) vxd_end();
  return rc;
}
int format_disk(disk_handle * d, const floppy_geom * g,
                const char * label, int style) {
  int i, j;
  DWORD lba;
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
  dos_dev_params saved;
  int saved_ok = 0, rc = 0;
  G.fmt_low_level_ok = 0;
  G.fmt_bad_tracks  = 0;
  G.fmt_sys_bad     = 0;
  memzero(bad_track, sizeof bad_track);
  HANDLE log = log_create("fdchk-format.log");
  {
    SYSTEMTIME t;
    GetLocalTime(&t);
    LOG_FMT(log,
        "fdchk format log\r\n"
        "drive: %c: (%s)   target: %s\r\n"
        "style: %s   date: %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
        'A' + d->drive, drive_type_str(G.drive_type[d->drive & 1]), g->name,
        style == FMT_FULL ? "full (low-level)" : "quick",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  }
  if (!sec || !fat || !root) { rc = -1; goto done; }
  fmt_build_boot(sec, g, label);
  /*  Reserve the first two FAT entries.  */
  fat[0] = g->media_byte; fat[1] = 0xFF; fat[2] = 0xFF;
  /*  Write the optional volume entry.  */
  if (label && label[0]) {
    fat_dirent * e = (fat_dirent *) root;
    memcpy(e->name, sec + 0x2B, 11);
    e->attr = 0x08;
    SYSTEMTIME t;  GetLocalTime(&t);
    e->wrt_t = (WORD) ((t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2));
    e->wrt_d = (WORD) (((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay);
  }
  saved_ok = disk_get_dev_params(d, &saved, 1) == 0;
  /*  Set the target geometry in the driver.  */
  ui_status("Setting drive parameters...");
  int set_rc = disk_set_dev_params(d, g, G.drive_type[d->drive & 1]);
  if (set_rc != 0) {
    LOG_FMT(log, "set-device-parameters: %s; retry without a type\r\n",
            dos_ioctl_err_str(set_rc));
    set_rc = disk_set_dev_params(d, g, DEV_UNKNOWN);
  }
  LOG_FMT(log, "set-device-parameters: %s\r\n\r\n",
          set_rc ? dos_ioctl_err_str(set_rc) : "ok");
  if (set_rc != 0 && style == FMT_FULL) {
    wsprintfA(G.fmt_msg,
        "The driver could not set this disk size (%s). The tracks may not "
        "match the selected size.",
        dos_ioctl_err_str(set_rc));
  }

  if (style == FMT_FULL) {
    int failed = 0;
    int ll = fmt_low_level(d, g, bad_track, log, &failed);
    LOG_FMT(log, "track format: rc=%d, %d of %d tracks failed\r\n",
            ll, failed, g->cyls * g->heads);
    if (G.abort_req) { rc = -1; goto done; }
    if (ll == 0) {
      G.fmt_low_level_ok = 1;
      G.fmt_bad_tracks  = failed;
      /*  Mark data clusters on failed tracks.  */
      Fi(n_track,
        if (!bad_track[i]) continue;
        Fj(g->spt,
          int lba = i * g->spt + j;
          if (lba < data_start) { G.fmt_sys_bad = 1; continue; }
          int cl = 2 + (lba - data_start) / g->sec_per_cluster;
          if (cl >= 2 && cl < n_clusters + 2) fat12_set(fat, cl, 0xFF7);
        ));
      if (failed)
        LOG_FMT(log, "\r\n%d track(s) failed; fdchk marked their clusters "
                     "0xFF7\r\n", failed);
    } else if (G.fmt_method == FMT_BY_FDC && G.vxd) {
      rc = -DOS_ERR_CTRL_FAIL;
      lstrcpyA(G.fmt_msg,
          "Raw FDC format failed on the first track.\r\n\r\n"
          "See fdchk-format.log for ST0/ST1/ST2. If ST1 bit 4 is set, use\r\n"
          "the block-driver method; this machine cannot use non-DMA format.");
      goto done;
    } else if (ll == -0x13 || ll == -0x15) {
      /*  Do not retry protected or absent media.  */
      rc = (ll == -0x13) ? -DOS_ERR_WRITE_PROTECT : -DOS_ERR_TIMEOUT;
      goto done;
    } else {
      ui_status("Cannot format tracks; writing to each sector instead...");
      LOG_FMT(log, "writing to each sector instead\r\n");
      fill = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE * g->spt);
      if (!fill) { rc = -1; goto done; }
      Fi(SECTOR_SIZE * g->spt, fill[i] = 0xF6);
      for (lba = 0; lba < (DWORD) g->total_sec; ) {
        if (G.abort_req) { rc = -1; goto done; }
        WORD n = (WORD) g->spt;
        if (lba + n > (DWORD) g->total_sec) n = (WORD) (g->total_sec - lba);
        rc = disk_write(d, lba, n, fill);
        if (rc) goto done;
        Fi(n, ui_set_state((int) (lba + i), ST_GOOD));
        lba += n;
        G.scanned = lba;
        G.current_sec = (int) lba;
        ui_progress(G.scanned, (DWORD) g->total_sec);
      }
    }
  }
  /*  Write FAT12 filesystem data.  */
  DWORD fat1 = (DWORD) g->reserved_sec;
  DWORD rootlba = fat1 + (DWORD) (g->num_fats * g->fat_size);
  ui_status("Writing boot sector...");
  rc = disk_write(d, 0, 1, sec);
  if (rc) goto done;
  Fi(g->num_fats,
    char m[40];
    wsprintfA(m, "Writing FAT #%d...", i + 1);
    ui_status(m);
    rc = disk_write(d, fat1 + (DWORD) (i * g->fat_size),
                    (WORD) g->fat_size, fat);
    if (rc) goto done;
  );
  ui_status("Writing root directory...");
  rc = disk_write(d, rootlba, (WORD) root_sec, root);
  if (rc) goto done;
  ui_status("Format complete.");
done:
  /*  Restore the drive geometry.  */
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
DWORD WINAPI format_thread(LPVOID arg) {
  (void) arg;
  disk_handle dh;
  int i;
  const floppy_geom * g = G.fmt_geom ? G.fmt_geom : GEOM_1440;
  G.fmt_rc = -1;
  G.fmt_msg[0] = 0;
  geom_apply(g);
  Fi(MAX_SECTORS, G.state[i] = ST_UNTESTED);
  SendMessageA(G.main, WM_APP_REPAINT, 0, 0);
  if (!disk_open(&dh, G.drive)) {
    lstrcpyA(G.fmt_msg, "Cannot open VWIN32.");
    ui_status("Cannot open the drive.");
    goto finish;
  }
  ui_status("Locking the volume...");
  int lock = disk_lock_tiered(&dh);
  if (lock < 0) {
    wsprintfA(G.fmt_msg,
        "Cannot lock drive %c: for formatting (%s).\r\n\r\n"
        "Close programs using the disk and try again.",
        'A' + G.drive, dos_ioctl_err_str(lock));
    ui_status("Cannot lock the drive.");
    goto close;
  }
  BYTE dt = disk_probe_drive_type(&dh);
  if (dt != DEV_UNKNOWN) {
    G.drive_type[G.drive & 1] = dt;
    ui_drive_labels();
  }
  /*  Clear the first media-change error, then check for a disk.  */
  ui_status("Checking for a disk...");
  if (probe_disk_present(&dh) == MEDIA_NONE) {
    char m[220];
    wsprintfA(m,
        "There is no disk in drive %c:, or the drive is not ready.\r\n\r\n"
        "Insert a disk and click Retry. A damaged disk can cause the same error.",
        'A' + G.drive);
    if (ui_prompt(m, "Format floppy",
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
    lstrcpyA(G.fmt_msg, "Formatting stopped after writing began.");
  } else {
    G.fmt_rc = -1;
    if (G.fmt_style == FMT_QUICK)
      wsprintfA(G.fmt_msg,
          "Write failed: %s.\r\n\r\n"
          "Try a full format. It can write new sector address marks.",
          dos_err_str(rc));
    else
      wsprintfA(G.fmt_msg,
          "Write failed: %s.\r\n\r\n"
          "See fdchk-format.log for each track result. Format the disk again "
          "before using it.",
          dos_err_str(rc));
  }
  /*  Add the lock level to the error message.  */
  if (G.fmt_rc != 0 && lock < 1) {
    char m[96];
    wsprintfA(m, "\r\n\r\nVolume lock level: %d.", lock);
    lstrcpynA(G.fmt_msg + lstrlenA(G.fmt_msg), m,
              (int) (sizeof G.fmt_msg) - lstrlenA(G.fmt_msg));
  }
close:
  disk_unlock(&dh);
  disk_close(&dh);
finish:
  InterlockedExchange(&G.running, 0);
  PostMessageA(G.main, WM_APP_DONE, 0, 0);
  return 0;
}
