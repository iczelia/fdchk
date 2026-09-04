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

#include "fdchk.h"

/*  Load the embedded raw FDC driver.  */

static int extract(const char * dir) {
  HRSRC res = FindResourceA(G.instance, "FDCHKVXD", MAKEINTRESOURCEA(10));
  if (!res) return 0;
  HGLOBAL hg = LoadResource(G.instance, res);
  if (!hg) return 0;
  void * data = LockResource(hg);
  DWORD size  = SizeofResource(G.instance, res);
  if (!data || !size || !dir || !dir[0]) return 0;
  int n = lstrlenA(dir);
  if (dir[n - 1] == '\\')  wsprintfA(G.vxd_tmp_path, "%sfdchk.vxd", dir);
  else                     wsprintfA(G.vxd_tmp_path, "%s\\fdchk.vxd", dir);
  HANDLE f = CreateFileA(G.vxd_tmp_path, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) { G.vxd_tmp_path[0] = 0;  return 0; }
  DWORD cb;
  BOOL ok = WriteFile(f, data, size, &cb, NULL) && cb == size;
  CloseHandle(f);
  if (!ok) {
    DeleteFileA(G.vxd_tmp_path);  G.vxd_tmp_path[0] = 0;  return 0;
  }
  return 1;
}
static HANDLE vxd_log = INVALID_HANDLE_VALUE;
static void restore_controller(void);
static int answers(HANDLE h) {
  fdc_out o;  memzero(&o, sizeof o);  DWORD cb = 0;
  if (!h || h == INVALID_HANDLE_VALUE) return 0;
  if (DeviceIoControl(h, IOCTL_FDC_IDENT, NULL, 0, &o, sizeof o, &cb, NULL)
      && o.status == 0x00) {
    if (o.st0 == FDC_VXD_VERSION) return 1;
    LOG_FMT(vxd_log,
            "    identify: interface %02x, need %02x; old driver still loaded\r\n",
            o.st0, FDC_VXD_VERSION);
    return -1;
  }
  LOG_FMT(vxd_log, "    identify: error=%lu bytes=%lu reply=%02x %02x\r\n",
          GetLastError(), cb, o.status, o.st0);
  return 0;
}
/*  Open and identify one VxD name. Return -1 after unloading an old copy.  */
static int try_name(const char * name) {
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
    LOG_FMT(vxd_log, "  %s: cannot open (CREATE_NEW=%lu, OPEN_EXISTING=%lu)\r\n",
            name, err_new, err_open);
    return 0;
  }
  ans = answers(h);
  if (ans != 1) {
    if (ans == 0)
      LOG_FMT(vxd_log,
              "  %s: opened as a file, not a device (error=%lu)\r\n",
              name, GetLastError());
    /*  VXDLDR unloads after the last handle closes.  */
    CloseHandle(h);
    return ans;
  }
  LOG_FMT(vxd_log, "  %s: ready\r\n", name);
  G.vxd = h;
  return 1;
}
static int try_twice(const char * name) {
  int rc = try_name(name);
  if (rc != -1) return rc == 1;
  LOG_FMT(vxd_log, "  %s: old driver unloaded; retrying\r\n", name);
  Sleep(100);
  return try_name(name) == 1;
}

