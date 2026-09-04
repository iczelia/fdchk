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

/*  Shared state and interfaces.  */
#ifndef FDCHK_H
#define FDCHK_H

#define _WIN32_WINNT 0x0400
#define WINVER       0x0400
#include <windows.h>
#include <commctrl.h>

/*  Use the ANSI value for IDC_ARROW.  */
#undef  IDC_ARROW
#define IDC_ARROW MAKEINTRESOURCEA(32512)
#define memzero(p, n) memset((p), 0, (n))

/*  Loop macros require caller declare induction variable.  */
#define Fi(n, ...) for (i = 0; i < (n); i++) {  __VA_ARGS__; }
#define Fj(n, ...) for (j = 0; j < (n); j++) {  __VA_ARGS__; }
#define Fk(n, ...) for (k = 0; k < (n); k++) {  __VA_ARGS__; }
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/*  Freestanding string and formatting helpers.  */
#define lstrlenA  x_strlen
#define lstrcpyA  x_strcpy
#define lstrcpynA x_strcpyn
#define wsprintfA x_sprintf
int    x_strlen(const char * s);
char * x_strcpy (char * d, const char * s);
char * x_strcpyn(char * d, const char * s, int n);
int    x_sprintf(char * out, const char * fmt, ...);

#define APP_NAME     "fdchk"
#define APP_CLASS    "FdchkMainWnd"
#define APP_VERSION  "1.0.3"
#define WND_W        560
#define WND_H        498
#define MAX_SECTORS  5760     /*  2.88 MB  */
#define SECTOR_SIZE  512
#define MAX_BAD      512
#define MAX_CYLS     80
#define MAX_HEADS    2
#define MAX_SPT      36

/*  One byte per sector or diagnostic cell.  */
enum {
  ST_UNTESTED  = 0,
  ST_SYSTEM    = 1,   /*  boot, FAT, or root directory  */
  ST_GOOD      = 2,
  ST_SCANNING  = 3,
  ST_WRITING   = 4,
  ST_VERIFY    = 5,
  ST_BAD_NEW   = 6,
  ST_BAD_OLD   = 7,   /*  marked bad in the FAT  */
  ST_WRONG_CYL = 8,   /*  sector ID names another cylinder  */
  ST_NO_AM     = 9,   /*  no address mark  */
  ST_DATA_0    = 16, ST_DATA_1 = 17, ST_DATA_2 = 18, ST_DATA_3 = 19,
  /*  Bright-to-dim scan trail.  */
  ST_TRAIL_0   = 24, ST_TRAIL_1 = 25, ST_TRAIL_2 = 26, ST_TRAIL_3 = 27
};

/*  Test modes.  */
enum {
  MODE_STANDARD   = 0,  /*  read every sector  */
  MODE_THOROUGH   = 1,  /*  write and check four patterns  */
  MODE_DIAGNOSTIC = 2,  /*  test FDC seeks and alignment  */
  MODE_CHKFS      = 3,
  MODE_DEFRAG     = 4,
  MODE_FORMAT     = 5
};

/*  Full format formats every track before writing FAT12.  */
enum {
  FMT_QUICK = 0,
  FMT_FULL  = 1
};

/*  Standard media geometry and its DOS driver parameters.  */
typedef struct {
  const char * name;
  int  total_sec;
  int  cyls, heads, spt;
  int  sec_per_cluster, reserved_sec, num_fats, fat_size, root_entries;
  BYTE media_byte;           /*  BPB descriptor  */
  BYTE dos_dev_type;
  BYTE inch5;
  BYTE gap3_fmt;             /*  FDC format gap  */
  BYTE rate;                 /*  CCR: 500k, 300k, 250k, or 1M  */
} floppy_geom;

enum {
  FMT_BY_DRIVER = 0,   /*  Int 21h 440Dh CX=0842h  */
  FMT_BY_FDC    = 1    /*  fdchk.vxd, FDC command 4Dh  */
};

/*  SPECIFY timings: SRT=13, HUT=15 / HLT=1.  ND is set by the VxD.  */
#define FDC_SPECIFY_1 0xDF
#define FDC_SPECIFY_2 0x02

/*  DOS device types for Int 21h 440Dh.  */
#define DEV_360K   0    /*  320K/360K 5.25"  */
#define DEV_1200K  1    /*  1.2 MB 5.25"  */
#define DEV_720K   2    /*  720 KB 3.5"  */
#define DEV_1440K  7    /*  1.44 MB 3.5"  */
#define DEV_2880K  9    /*  2.88 MB 3.5"  */
#define DEV_UNKNOWN 0xFF

