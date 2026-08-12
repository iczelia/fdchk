/*  fdchk -- Copyright (C) 2026 Kamila Szewczyk
    SPDX-License-Identifier: GPL-3.0-only

    This program is free software; you can redistribute it and/or modify
    it under the terms of version 3 of the GNU General Public License as
    published by the Free Software Foundation.  Version 3 is the only
    version of that license that applies to this program.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

#include "fdchk.h"

/*  Write the embedded VxD blob.  */
static int vxd_extract_to(const char * dir) {
  HRSRC res = FindResourceA(G.hInst, "FDCHKVXD", MAKEINTRESOURCEA(10));
  if (!res) return 0;
  HGLOBAL hg = LoadResource(G.hInst, res);
  if (!hg) return 0;
  void * data = LockResource(hg);
  DWORD size  = SizeofResource(G.hInst, res);
  if (!data || !size || !dir || !dir[0]) return 0;
  int n = lstrlenA(dir);
  if (dir[n - 1] == '\\')  wsprintfA(G.vxd_tmp_path, "%sfdchk.vxd", dir);
  else                     wsprintfA(G.vxd_tmp_path, "%s\\fdchk.vxd", dir);
  HANDLE f = CreateFileA(G.vxd_tmp_path, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE)
    { G.vxd_tmp_path[0] = 0;  return 0; }
  DWORD cb;
  BOOL ok = WriteFile(f, data, size, &cb, NULL) && cb == size;
  CloseHandle(f);
  if (!ok)
    { DeleteFileA(G.vxd_tmp_path);  G.vxd_tmp_path[0] = 0;  return 0; }
  return 1;
}
static HANDLE g_vxd_log = INVALID_HANDLE_VALUE;
static void vxd_restore_controller(void);
static int vxd_answers(HANDLE h) {
  FdcOut o;  memzero(&o, sizeof o);  DWORD cb = 0;
  if (!h || h == INVALID_HANDLE_VALUE) return 0;
  if (DeviceIoControl(h, IOCTL_FDC_IDENT, NULL, 0, &o, sizeof o, &cb, NULL)
      && o.status == 0x00) {
    if (o.st0 == FDC_VXD_VERSION) return 1;
    LOG_FMT(g_vxd_log,
            "    identify: interface version %02x, wanted %02x - an older\r\n"
            "    fdchk.vxd is still resident\r\n", o.st0, FDC_VXD_VERSION);
    return -1;
  }
  /*  It answered something other than the identify reply.  */
  LOG_FMT(g_vxd_log, "    identify: err=%lu cb=%lu reply=%02x %02x\r\n",
          GetLastError(), cb, o.status, o.st0);
  return 0;
}
/*  Try one name in the VxD namespace and keep it only if it answers.
    -1 means a stale driver was found and unloaded, so it is worth one
    more go at the same name.  */
static int vxd_try(const char * name) {
  DWORD err_new = 0, err_open = 0;
  int ans;
  HANDLE h = CreateFileA(name, 0, 0, NULL, CREATE_NEW,
                         FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    err_new = GetLastError();
    h = CreateFileA(name, 0, 0, NULL, OPEN_EXISTING,
                    FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) err_open = GetLastError();
  }
  if (h == INVALID_HANDLE_VALUE) {
    LOG_FMT(g_vxd_log, "  %s: open failed (CREATE_NEW err=%lu, "
                       "OPEN_EXISTING err=%lu)\r\n",
            name, err_new, err_open);
    return 0;
  }
  ans = vxd_answers(h);
  if (ans != 1) {
    if (ans == 0)
      LOG_FMT(g_vxd_log,
              "  %s: opened but did not identify itself - a file handle,\r\n"
              "    not a device handle (err=%lu)\r\n",
              name, GetLastError());
    /*  Closing the last handle is what makes VXDLDR unload it.  */
    CloseHandle(h);
    return ans;
  }
  LOG_FMT(g_vxd_log, "  %s: loaded and answering\r\n", name);
  G.hVxd = h;
  return 1;
}
/*  vxd_try, plus one retry if the first go turned out a stale driver that
    we have now unloaded.  */
