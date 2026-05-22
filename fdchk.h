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

/*  Shared declarations: constants, structures, global state, prototypes.  */

#ifndef FDCHK_H
#define FDCHK_H

#define _WIN32_WINNT 0x0400
#define WINVER       0x0400
#include <windows.h>
#include <commctrl.h>

/*  In ANSI builds mingw-w64 sometimes resolves IDC_ARROW to the W form;
    pin it to the ANSI ordinal.  */
#undef  IDC_ARROW
#define IDC_ARROW MAKEINTRESOURCEA(32512)

#define memzero(p, n) memset((p), 0, (n))

/*  String and printf helpers (fdchk.c) stand in for the kernel32/user32
    exports of the same role, so those need not be imported.  */
#define lstrlenA  x_strlen
#define lstrcpyA  x_strcpy
#define lstrcpynA x_strcpyn
#define wsprintfA x_sprintf
int    x_strlen (const char * s);
char * x_strcpy (char * d, const char * s);
char * x_strcpyn(char * d, const char * s, int n);
int    x_sprintf(char * out, const char * fmt, ...);

/*  Constants.  */

#define APP_NAME     "Floppy Disk Checker"
#define APP_CLASS    "FdchkMainWnd"
#define APP_VERSION  "1.0"
#define WND_W        560
#define WND_H        498
#define MAX_SECTORS  2880     /*  1.44 MB floppy  */
#define SECTOR_SIZE  512
#define MAX_BAD      512

/*  Sector / diagnostic-cell states (one byte per cell in G.state).  */
enum {
  ST_UNTESTED  = 0,
  ST_SYSTEM    = 1,   /*  reserved / FAT / root directory  */
  ST_GOOD      = 2,
  ST_SCANNING  = 3,
  ST_WRITING   = 4,
  ST_VERIFY    = 5,
  ST_BAD_NEW   = 6,
  ST_BAD_OLD   = 7,   /*  already marked bad in the FAT  */
  ST_WRONG_CYL = 8,   /*  diag: sector ID read from the wrong cylinder  */
  ST_NO_AM     = 9,   /*  diag: no address mark (unformatted / severe)  */
  /*  chkfs: a cluster owned by file N, shaded by (N & 3) so adjacent
      files visibly contrast.  */
  ST_DATA_0    = 16, ST_DATA_1 = 17, ST_DATA_2 = 18, ST_DATA_3 = 19,
  /*  fading trail behind the scanner, brightest (0) to dimmest (3).  */
  ST_TRAIL_0   = 24, ST_TRAIL_1 = 25, ST_TRAIL_2 = 26, ST_TRAIL_3 = 27
};

/*  Test modes.  */
enum {
  MODE_STANDARD   = 0,  /*  read-only surface verify  */
  MODE_THOROUGH   = 1,  /*  full write-pattern surface test  */
  MODE_DIAGNOSTIC = 2,  /*  BIOS / FDC head + seek + alignment probe  */
  MODE_CHKFS      = 3,  /*  FAT12 consistency check  */
  MODE_DEFRAG     = 4   /*  defragment the disk  */
};

/*  Control IDs.  */
#define ID_DRIVE_A    1001
#define ID_DRIVE_B    1002
#define ID_STANDARD   1010
#define ID_THOROUGH   1011
#define ID_DIAGNOSTIC 1012
#define ID_CHKFS      1013
#define ID_AUTOFIX    1020
#define ID_BATCH      1021
#define ID_START      1030
#define ID_STOP       1031
#define ID_RECOVER    1032
#define ID_FORMAT     1033
#define ID_ABOUT      1034
#define ID_CLOSE      1035
#define ID_LOGS       1037
#define ID_ABOUT_OK   1038
#define ID_DEFRAG     1039
#define ID_GRID       1100
#define ID_PROGRESS   1101
#define ID_STATUSBAR  1102

/*  Worker-thread -> UI messages.  */
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)
#define WM_APP_REPAINT  (WM_APP + 3)

/*  VWIN32 raw sector I/O.  */

typedef struct {
  DWORD reg_EBX, reg_EDX, reg_ECX, reg_EAX, reg_EDI, reg_ESI, reg_Flags;
} DIOC_REGISTERS;

