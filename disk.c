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
    BIOS Int 13h passthrough for diagnostics, and the FAT12 formatter.  */

#include "fdchk.h"

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

/*  Issue Int 21h AX=440Dh CX=08??h to VWIN32 (lock / unlock / get-info).  */
static int vwin32_ioctl_440d(DiskHandle * d, WORD cx, BYTE bh, BYTE dx) {
  DIOC_REGISTERS r;
  DWORD cb = 0;
  memzero(&r, sizeof r);
  r.reg_EAX   = 0x440D;
  r.reg_EBX   = (DWORD) (BYTE) (d->drive + 1);   /*  1 = A:, 2 = B:  */
  r.reg_EBX  |= (DWORD) bh << 8;
  r.reg_ECX   = cx;
  r.reg_EDX   = dx;
  r.reg_Flags = 1;
  if (!DeviceIoControl(d->hVwin32, VWIN32_DIOC_DOS_IOCTL,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.reg_Flags & 1) return -(int) (r.reg_EAX & 0xFF);
  return 0;
}

/*  Lock the logical volume for raw access (Int 21h 440Dh, CX=084Ah).
    The documented Win9x protocol is to escalate: the level-0 lock is
    granted only when no files are open on the volume and it flushes the
    OS buffers; higher levels add exclusivity.  Releasing the lock makes
    Windows discard its cached FAT/directory, so our raw writes stay
    coherent.  A level-0 failure means the volume is in use - the caller
    must abort rather than write without it.
    max_level: 0 for read-only access, up to 3 for an exclusive write.  */
int disk_lock(DiskHandle * d, int max_level) {
  if (d->locked) return 0;
  for (int lvl = 0; lvl <= max_level; ++lvl) {
    int r = vwin32_ioctl_440d(d, 0x084A, (BYTE) lvl, 1);
    if (r != 0) {
      while (lvl-- > 0) vwin32_ioctl_440d(d, 0x086A, 0, 0);  /* unwind */
      return r;
    }
  }
  d->locked = 1;
  d->lock_levels = max_level + 1;
  return 0;
}

int disk_unlock(DiskHandle * d) {
  if (!d->locked) return 0;
  int r = 0;
  for (int i = 0; i < d->lock_levels; ++i)
    r = vwin32_ioctl_440d(d, 0x086A, 0, 0);
  d->locked = 0;
  d->lock_levels = 0;
  return r;
}

/*  Read/write an absolute sector range.  0 on success, negative DOS error.  */
int disk_read(DiskHandle * d, DWORD lba, WORD count, void * buf) {
  DISKIO io;
  DIOC_REGISTERS r;
  DWORD cb = 0;
  io.dwStartSector = lba;
  io.wSectors      = count;
  io.dwBuffer      = (DWORD) (ULONG_PTR) buf;
  memzero(&r, sizeof r);
  r.reg_EAX   = (DWORD) (BYTE) d->drive;
  r.reg_EBX   = (DWORD) (ULONG_PTR) &io;
  r.reg_ECX   = 0xFFFF;
  r.reg_Flags = 1;
  if (!DeviceIoControl(d->hVwin32, VWIN32_DIOC_DOS_INT25,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.reg_Flags & 1) return -(int) (r.reg_EAX & 0xFF);
  return 0;
}

int disk_write(DiskHandle * d, DWORD lba, WORD count, const void * buf) {
  DISKIO io;
  DIOC_REGISTERS r;
  DWORD cb = 0;
  io.dwStartSector = lba;
  io.wSectors      = count;
  io.dwBuffer      = (DWORD) (ULONG_PTR) buf;
  memzero(&r, sizeof r);
  r.reg_EAX   = (DWORD) (BYTE) d->drive;
  r.reg_EBX   = (DWORD) (ULONG_PTR) &io;
  r.reg_ECX   = 0xFFFF;
  r.reg_Flags = 1;
  if (!DeviceIoControl(d->hVwin32, VWIN32_DIOC_DOS_INT26,
                       &r, sizeof r, &r, sizeof r, &cb, NULL))
    return -1;
  if (r.reg_Flags & 1) return -(int) (r.reg_EAX & 0xFF);
  return 0;
}

/*  Multi-sector probe to tell "no disk" from "broken disk".  A broken
    disk usually has some readable sectors (CRC errors are local); a truly
    absent disk fails uniformly with a no-media code everywhere.
      1  = medium present (at least partially readable)
      0  = no disk (every probe gave a no-media error)
     -e  = ambiguous (every probe failed with other codes)  */
int probe_disk_present(DiskHandle * d) {
  static const DWORD probes[] = { 0, 33, 1440, 2879 };
  BYTE buf[SECTOR_SIZE];
  int n = (int) (sizeof probes / sizeof probes[0]);
  int n_ok = 0, n_no_media = 0, last_err = 0;
  for (int i = 0; i < n; ++i) {
    int rc = disk_read(d, probes[i], 1, buf);
    if (rc == 0) { n_ok++; continue; }
    int e = -rc;
    last_err = e;
    if (e == 0x02 || e == 0x06 || e == 0x80 || e == 0xAA) n_no_media++;
  }
  if (n_ok > 0)          return 1;
  if (n_no_media == n)   return 0;
  return -last_err;
}

/*  BIOS Int 13h passthrough - the diagnostic-mode fallback.

    VWIN32_DIOC_DOS_INT13 issues raw BIOS disk calls.  BIOS drains the FDC
    result phase before returning, so we never see ST0/ST1/ST2 directly -
    but the BIOS digest in AH reverse-decodes well enough to tell a
    Wrong-Cylinder fault from an ordinary CRC error.  */

/*  cyl is 0..1023 in BIOS 10-bit form.  Returns 0 on success, -AH on
    failure; ah_out (optional) receives the raw AH for decoding.  */
int bios_int13(DiskHandle * d, BYTE ah_in, BYTE al_in,
               int cyl, int head, int sec, void * buf, BYTE * ah_out) {
  DIOC_REGISTERS r;
  DWORD cb;
  memzero(&r, sizeof r);
  r.reg_EAX = ((DWORD) ah_in << 8) | al_in;
  /*  CL[5:0] = sector (1-based); CL[7:6] = cyl[9:8]; CH = cyl[7:0].  */
  r.reg_ECX = (((DWORD) cyl & 0xFFu) << 8)
            | (((DWORD) cyl & 0x300u) >> 2)
            | ((DWORD) sec & 0x3Fu);
  r.reg_EDX = ((DWORD) (BYTE) head << 8) | (DWORD) (BYTE) d->drive;
  r.reg_EBX = buf ? (DWORD) (ULONG_PTR) buf : 0;
  r.reg_Flags = 1;
  if (!DeviceIoControl(d->hVwin32, VWIN32_DIOC_DOS_INT13,
                       &r, sizeof r, &r, sizeof r, &cb, NULL)) {
    if (ah_out) *ah_out = 0xFE;
    return -1;
  }
  BYTE ah = (BYTE) ((r.reg_EAX >> 8) & 0xFFu);
  if (ah_out) *ah_out = ah;
  if (r.reg_Flags & 1) return -(int) ah;
  return 0;
}

/*  Reverse-decode a BIOS AH back to a best-guess (ST0, ST1, ST2).  Lossy,
    but enough to surface Wrong-Cylinder (AH=0x40) vs CRC (AH=0x10).  */
void bios_ah_to_st(BYTE ah, BYTE * st0, BYTE * st1, BYTE * st2) {
  *st0 = *st1 = *st2 = 0;
  if (ah == 0) return;
  *st0 |= 0x40;                                  /*  abnormal termination  */
  switch (ah) {
    case 0x02: *st1 |= 0x01; break;              /*  missing address mark  */
    case 0x03: *st1 |= 0x02; break;              /*  not writable  */
    case 0x04: *st1 |= 0x04; break;              /*  no data / not found  */
    case 0x05: *st0 |= 0x10; break;              /*  reset failed  */
    case 0x06: *st0 |= 0x40; break;              /*  media changed  */
    case 0x08: *st1 |= 0x10; break;              /*  DMA over-run  */
    case 0x09: *st1 |= 0x80; break;              /*  end of cylinder  */
    case 0x0C: *st2 |= 0x02; break;              /*  invalid media  */
    case 0x10: *st1 |= 0x20; *st2 |= 0x20; break; /*  CRC in ID or data  */
    case 0x20: *st0 |= 0x10; break;              /*  controller failure  */
    case 0x40: *st2 |= 0x10; break;              /*  wrong cylinder  */
    case 0x80:
    case 0xAA: *st0 |= 0x08; break;              /*  timeout / not ready  */
    case 0xCC: *st0 |= 0x10; break;              /*  write fault  */
    default:   *st0 |= 0x40; break;
  }
}

const char * bios_ah_str(BYTE ah) {
  switch (ah) {
    case 0x00: return "OK";
    case 0x01: return "bad command";
    case 0x02: return "no address mark (unformatted track?)";
    case 0x03: return "write-protected";
    case 0x04: return "sector not found";
    case 0x05: return "reset failed";
    case 0x06: return "media changed";
    case 0x07: return "drive parameter activity failed";
    case 0x08: return "DMA overrun";
    case 0x09: return "DMA crosses 64K boundary";
    case 0x0C: return "invalid media";
    case 0x10: return "CRC / ECC data error";
    case 0x20: return "controller failure";
    case 0x40: return "WRONG CYLINDER (head misalignment)";
    case 0x80: return "drive not ready / timeout";
    case 0xAA: return "drive not ready";
    case 0xBB: return "undefined error";
    case 0xCC: return "write fault";
    case 0xE0: return "status register error";
    case 0xFE: return "VWIN32 IOCTL refused";
    case 0xFF: return "INT 13h unavailable (NT?)";
    default:   return "unknown error";
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

/*  FAT12 quick / full format.  Always lays down a standard 1.44 MB disk.  */

int format_disk(DiskHandle * d, const char * label, int full_format) {
  BYTE * sec  = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  BYTE * fat  = (BYTE *) LocalAlloc(LPTR, 9 * SECTOR_SIZE);
  BYTE * root = (BYTE *) LocalAlloc(LPTR, 14 * SECTOR_SIZE);
  BYTE * fill = NULL;
  int rc = 0;
  if (!sec || !fat || !root) { rc = -1; goto done; }

  /*  Boot sector / BPB - standard 1.44 MB FAT12.  */
  static const BYTE bpb_template[] = {
    0xEB, 0x3C, 0x90,                       /*  jmpBoot  */
    'M','S','W','I','N','4','.','1',         /*  OEM name  */
    0x00, 0x02,                             /*  bytes/sector = 512  */
    0x01,                                   /*  sectors/cluster = 1  */
    0x01, 0x00,                             /*  reserved sectors = 1  */
    0x02,                                   /*  FAT count  */
    0xE0, 0x00,                             /*  root entries = 224  */
    0x40, 0x0B,                             /*  total sectors = 2880  */
    0xF0,                                   /*  media byte  */
    0x09, 0x00,                             /*  sectors/FAT = 9  */
    0x12, 0x00,                             /*  sectors/track = 18  */
    0x02, 0x00,                             /*  heads = 2  */
    0x00, 0x00, 0x00, 0x00,                 /*  hidden sectors = 0  */
    0x00, 0x00, 0x00, 0x00,                 /*  total_sec_32 = 0  */
    0x00,                                   /*  drive number  */
    0x00,                                   /*  reserved  */
    0x29                                    /*  extended boot signature  */
  };
  memcpy(sec, bpb_template, sizeof bpb_template);
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

  /*  FAT: entry 0 = media byte | 0xF00, entry 1 = end-of-chain reserve.  */
  fat[0] = 0xF0; fat[1] = 0xFF; fat[2] = 0xFF;

  /*  Root directory: if a label was given, stamp it as a volume entry.  */
  if (label) {
    FatDirEntry * e = (FatDirEntry *) root;
    memcpy(e->name, sec + 0x2B, 11);
    e->attr = 0x08;                         /*  volume label  */
    SYSTEMTIME t;
    GetLocalTime(&t);
    e->wrt_t = (WORD) ((t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2));
    e->wrt_d = (WORD) (((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay);
  }

  /*  Full format: pre-fill the data area with 0xF6.  */
  if (full_format) {
    fill = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE * 18);
    if (!fill) { rc = -1; goto done; }
    for (int i = 0; i < SECTOR_SIZE * 18; ++i) fill[i] = 0xF6;
    ui_status("Full format: writing 0xF6 to data area...");
    for (DWORD lba = 33; lba < 2880; ) {
      if (G.abort_req) { rc = -1; goto done; }
      WORD n = 18;
      if (lba + n > 2880) n = (WORD) (2880 - lba);
      rc = disk_write(d, lba, n, fill);
      if (rc) goto done;
      lba += n;
    }
  }

  /*  Write the metadata.  */
  ui_status("Writing boot sector...");
  rc = disk_write(d,  0,  1, sec);   if (rc) goto done;
  ui_status("Writing FAT #1...");
  rc = disk_write(d,  1,  9, fat);   if (rc) goto done;
  ui_status("Writing FAT #2...");
  rc = disk_write(d, 10,  9, fat);   if (rc) goto done;
  ui_status("Writing root directory...");
  rc = disk_write(d, 19, 14, root);  if (rc) goto done;
  ui_status("Format complete.");

done:
  if (fill) LocalFree(fill);
  if (sec)  LocalFree(sec);
  if (fat)  LocalFree(fat);
  if (root) LocalFree(root);
  return rc;
}