int vxd_open(void) {
  char dirs[4][MAX_PATH];
  int i, n_dirs = 0, ok = 0;
  G.vxd = NULL;
  G.vxd_tmp_path[0] = 0;
  vxd_log = log_create("fdchk-vxd.log");
  /*  Search in VXDLDR order.  */
  if (GetSystemDirectoryA(dirs[n_dirs], MAX_PATH))       n_dirs++;
  if (GetWindowsDirectoryA(dirs[n_dirs], MAX_PATH))      n_dirs++;
  {
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, MAX_PATH)) {
      int i = lstrlenA(exe);
      while (i > 0 && exe[i - 1] != '\\') i--;
      if (i > 1) {
        exe[i - 1] = 0;
        lstrcpynA(dirs[n_dirs], exe, MAX_PATH);
        n_dirs++;
      }
    }
  }
  /*  Delete driver files from earlier runs.  */
  Fi(n_dirs,
    char old_path[MAX_PATH];
    int n = lstrlenA(dirs[i]);
    if (n && dirs[i][n - 1] == '\\') wsprintfA(old_path, "%sfdchk.vxd", dirs[i]);
    else                             wsprintfA(old_path, "%s\\fdchk.vxd", dirs[i]);
    if (DeleteFileA(old_path))
      LOG_FMT(vxd_log, "deleted old driver file %s\r\n", old_path);
  );
  Fi(n_dirs,
    if (ok) break;
    if (!extract(dirs[i])) {
      LOG_FMT(vxd_log, "cannot write to %s\r\n", dirs[i]);
      continue;
    }
    LOG_FMT(vxd_log, "wrote %s\r\nload attempts:\r\n",
            G.vxd_tmp_path);
    /*  VXDLDR accepts the device name, not its path.  */
    if (try_twice("\\\\.\\FDCHK.VXD")) { ok = 1; break; }
    if (try_twice("\\\\.\\fdchk.vxd")) { ok = 1; break; }
    DeleteFileA(G.vxd_tmp_path);
    G.vxd_tmp_path[0] = 0;
  );
  if (!ok) {
    G.vxd = NULL;
    if (G.vxd_tmp_path[0]) DeleteFileA(G.vxd_tmp_path);
  }
  if (ok) restore_controller();
  LOG_FMT(vxd_log, "result: %s\r\n", ok ? "loaded" : "not loaded");
  if (vxd_log != INVALID_HANDLE_VALUE) {
    CloseHandle(vxd_log);
    vxd_log = INVALID_HANDLE_VALUE;
  }
  return ok;
}
/*  Stop motors and reopen the interrupt and DMA gate.  */
static void restore_controller(void) {
  fdc_in in_buf;  fdc_out o;
  if (!G.vxd || G.vxd == INVALID_HANDLE_VALUE) return;
  memzero(&in_buf, sizeof in_buf);
  vxd_call(IOCTL_FDC_END, &in_buf, &o);
}
void vxd_close(void) {
  vxd_end();
  restore_controller();
  if (G.vxd && G.vxd != INVALID_HANDLE_VALUE) { CloseHandle(G.vxd);  G.vxd = NULL; }
  if (G.vxd_tmp_path[0]) { DeleteFileA(G.vxd_tmp_path);  G.vxd_tmp_path[0] = 0; }
}
/*  Send one VxD request.  */
int vxd_call(DWORD ioctl, const fdc_in * in_buf, fdc_out * out_buf) {
  DWORD cb = 0;
  if (!G.vxd || G.vxd == INVALID_HANDLE_VALUE) return 0;
  return DeviceIoControl(G.vxd, ioctl,
      (LPVOID) in_buf, in_buf ? sizeof(fdc_in) : 0,
      out_buf, out_buf ? sizeof(fdc_out) : 0, &cb, NULL) ? 1 : 0;
}

static int fdc_depth = 0;
static int fdc_drive = -1;

int vxd_begin(int drive) {
  fdc_in in_buf;  fdc_out o;
  if (!G.vxd) return 0;
  if (fdc_depth > 0) {
    if (drive != fdc_drive) return 0;
    fdc_depth++;
    return 1;
  }
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.flags = FDC_F_MOTOR | FDC_F_GATE;
  if (!vxd_call(IOCTL_FDC_MOTOR, &in_buf, &o)) return 0;
  Sleep(FDC_SPINUP_MS);
  fdc_depth = 1;
  fdc_drive = drive;
  return 1;
}
void vxd_end(void) {
  fdc_in in_buf;  fdc_out o;
  if (!G.vxd || fdc_depth <= 0) return;
  fdc_depth--;
  if (fdc_depth > 0) return;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) fdc_drive;
  vxd_call(IOCTL_FDC_END, &in_buf, &o);
  fdc_drive = -1;
}