#pragma pack(push, 1)
typedef struct {
  DWORD dwStartSector;
  WORD  wSectors;
  DWORD dwBuffer;
} DISKIO;
#pragma pack(pop)

#define VWIN32_DIOC_DOS_IOCTL  1
#define VWIN32_DIOC_DOS_INT25  2
#define VWIN32_DIOC_DOS_INT26  3
#define VWIN32_DIOC_DOS_INT13  4

/*  DOS error codes returned in AL after a carry-set call.  */
#define DOS_ERR_BAD_CMD        0x01
#define DOS_ERR_ADDR_MARK      0x02
#define DOS_ERR_WRITE_PROTECT  0x03
#define DOS_ERR_SECTOR_NOT_FND 0x04
#define DOS_ERR_CHANGE_LINE    0x06
#define DOS_ERR_DMA_OVERRUN    0x08
#define DOS_ERR_INVALID_MEDIA  0x0C
#define DOS_ERR_DATA           0x10
#define DOS_ERR_CTRL_FAIL      0x20
#define DOS_ERR_SEEK_FAIL      0x40
#define DOS_ERR_TIMEOUT        0x80

/*  High-level disk handle.  */
typedef struct {
  HANDLE hVwin32;
  int    drive;        /*  0 = A:, 1 = B:  */
  int    locked;
} DiskHandle;

/*   FDC VxD interface.  */

#define IOCTL_FDC_RESET    0x0080
#define IOCTL_FDC_SENSEINT 0x0081
#define IOCTL_FDC_RECAL    0x0082
#define IOCTL_FDC_SEEK     0x0083
#define IOCTL_FDC_READID   0x0084
#define IOCTL_FDC_SPECIFY  0x0085
#define IOCTL_FDC_RAW      0x008F

#pragma pack(push, 1)
typedef struct {
  BYTE drive, head, cyl, sec, size_code, reserved, motor, raw_count;
  BYTE raw[15];
} FdcIn;

typedef struct {
  BYTE status, st0, st1, st2, c, h, r, n, cur_cyl, result_n;
  BYTE reserved[6];
} FdcOut;

/*  FAT12 directory entry (32 bytes).  */
typedef struct {
  char  name[11];
  BYTE  attr, ntres, crt_t10;
  WORD  crt_t, crt_d, acc_d, start_hi, wrt_t, wrt_d, start_lo;
  DWORD size;
} FatDirEntry;
#pragma pack(pop)

/*  Global application state.  */

typedef struct {
  HINSTANCE hInst;
  HWND      hMain, hGrid, hStatus, hProgress;
  HWND      hStandard, hThorough, hDiagnostic, hChkfs;
  HWND      hAutoFix, hBatch, hDriveA, hDriveB;
  HWND      hStart, hStop, hRecover, hFormat, hDefrag, hLogs, hAbout, hClose;
  HWND      hGrpDrive, hGrpType;
  char      log_dir[MAX_PATH];        /*  <exe-dir>\logs  */
  char      vxd_tmp_path[MAX_PATH];   /*  where the embedded VxD got dropped  */
  RECT      rcPlinth;                 /*  status plinth, repainted on a timer  */

  /*  GDI objects.  */
  HFONT   hFont;
  HBRUSH  hbrFace, hbrBlack, hbrShadow, hbrUntested, hbrSystem;
  HBRUSH  hbrBadNew, hbrBadOld, hbrWrite, hbrVerify, hbrWrongCyl, hbrNoAM;
  HBRUSH  hbrData[4];      /*  fragmentation: 4 alternating hues  */
  HBRUSH  hbrTrail[4];     /*  scan trail: 4 fading hues  */
  HBITMAP hbmScan;         /*  8x8 hatch used as the scanning brush  */
  HBRUSH  hbrScan;
  HPEN    penShadow, penHilite;

  /*  Worker thread.  */
  HANDLE       hThread;
  DWORD        tid;
  volatile LONG abort_req, running;
  volatile LONG closing;   /*  close requested; destroy after worker exits  */
  HANDLE       hVxd;       /*  raw FDC VxD, NULL if unavailable  */

  /*  User choices.  */
  int drive, mode, autofix, batch;

  /*  Geometry detected from the BPB.  */
  int   bytes_per_sec, sec_per_cluster, reserved_sec, num_fats, fat_size;
  int   root_entries, total_sec, data_start_sec, total_clusters;
  BYTE  media_byte;
  DWORD vol_id;
  char  vol_label[12];
  char  fs_type[9];
  int   has_fat;           /*  1 if the BPB parsed OK  */
  int   has_a, has_b;      /*  floppy drives detected at startup  */

  /*  Defrag results, passed back to on_done.  */
  int defrag_rc, defrag_moved, defrag_zeroed;

  /*  Sector state map, one byte per sector.  */
  BYTE * state;

  /*  Progress.  */
  int   current_sec;
  DWORD scanned, good_count, bad_count, t_start;
  char  status[128];

  /*  Bad-sector log, heap-allocated.  */
  DWORD * bad_lba;
  int     bad_n;

  int scan_anim_phase;     /*  0..3 for the animated hatch  */

  /*  Scan trail: a 4-cell fading tail behind the scanner.  */
  int trail_sec[4];        /*  sector indices, -1 = empty  */
  int trail_age[4];        /*  0..3, -1 = empty slot  */
} App;