/*  CMOS 10h nibble values.  */
#define CMOS_FD_360K   1
#define CMOS_FD_1200K  2
#define CMOS_FD_720K   3
#define CMOS_FD_1440K  4
#define CMOS_FD_2880K  5

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

/*  Worker-to-UI messages.  */
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)
#define WM_APP_REPAINT  (WM_APP + 3)

/*  VWIN32 raw sector I/O.  */
typedef struct {
  DWORD ebx, edx, ecx, eax, edi, esi, flags;
} dioc_regs;

#pragma pack(push, 1)
typedef struct {
  DWORD start_sector;
  WORD  sectors;
  DWORD buffer;
} disk_io;
#pragma pack(pop)

#define VWIN32_DIOC_DOS_IOCTL  1
#define VWIN32_DIOC_DOS_INT25  2
#define VWIN32_DIOC_DOS_INT26  3

/*  CX values for Int 21h AX=440Dh.  */
#define DOS_IOCTL_SET_DEV_PARAMS 0x0840
#define DOS_IOCTL_FORMAT_TRACK   0x0842
#define DOS_IOCTL_GET_DEV_PARAMS 0x0860
#define DOS_IOCTL_VERIFY_TRACK   0x0862
#define DOS_IOCTL_LOCK_VOLUME    0x084A
#define DOS_IOCTL_UNLOCK_VOLUME  0x086A

#pragma pack(push, 1)

/*  Device parameter block for CX=0840h / 0860h.  */
typedef struct {
  BYTE  spec_func;      /*  00h  bit 0: default BPB, bit 2: uniform sectors  */
  BYTE  dev_type;       /*  01h  DEV_* above  */
  WORD  dev_attr;       /*  02h  bit 0: medium is not removable  */
  WORD  cyls;           /*  04h  */
  BYTE  media_type;     /*  06h  1 = 360K media in a 1.2 MB drive  */
  /*  31-byte device BPB.  */
  WORD  bytes_per_sec;  /*  07h  */
  BYTE  sec_per_clus;   /*  09h  */
  WORD  reserved_sec;   /*  0Ah  */
  BYTE  num_fats;       /*  0Ch  */
  WORD  root_entries;   /*  0Dh  */
  WORD  total_sec;      /*  0Fh  */
  BYTE  media_byte;     /*  11h  */
  WORD  fat_size;       /*  12h  */
  WORD  spt;            /*  14h  */
  WORD  heads;          /*  16h  */
  DWORD hidden_sec;     /*  18h  */
  DWORD total_sec32;    /*  1Ch  */
  BYTE  bpb_pad[6];     /*  20h  */
  WORD  track_spt;      /*  26h  sectors in the layout below  */
  struct {
    WORD sec;           /*  1-based sector number  */
    WORD size;          /*  sector size in bytes  */
  } track[MAX_SPT];     /*  28h  */
} dos_dev_params;

/*  Track parameter block for CX=0842h and 0862h.  */
typedef struct {
  BYTE spec_func;
  WORD head;
  WORD cyl;
} dos_track_params;

#pragma pack(pop)

/*  DOS errors returned in AL with carry set.  */
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
  HANDLE vwin32;
  int    drive;        /*  0 = A:, 1 = B:  */
  int    locked;       /*  nested lock levels held  */
} disk_handle;

/*  Disk check results.  */
enum {
  MEDIA_PRESENT    =  1,
  MEDIA_NONE       =  0,
  MEDIA_UNREADABLE = -1
};

/*  Asynchronous FDC interface.  */

#define IOCTL_FDC_RESET    0x0080
#define IOCTL_FDC_RECAL    0x0082
#define IOCTL_FDC_SEEK     0x0083
#define IOCTL_FDC_READID   0x0084
#define IOCTL_FDC_POLL     0x0085
#define IOCTL_FDC_DIR      0x0086
#define IOCTL_FDC_FORMAT   0x0087
#define IOCTL_FDC_CMOS     0x0088
#define IOCTL_FDC_RESULT   0x0089
#define IOCTL_FDC_MOTOR    0x008A
#define IOCTL_FDC_SPECIFY  0x008B
#define IOCTL_FDC_END      0x008C
#define IOCTL_FDC_IDENT    0x008E

/*  Non-error fdc_out.status values.  */
#define FDC_ST_OK    0x00
#define FDC_ST_BUSY  0x01

/*  IOCTL_FDC_IDENT interface version.  */
#define FDC_VXD_VERSION 0x02