static int vxd_try_twice(const char * name) {
  int rc = vxd_try(name);
  if (rc != -1) return rc == 1;
  LOG_FMT(g_vxd_log, "  %s: unloaded the stale one, retrying\r\n", name);
  Sleep(100);
  return vxd_try(name) == 1;
}
/*  Open the VxD.  */
int vxd_open(void) {
  char dirs[4][MAX_PATH];
  int n_dirs = 0, ok = 0;
  G.hVxd = NULL;
  G.vxd_tmp_path[0] = 0;
  g_vxd_log = log_create("fdchk-vxd.log");
  /*  Directories VXDLDR will search, best first.  */
  if (GetSystemDirectoryA(dirs[n_dirs], MAX_PATH))       n_dirs++;
  if (GetWindowsDirectoryA(dirs[n_dirs], MAX_PATH))      n_dirs++;
  {
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, MAX_PATH)) {
      int i = lstrlenA(exe);
      while (i > 0 && exe[i - 1] != '\\') --i;
      if (i > 1) {
        exe[i - 1] = 0;
        lstrcpynA(dirs[n_dirs], exe, MAX_PATH);
        n_dirs++;
      }
    }
  }
  /*  Remove previous instances of the VxD from the system.  */
  for (int i = 0; i < n_dirs; ++i) {
    char stale[MAX_PATH];
    int n = lstrlenA(dirs[i]);
    if (n && dirs[i][n - 1] == '\\') wsprintfA(stale, "%sfdchk.vxd", dirs[i]);
    else                             wsprintfA(stale, "%s\\fdchk.vxd", dirs[i]);
    if (DeleteFileA(stale))
      LOG_FMT(g_vxd_log, "removed stale %s\r\n", stale);
  }
  for (int i = 0; i < n_dirs && !ok; ++i) {
    if (!vxd_extract_to(dirs[i])) {
      LOG_FMT(g_vxd_log, "could not write to %s\r\n", dirs[i]);
      continue;
    }
    LOG_FMT(g_vxd_log, "extracted to %s\r\nload attempts:\r\n",
            G.vxd_tmp_path);
    /*  Name only - VXDLDR searches for it; a path here yields error 2.  */
    if (vxd_try_twice("\\\\.\\FDCHK.VXD")) { ok = 1; break; }
    if (vxd_try_twice("\\\\.\\fdchk.vxd")) { ok = 1; break; }
    DeleteFileA(G.vxd_tmp_path);
    G.vxd_tmp_path[0] = 0;
  }
  if (!ok) {
    G.hVxd = NULL;
    if (G.vxd_tmp_path[0]) DeleteFileA(G.vxd_tmp_path);
  }
  if (ok) vxd_restore_controller();   /*  clear anything a past run left  */
  LOG_FMT(g_vxd_log, "result: %s\r\n", ok ? "VxD active" : "no VxD");
  if (g_vxd_log != INVALID_HANDLE_VALUE) {
    CloseHandle(g_vxd_log);
    g_vxd_log = INVALID_HANDLE_VALUE;
  }
  return ok;
}
/*  Put the controller back to the state the system's own floppy driver
    expects: motors off, interrupt and DMA gate open.  */
static void vxd_restore_controller(void) {
  FdcIn in_buf;  FdcOut o;
  if (!G.hVxd || G.hVxd == INVALID_HANDLE_VALUE) return;
  memzero(&in_buf, sizeof in_buf);
  vxd_call(IOCTL_FDC_END, &in_buf, &o);
}
void vxd_close(void) {
  vxd_end();
  vxd_restore_controller();
  if (G.hVxd && G.hVxd != INVALID_HANDLE_VALUE)
    { CloseHandle(G.hVxd);  G.hVxd = NULL; }
  if (G.vxd_tmp_path[0])
    { DeleteFileA(G.vxd_tmp_path);  G.vxd_tmp_path[0] = 0; }
}
/*  Send an IOCTL to the VxD.  1 on success, 0 on failure.  */
int vxd_call(DWORD ioctl, const FdcIn * in_buf, FdcOut * out_buf) {
  DWORD cb = 0;
  if (!G.hVxd || G.hVxd == INVALID_HANDLE_VALUE) return 0;
  return DeviceIoControl(G.hVxd, ioctl,
      (LPVOID) in_buf, in_buf ? sizeof(FdcIn) : 0,
      out_buf, out_buf ? sizeof(FdcOut) : 0, &cb, NULL) ? 1 : 0;
}

/*  Controller sessions.  */
static int g_fdc_depth = 0;
static int g_fdc_drive = -1;

int vxd_begin(int drive) {
  FdcIn in_buf;  FdcOut o;
  if (!G.hVxd) return 0;
  if (g_fdc_depth > 0) {
    if (drive != g_fdc_drive) return 0;   /*  no switching mid-session  */
    g_fdc_depth++;
    return 1;
  }
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.flags = FDC_F_MOTOR | FDC_F_GATE;
  if (!vxd_call(IOCTL_FDC_MOTOR, &in_buf, &o)) return 0;
  Sleep(FDC_SPINUP_MS);                   /*  let the spindle reach speed  */
  g_fdc_depth = 1;
  g_fdc_drive = drive;
  return 1;
}
void vxd_end(void) {
  FdcIn in_buf;  FdcOut o;
  if (!G.hVxd || g_fdc_depth <= 0) return;
  if (--g_fdc_depth > 0) return;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) g_fdc_drive;
  vxd_call(IOCTL_FDC_END, &in_buf, &o);   /*  motors off, gate open  */
  g_fdc_drive = -1;
}