extern App G;

/*  Format a line and append it to an open log file.  */
#define LOG_FMT(handle, ...)         \
  do {                               \
    char buf_[512];                  \
    wsprintfA(buf_, __VA_ARGS__);    \
    log_write((handle), buf_);       \
  } while (0)

/*  Cross-unit prototypes.  */

/*  fdchk.c  */
DWORD now_ms(void);
void  init_log_dir(void);
void  log_path(char * out, const char * name);
void  fmt_hms(DWORD ms, char * out);
void  log_write(HANDLE h, const char * s);
void  rng_seed(DWORD s);
DWORD rng_next(void);

/*  disk.c  */
int   disk_open(DiskHandle * d, int drive);
void  disk_close(DiskHandle * d);
int   disk_lock(DiskHandle * d, int level);
int   disk_unlock(DiskHandle * d);
int   disk_read(DiskHandle * d, DWORD lba, WORD count, void * buf);
int   disk_write(DiskHandle * d, DWORD lba, WORD count, const void * buf);
int   probe_disk_present(DiskHandle * d);
int   bios_int13(DiskHandle * d, BYTE ah_in, BYTE al_in,
                 int cyl, int head, int sec, void * buf, BYTE * ah_out);
void  bios_ah_to_st(BYTE ah, BYTE * st0, BYTE * st1, BYTE * st2);
const char * bios_ah_str(BYTE ah);
const char * dos_err_str(int e);
int   format_disk(DiskHandle * d, const char * label, int full_format);

/*  vxd.c  */
int   vxd_open(void);
void  vxd_close(void);
int   vxd_call(DWORD ioctl, const FdcIn * in_buf, FdcOut * out_buf);
const char * st_summary(BYTE st0, BYTE st1, BYTE st2);

/*  fat.c  */
int   parse_bpb(const BYTE * s);
int   lba_to_cluster(DWORD lba);
DWORD cluster_to_lba(int cl);
void  name83_to_str(const FatDirEntry * e, char * out);
WORD  fat12_get(const BYTE * fat, int n);
void  fat12_set(BYTE * fat, int n, WORD v);
int   chkfs_worker(DiskHandle * dh);
int   do_recovery(HWND parent, const char * out_dir);

/*  scan.c  */
void  mark_system_sectors(void);
DWORD WINAPI worker_proc(LPVOID arg);

/*  defrag.c  */
DWORD WINAPI defrag_thread_proc(LPVOID arg);

/*  ui.c  */
void  ui_set_state(int sec, BYTE st);
void  ui_status(const char * s);
int   ui_prompt(const char * msg, const char * caption, UINT flags);
void  trail_clear(void);
void  trail_tick(void);
void  grid_calc(HWND h);
void  grid_invalidate_cell(int idx);
void  update_scan_brush(void);
void  create_controls(HWND hWnd);
void  layout_apply(HWND hWnd);
void  paint_legend(HDC hdc);
void  start_scan(HWND hWnd);
void  stop_scan(HWND hWnd);
void  on_done(HWND hWnd);
void  do_about(HWND hWnd);
void  do_logs(HWND parent);
void  do_format_flow(HWND hWnd);
void  do_defrag_flow(HWND hWnd);
void  do_recover_flow(HWND hWnd);
LRESULT CALLBACK grid_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);

#endif