/*  fdc_in.flags.  */
#define FDC_F_MOTOR  0x01
#define FDC_F_GATE   0x02
#define FDC_F_SENSE  0x01

/*  Motor, seek, and result timeouts.  */
#define FDC_SPINUP_MS  500
#define FDC_SEEK_MS    1000
#define FDC_RESULT_MS  1000

#pragma pack(push, 1)
typedef struct {
  BYTE drive, head, cyl;
  BYTE sec;         /*  sectors per track  */
  BYTE size_code;   /*  N; 2 means 512-byte sectors  */
  BYTE gap3, filler, rate;
  BYTE spec1, spec2;
  BYTE flags;
} fdc_in;

typedef struct {
  BYTE status, st0, st1, st2, c, h, r, n, cur_cyl, result_n;
  BYTE dir_before, dir_after;
  BYTE stage, msr;
  BYTE cmos;                    /*  A: high nibble; B: low nibble  */
  BYTE pad;
} fdc_out;

_Static_assert(sizeof(fdc_in) == 11, "fdc_in/VxD layout mismatch");
_Static_assert(sizeof(fdc_out) == 16, "fdc_out/VxD layout mismatch");

#define FDC_DIR_DSKCHG 0x80

/*  FAT12 directory entry (32 bytes).  */
typedef struct {
  char  name[11];
  BYTE  attr, ntres, crt_t10;
  WORD  crt_t, crt_d, acc_d, start_hi, wrt_t, wrt_d, start_lo;
  DWORD size;
} fat_dirent;
#pragma pack(pop)

/*  Global application state.  */
typedef struct {
  HINSTANCE instance;
  HWND      main, grid, statusbar, progress;
  HWND      standard, thorough, diagnostic, chkfs;
  HWND      auto_fix_button, batch_button, drive_a, drive_b;
  HWND      start_button, stop_button, recover_button, format_button;
  HWND      defrag_button, logs_button, about_button, close_button;
  HWND      drive_group, test_group;
  char      log_dir[MAX_PATH];
  char      vxd_tmp_path[MAX_PATH];
  RECT      plinth;

  /*  GDI objects.  */
  HFONT   font;
  HBRUSH  face_brush, black_brush, shadow_brush, untested_brush, system_brush;
  HBRUSH  bad_new_brush, bad_old_brush, write_brush, verify_brush;
  HBRUSH  wrong_cyl_brush, no_am_brush;
  HBRUSH  data_brush[4];
  HBRUSH  trail_brush[4];
  HBITMAP scan_bitmap;
  HBRUSH  scan_brush;
  HPEN    shadow_pen, highlight_pen;

  /*  Worker thread.  */
  HANDLE       thread;
  DWORD        tid;
  volatile LONG abort_req, running;
  volatile LONG closing;
  HANDLE       vxd;

  /*  User choices.  */
  int drive, mode, auto_fix, batch;

  /*  Geometry read from the BPB.  */
  int   bytes_per_sec, sec_per_cluster, reserved_sec, num_fats, fat_size;
  int   root_entries, total_sec, data_start_sec, total_clusters;
  int   cyls, heads, spt;
  BYTE  media_byte;
  DWORD vol_id;
  char  vol_label[12];
  char  fs_type[9];
  int   has_fat;
  int   has_a, has_b;
  BYTE  drive_type[2];

  /*  Defragmentation result.  */
  int defrag_rc, defrag_moved, defrag_zeroed;

  /*  Format job and result.  */
  const floppy_geom * fmt_geom;
  int  fmt_style;
  int  fmt_method;
  int  fmt_rc;             /*  0 done, 1 stopped, -1 failed  */
  int  fmt_low_level_ok;
  int  fmt_bad_tracks;
  int  fmt_sys_bad;
  char fmt_label[12];
  char fmt_msg[256];

  /*  Sector state map, one byte per sector.  */
  BYTE * state;

  /*  Progress.  */
  int   current_sec;
  DWORD scanned, good_count, bad_count, t_start;
  char  status[128];

  /*  Bad sectors.  */
  DWORD * bad_lba;
  int     bad_n;

  int scan_anim_phase;

  /*  Four-cell scan trail.  */
  int trail_sec[4];
  int trail_age[4];
} app;

extern app G;

/*  Format a line and append it to an open log file.  */
#define LOG_FMT(handle, ...)         \
  do {                               \
    char buf_[512];                  \
    wsprintfA(buf_, __VA_ARGS__);    \
    log_write((handle), buf_);       \
  } while (0)