static void fdc_gate_open(int drive) {
  fdc_in in_buf;  fdc_out o;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.flags = FDC_F_MOTOR | FDC_F_GATE;
  vxd_call(IOCTL_FDC_MOTOR, &in_buf, &o);
}

/*  Poll an asynchronous request to completion.  */
static int poll_until(DWORD ioctl, const fdc_in * in_buf, fdc_out * out,
                      DWORD limit_ms) {
  DWORD t0 = now_ms();
  for (;;) {
    if (!vxd_call(ioctl, in_buf, out)) return -1;
    if (out->status != FDC_ST_BUSY) return 0;
    DWORD el = now_ms() - t0;
    if (el > limit_ms) return -0xE7;
    if (el < 20) Sleep(0);
    else         Sleep(1);
  }
}
int vxd_reset(fdc_out * out) {
  fdc_out o;
  if (out) memzero(out, sizeof *out);
  if (!G.vxd) return -1;
  if (!vxd_call(IOCTL_FDC_RESET, NULL, &o)) return -1;
  if (out) *out = o;
  return 0;
}
int vxd_recalibrate(int drive, fdc_out * out) {
  fdc_in in_buf;  fdc_out o;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.vxd) return -1;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  if (!vxd_call(IOCTL_FDC_RECAL, &in_buf, &o)) { fdc_gate_open(drive); return -1; }
  if (o.status) {
    fdc_gate_open(drive);
    if (out) *out = o;
    return -(int) o.status;
  }
  in_buf.flags = FDC_F_SENSE;
  rc = poll_until(IOCTL_FDC_POLL, &in_buf, &o, FDC_SEEK_MS);
  if (rc != 0) { vxd_reset(NULL);  fdc_gate_open(drive); }
  if (out) *out = o;
  return rc;
}
int vxd_seek(int drive, int cyl, int head, fdc_out * out) {
  fdc_in in_buf;  fdc_out o;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.vxd) return -1;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.head  = (BYTE) head;
  in_buf.cyl   = (BYTE) cyl;
  if (!vxd_call(IOCTL_FDC_SEEK, &in_buf, &o)) { fdc_gate_open(drive); return -1; }
  if (o.status) {
    fdc_gate_open(drive);
    if (out) *out = o;
    return -(int) o.status;
  }
  in_buf.flags = FDC_F_SENSE;
  rc = poll_until(IOCTL_FDC_POLL, &in_buf, &o, FDC_SEEK_MS);
  if (rc != 0) { vxd_reset(NULL);  fdc_gate_open(drive); }
  if (out) *out = o;
  return rc;
}
int vxd_read_id(int drive, int head, fdc_out * out) {
  fdc_in in_buf;  fdc_out o;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.vxd) return -1;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.head  = (BYTE) head;
  if (!vxd_call(IOCTL_FDC_READID, &in_buf, &o)) { fdc_gate_open(drive); return -1; }
  if (o.status) {
    fdc_gate_open(drive);
    if (out) *out = o;
    return -(int) o.status;
  }
  /*  Allow two revolutions for a header.  */
  rc = poll_until(IOCTL_FDC_RESULT, &in_buf, &o, FDC_RESULT_MS);
  if (rc != 0) { vxd_reset(NULL);  fdc_gate_open(drive); }
  if (out) *out = o;
  if (rc == 0 && o.status) return -(int) o.status;
  return rc;
}
BYTE vxd_drive_type(int drive) {
  fdc_in in_buf;  fdc_out o;
  if (!G.vxd) return DEV_UNKNOWN;
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
int vxd_sense_media(int drive, fdc_out * out) {
  fdc_in in_buf;  fdc_out o;  BYTE before, after;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.vxd) return -2;
  if (!vxd_begin(drive)) return -2;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  vxd_call(IOCTL_FDC_DIR, &in_buf, &o);
  before = o.dir_before;
  /*  Guarantee a step pulse before resampling DIR.  */
  vxd_seek(drive, 1, 0, &o);
  vxd_seek(drive, 0, 0, &o);
  vxd_call(IOCTL_FDC_DIR, &in_buf, &o);
  after = o.dir_after;
  rc = vxd_read_id(drive, 0, &o);
  vxd_end();
  o.dir_before = before;
  o.dir_after  = after;
  if (out) *out = o;
  if (rc == 0 && o.result_n >= 7 && (o.st0 & 0xC0) == 0) return MEDIA_PRESENT;
  if ((before & FDC_DIR_DSKCHG) && (after & FDC_DIR_DSKCHG))
    return MEDIA_NONE;
  return MEDIA_UNREADABLE;
}
/*  Format one track through the raw FDC.  */
int vxd_format_track(int drive, const floppy_geom * g, int cyl, int head,
                    BYTE dev_type, fdc_out * out) {
  fdc_in in_buf;  fdc_out o;  int ok;
  if (out) memzero(out, sizeof *out);
  if (!G.vxd) return -1;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive     = (BYTE) drive;
  in_buf.head      = (BYTE) head;
  in_buf.cyl       = (BYTE) cyl;
  in_buf.sec       = (BYTE) g->spt;
  in_buf.size_code = 2;
  in_buf.gap3      = g->gap3_fmt;
  in_buf.filler    = 0xF6;
  in_buf.rate      = geom_rate_in_drive(g, dev_type);
  in_buf.spec1     = FDC_SPECIFY_1;
  in_buf.spec2     = FDC_SPECIFY_2;
  if (!vxd_begin(drive)) return -1;
  /*  Set the data rate before timing the seek.  */
  vxd_call(IOCTL_FDC_SPECIFY, &in_buf, &o);
  vxd_seek(drive, cyl, head, &o);
  ok = vxd_call(IOCTL_FDC_FORMAT, &in_buf, &o);
  vxd_end();
  if (!ok) return -1;
  if (out) *out = o;
  if (o.status) return -(int) o.status;
  if (o.result_n < 7) return -0xE7;
  if (o.st0 & 0xC0) return -(int) (o.st0 ? o.st0 : 0xE8);
  if (o.st1) return -0xEA;
  return 0;
}
/*  Check a track ID at the requested cylinder.  */
int vxd_verify_track(int drive, int cyl, int head, fdc_out * out) {
  fdc_out o;  int rc;
  if (out) memzero(out, sizeof *out);
  if (!G.vxd) return -1;
  if (!vxd_begin(drive)) return -1;
  vxd_seek(drive, cyl, head, &o);
  rc = vxd_read_id(drive, head, &o);
  vxd_end();
  if (out) *out = o;
  if (rc != 0)           return -1;
  if (o.result_n < 7)    return -0xE7;
  if (o.st0 & 0xC0)      return -(int) o.st0;
  if (o.c != (BYTE) cyl) return -0xE9;
  return 0;
}
/*  Decode an FDC result.  */
const char * st_summary(BYTE st0, BYTE st1, BYTE st2) {
  if (st2 & 0x10)            return "wrong cylinder (head misaligned)";
  if (st2 & 0x20)            return "CRC error in data field";
  if (st1 & 0x20)            return "CRC error in ID field";
  if (st1 & 0x01)            return "missing ID address mark";
  if (st2 & 0x01)            return "missing data address mark";
  if (st1 & 0x04)            return "no data (track unformatted?)";
  if (st1 & 0x02)            return "not writable";
  if (st1 & 0x10)            return "DMA overrun";
  if (st1 & 0x80)            return "end of cylinder";
  if (st0 & 0x10)            return "equipment check / drive fault";
  if (st0 & 0x08)            return "not ready";
  if ((st0 & 0xC0) == 0x40)  return "abnormal termination";
  if ((st0 & 0xC0) == 0x80)  return "invalid command";
  if ((st0 & 0xC0) == 0xC0)  return "polling-state change";
  return "ok";
}