/*  Reopen the controller's interrupt and DMA gate.  */
static void fdc_gate_open(int drive) {
  FdcIn in_buf;  FdcOut o;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.flags = FDC_F_MOTOR | FDC_F_GATE;
  vxd_call(IOCTL_FDC_MOTOR, &in_buf, &o);
}

/*  Drive a command the VxD has started to completion.  */
static int vxd_poll_until(DWORD ioctl, const FdcIn * in_buf, FdcOut * out,
                          DWORD limit_ms) {
  DWORD t0 = now_ms();
  for (;;) {
    if (!vxd_call(ioctl, in_buf, out)) return -1;
    if (out->status != FDC_ST_BUSY) return 0;
    DWORD el = now_ms() - t0;
    if (el > limit_ms) return -0xE7;      /*  it never came back  */
    if (el < 20) Sleep(0);
    else         Sleep(1);
  }
}
/*  Reset the controller.  */
int vxd_reset(FdcOut * out) {
  FdcOut o;
  if (out) memzero(out, sizeof *out);
  if (!G.hVxd) return -1;
  if (!vxd_call(IOCTL_FDC_RESET, NULL, &o)) return -1;
  if (out) *out = o;
  return 0;
}
/*  Seek to track 0 and wait for the head to get there.  */
int vxd_recalibrate(int drive, FdcOut * out) {
  FdcIn in_buf;  FdcOut o;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.hVxd) return -1;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  if (!vxd_call(IOCTL_FDC_RECAL, &in_buf, &o)) { fdc_gate_open(drive); return -1; }
  if (o.status) {                         /*  never got started  */
    fdc_gate_open(drive);
    if (out) *out = o;
    return -(int) o.status;
  }
  in_buf.flags = FDC_F_SENSE;
  rc = vxd_poll_until(IOCTL_FDC_POLL, &in_buf, &o, FDC_SEEK_MS);
  if (rc != 0) { vxd_reset(NULL);  fdc_gate_open(drive); }
  if (out) *out = o;
  return rc;
}
/*  Step to a cylinder and wait for the seek to finish.  */
int vxd_seek(int drive, int cyl, int head, FdcOut * out) {
  FdcIn in_buf;  FdcOut o;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.hVxd) return -1;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.head  = (BYTE) head;
  in_buf.cyl   = (BYTE) cyl;
  if (!vxd_call(IOCTL_FDC_SEEK, &in_buf, &o)) { fdc_gate_open(drive); return -1; }
  if (o.status) {                         /*  never got started  */
    fdc_gate_open(drive);
    if (out) *out = o;
    return -(int) o.status;
  }
  in_buf.flags = FDC_F_SENSE;
  rc = vxd_poll_until(IOCTL_FDC_POLL, &in_buf, &o, FDC_SEEK_MS);
  if (rc != 0) { vxd_reset(NULL);  fdc_gate_open(drive); }
  if (out) *out = o;
  return rc;
}
/*  Report the first sector header to pass under the head.  */
int vxd_read_id(int drive, int head, FdcOut * out) {
  FdcIn in_buf;  FdcOut o;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.hVxd) return -1;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.head  = (BYTE) head;
  if (!vxd_call(IOCTL_FDC_READID, &in_buf, &o)) { fdc_gate_open(drive); return -1; }
  if (o.status) {                         /*  never got started  */
    fdc_gate_open(drive);
    if (out) *out = o;
    return -(int) o.status;
  }
  /*  A header can take two revolutions to come round.  */
  rc = vxd_poll_until(IOCTL_FDC_RESULT, &in_buf, &o, FDC_RESULT_MS);
  if (rc != 0) { vxd_reset(NULL);  fdc_gate_open(drive); }
  if (out) *out = o;
  if (rc == 0 && o.status) return -(int) o.status;
  return rc;
}
/*  VxD type query.  */
BYTE vxd_drive_type(int drive) {
  FdcIn in_buf;  FdcOut o;
  if (!G.hVxd) return DEV_UNKNOWN;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  if (!vxd_call(IOCTL_FDC_CMOS, &in_buf, &o)) return DEV_UNKNOWN;
  BYTE t = (drive == 0) ? (BYTE) (o.cmos >> 4) : (BYTE) (o.cmos & 0x0F);
  switch (t) {
    case CMOS_FD_360K:  return DEV_360K;
    case CMOS_FD_1200K: return DEV_1200K;
    case CMOS_FD_720K:  return DEV_720K;
    case CMOS_FD_1440K: return DEV_1440K;
    case CMOS_FD_2880K: return DEV_2880K;
    default:            return DEV_UNKNOWN;
  }
}
/*  VxD insertion check.  */
int vxd_sense_media(int drive, FdcOut * out) {
  FdcIn in_buf;  FdcOut o;  BYTE before, after;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.hVxd) return -2;
  if (!vxd_begin(drive)) return -2;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  vxd_call(IOCTL_FDC_DIR, &in_buf, &o);
  before = o.dir_before;
  /*  Seek to 1 then back to 0.  Whichever cylinder the head starts on, one
      of the two moves it, so a step pulse is guaranteed.  */
  vxd_seek(drive, 1, 0, &o);
  vxd_seek(drive, 0, 0, &o);
  vxd_call(IOCTL_FDC_DIR, &in_buf, &o);
  after = o.dir_after;
  rc = vxd_read_id(drive, 0, &o);
  vxd_end();
  /*  Leaves ST0/ST1/ST2 and C/H/R/N holding the READ ID result.  */
  o.dir_before = before;
  o.dir_after  = after;
  if (out) *out = o;
  if (rc == 0 && o.result_n >= 7 && (o.st0 & 0xC0) == 0) return MEDIA_PRESENT;
  if ((before & FDC_DIR_DSKCHG) && (after & FDC_DIR_DSKCHG))
    return MEDIA_NONE;
  return MEDIA_UNREADABLE;
}
/*  Lay down one track with the FDC's own FORMAT TRACK command.  The seek
    happens here rather than in the driver: only the execution phase, which
    has to feed the controller in real time for one revolution, genuinely
    needs the CPU to itself.  */