/*  Runtime and logging.  */
DWORD now_ms(void);
void  init_log_dir(void);
void  log_path(char * out, const char * name);
void  fmt_hms(DWORD ms, char * out);
void  log_write(HANDLE h, const char * s);
HANDLE log_create(const char * name);
void  rng_seed(DWORD s);
DWORD rng_next(void);

/*  Disk and media.  */
int   disk_open(disk_handle * d, int drive);
void  disk_close(disk_handle * d);
int   disk_lock(disk_handle * d, int level);
int   disk_lock_tiered(disk_handle * d);
int   disk_unlock(disk_handle * d);
int   disk_read(disk_handle * d, DWORD lba, WORD count, void * buf);
int   disk_write(disk_handle * d, DWORD lba, WORD count, const void * buf);
int   probe_disk_present(disk_handle * d);
const char * dos_err_str(int e);
const char * dos_ioctl_err_str(int e);

int   geom_count(void);
const floppy_geom * geom_at(int i);
const floppy_geom * geom_for_size(int total_sec);
const floppy_geom * geom_for_bpb(int total_sec, int spt, int heads);
const floppy_geom * geom_for_drive_type(BYTE dev_type);
const char * drive_type_str(BYTE dev_type);
int   geom_fits_drive(const floppy_geom * g, BYTE dev_type);
void  geom_apply(const floppy_geom * g);
BYTE  geom_rate_in_drive(const floppy_geom * g, BYTE dev_type);

int   disk_get_dev_params(disk_handle * d, dos_dev_params * p, int want_default);
int   disk_set_dev_params(disk_handle * d, const floppy_geom * g, BYTE dev_type);
int   disk_format_track(disk_handle * d, int cyl, int head);
int   disk_verify_track(disk_handle * d, int cyl, int head);
BYTE  disk_probe_drive_type(disk_handle * d);
int   disk_resync_media(disk_handle * d);
int   format_disk(disk_handle * d, const floppy_geom * g,
                  const char * label, int style);
DWORD WINAPI format_thread(LPVOID arg);

/*  Raw FDC driver.  */
int   vxd_open(void);
void  vxd_close(void);
int   vxd_call(DWORD ioctl, const fdc_in * in_buf, fdc_out * out_buf);
int   vxd_begin(int drive);
void  vxd_end(void);
int   vxd_reset(fdc_out * out);
int   vxd_recalibrate(int drive, fdc_out * out);
int   vxd_seek(int drive, int cyl, int head, fdc_out * out);
int   vxd_read_id(int drive, int head, fdc_out * out);
BYTE  vxd_drive_type(int drive);
int   vxd_sense_media(int drive, fdc_out * out);
int   vxd_format_track(int drive, const floppy_geom * g, int cyl, int head,
                       BYTE dev_type, fdc_out * out);
int   vxd_verify_track(int drive, int cyl, int head, fdc_out * out);
const char * st_summary(BYTE st0, BYTE st1, BYTE st2);

/*  FAT12.  */
int   parse_bpb(const BYTE * s);
int   lba_to_cluster(DWORD lba);
DWORD cluster_to_lba(int cl);
void  name83_to_str(const fat_dirent * e, char * out);
WORD  fat12_get(const BYTE * fat, int n);
void  fat12_set(BYTE * fat, int n, WORD v);
int   check_filesystem(disk_handle * dh);
int   recover_files(HWND parent, const char * out_dir);

/*  Workers.  */
void  mark_system_sectors(void);
DWORD WINAPI scan_thread(LPVOID arg);
DWORD WINAPI defrag_thread(LPVOID arg);

/*  UI.  */
void  ui_set_state(int sec, BYTE st);
void  ui_status(const char * s);
int   ui_prompt(const char * msg, const char * caption, UINT flags);
void  trail_clear(void);
void  trail_tick(void);
void  grid_calc(HWND h);
void  grid_invalidate_cell(int idx);
void  update_scan_brush(void);
void  ui_create(HWND window);
void  ui_layout(HWND window);
void  ui_paint_legend(HDC hdc);
void  ui_start_scan(HWND window);
void  ui_stop_scan(HWND window);
void  ui_worker_done(HWND window);
void  ui_about(HWND window);
void  ui_logs(HWND parent);
void  ui_format(HWND window);
void  ui_progress(DWORD done, DWORD total);
void  ui_drive_labels(void);
void  ui_defrag(HWND window);
void  ui_recover(HWND window);
LRESULT CALLBACK grid_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);

#endif