int vxd_format_track(int drive, const FloppyGeom * g, int cyl, int head,
                    BYTE dev_type, FdcOut * out) {
  FdcIn in_buf;  FdcOut o;  int ok;
  if (out) memzero(out, sizeof *out);
  if (!G.hVxd) return -1;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive     = (BYTE) drive;
  in_buf.head      = (BYTE) head;
  in_buf.cyl       = (BYTE) cyl;
  in_buf.sec       = (BYTE) g->spt;      /*  SC  */
  in_buf.size_code = 2;                  /*  N = 2, 512-byte sectors  */
  in_buf.gap3      = g->gap3_fmt;
  in_buf.filler    = 0xF6;
  in_buf.rate      = geom_rate_in_drive(g, dev_type);
  in_buf.spec1     = FDC_SPECIFY_1;
  in_buf.spec2     = FDC_SPECIFY_2;
  if (!vxd_begin(drive)) return -1;
  /*  Rate first, so the step timings the seek uses are the right ones.  */
  vxd_call(IOCTL_FDC_SPECIFY, &in_buf, &o);
  vxd_seek(drive, cyl, head, &o);
  ok = vxd_call(IOCTL_FDC_FORMAT, &in_buf, &o);
  vxd_end();
  if (!ok) return -1;
  if (out) *out = o;
  if (o.status) return -(int) o.status;
  if (o.result_n < 7) return -0xE7;      /*  no result phase  */
  if (o.st0 & 0xC0) return -(int) (o.st0 ? o.st0 : 0xE8);
  if (o.st1) return -0xEA;
  return 0;
}
/*  Confirm a track carries readable sector IDs at the cylinder we meant.  */
int vxd_verify_track(int drive, int cyl, int head, FdcOut * out) {
  FdcOut o;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.hVxd) return -1;
  if (!vxd_begin(drive)) return -1;
  vxd_seek(drive, cyl, head, &o);
  rc = vxd_read_id(drive, head, &o);
  vxd_end();
  if (out) *out = o;
  if (rc != 0)           return -1;
  if (o.result_n < 7)    return -0xE7;
  if (o.st0 & 0xC0)      return -(int) o.st0;
  if (o.c != (BYTE) cyl) return -0xE9;   /*  wrong cylinder  */
  return 0;
}
/*  Decode an FDC ST byte triplet into a short summary string.  */
const char * st_summary(BYTE st0, BYTE st1, BYTE st2) {
  if (st2 & 0x10)            return "WRONG CYLINDER (head misaligned)";
  if (st2 & 0x20)            return "CRC error in data field";
  if (st1 & 0x20)            return "CRC error in ID field";
  if (st1 & 0x01)            return "missing ID address mark";
  if (st2 & 0x01)            return "missing data address mark";
  if (st1 & 0x04)            return "no data (track unformatted?)";
  if (st1 & 0x02)            return "not writable";
  if (st1 & 0x10)            return "DMA over-run";
  if (st1 & 0x80)            return "end of cylinder";
  if (st0 & 0x10)            return "equipment check / drive fault";
  if (st0 & 0x08)            return "not ready";
  if ((st0 & 0xC0) == 0x40)  return "abnormal termination";
  if ((st0 & 0xC0) == 0x80)  return "invalid command";
  if ((st0 & 0xC0) == 0xC0)  return "polling-state change";
  return "OK";
}
