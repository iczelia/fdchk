/*  fdchk -- Copyright (C) 2026 Kamila Szewczyk

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

#include "fdchk.h"
#include <stdarg.h>

App G;

void * memset(void * d, int c, size_t n) {
  void * r = d;
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
  return r;
}
void * memcpy(void * d, const void * s, size_t n) {
  void * r = d;
  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  return r;
}
int memcmp(const void * a, const void * b, size_t n) {
  int r = 0;
  if (n)
    __asm__ volatile("repz cmpsb\n\t"
                     "movzbl -1(%1), %0\n\t"
                     "movzbl -1(%2), %%edx\n\t"
                     "subl %%edx, %0"
                     : "+a"(r), "+S"(a), "+D"(b), "+c"(n)
                     :
                     : "edx", "cc", "memory");
  return r;
}

int x_strlen(const char * s) {
  const char * p = s;
  while (*p) ++p;
  return (int) (p - s);
}
char * x_strcpy(char * d, const char * s) {
  char * r = d;
  while ((*d++ = *s++)) ;
  return r;
}
/*  lstrcpynA: copy at most n-1 chars, always NUL-terminate.  */
char * x_strcpyn(char * d, const char * s, int n) {
  char * r = d;
  if (n > 0) {
    while (--n && (*d = *s)) { ++d; ++s; }
    *d = 0;
  }
  return r;
}

/*  Minimal printf: '-' and '0' flags, a decimal width, an ignored length
    modifier, and %c %d %u %x %X %s %%.  */
int x_sprintf(char * out, const char * fmt, ...) {
  char * o = out;
  va_list ap;
  va_start(ap, fmt);
  for (; *fmt; ++fmt) {
    if (*fmt != '%') { *o++ = *fmt; continue; }
    int left = 0, zero = 0, width = 0;
    for (++fmt; *fmt == '-' || *fmt == '0'; ++fmt) {
      if (*fmt == '-') left = 1;
      else             zero = 1;
    }
    for (; *fmt >= '0' && *fmt <= '9'; ++fmt)
      width = width * 10 + (*fmt - '0');
    while (*fmt == 'l' || *fmt == 'h') ++fmt;

    char tmp[33], cbuf[2];
    const char * s = tmp;
    switch (*fmt) {
      case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; break;
      case 'c': cbuf[0] = (char) va_arg(ap, int); cbuf[1] = 0; s = cbuf; break;
      case '%': cbuf[0] = '%'; cbuf[1] = 0; s = cbuf; break;
      case 'd': case 'i': {
        long v = va_arg(ap, int);
        unsigned long u = v < 0 ? 0u - (unsigned long) v : (unsigned long) v;
        char * p = tmp + 32;
        *p = 0;
        do { *--p = (char) ('0' + u % 10); u /= 10; } while (u);
        if (v < 0) *--p = '-';
        s = p;
        break;
      }
      case 'u': {
        unsigned long u = va_arg(ap, unsigned int);
        char * p = tmp + 32;
        *p = 0;
        do { *--p = (char) ('0' + u % 10); u /= 10; } while (u);
        s = p;
        break;
      }
      case 'x': case 'X': {
        unsigned long u = va_arg(ap, unsigned int);
        const char * dig = *fmt == 'X' ? "0123456789ABCDEF"
                                       : "0123456789abcdef";
        char * p = tmp + 32;
        *p = 0;
        do { *--p = dig[u & 15]; u >>= 4; } while (u);
        s = p;
        break;
      }
      case 0:  --fmt; continue;
      default: cbuf[0] = *fmt; cbuf[1] = 0; s = cbuf; break;
    }

    int len = x_strlen(s), pad = width - len;
    if (left) {
      while (len--) *o++ = *s++;
      while (pad-- > 0) *o++ = ' ';
    } else if (zero) {
      if (len && (*s == '-' || *s == '+')) { *o++ = *s++; len--; }
      while (pad-- > 0) *o++ = '0';
      while (len--) *o++ = *s++;
    } else {
      while (pad-- > 0) *o++ = ' ';
      while (len--) *o++ = *s++;
    }
  }
  *o = 0;
  va_end(ap);
  return (int) (o - out);
}

DWORD now_ms(void) { return GetTickCount(); }

/*  <exe-dir>\logs\, created once at startup.  */
void init_log_dir(void) {
  char exe[MAX_PATH];
  GetModuleFileNameA(NULL, exe, MAX_PATH);
  char * last = exe;
  for (char * p = exe; *p; ++p)
    if (*p == '\\' || *p == '/') last = p;
  *last = 0;
  wsprintfA(G.log_dir, "%s\\logs", exe);
  CreateDirectoryA(G.log_dir, NULL);
}

void log_path(char * out, const char * name) {
  wsprintfA(out, "%s\\%s", G.log_dir, name);
}

void fmt_hms(DWORD ms, char * out) {
  DWORD s = ms / 1000;
  wsprintfA(out, "%02lu:%02lu:%02lu", (unsigned long) (s / 3600),
            (unsigned long) ((s / 60) % 60), (unsigned long) (s % 60));
}

HANDLE log_create(const char * name) {
  char full[MAX_PATH];
  log_path(full, name);
  return CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                     CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
}

void log_write(HANDLE h, const char * s) {
  DWORD cb;
  if (h != INVALID_HANDLE_VALUE) WriteFile(h, s, lstrlenA(s), &cb, NULL);
}

/*  xorshift32 PRNG.  */
static DWORD g_rng = 0x13579BDFu;
DWORD rng_next(void) {
  DWORD x = g_rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (g_rng = x);
}
void rng_seed(DWORD s) { g_rng = s ? s : 1; }

/*  GDI objects.  */

static void create_gdi_objects(void) {
  G.hFont = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
      ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "MS Sans Serif");

  G.hbrFace     = (HBRUSH) GetSysColorBrush(COLOR_BTNFACE);
  G.hbrBlack    = (HBRUSH) GetStockObject(BLACK_BRUSH);
  G.hbrShadow   = CreateSolidBrush(RGB(0x80, 0x80, 0x80));
  G.hbrUntested = CreateSolidBrush(RGB(0xC0, 0xC0, 0xC0));
  G.hbrSystem   = CreateSolidBrush(RGB(0x00, 0x80, 0x80));
  G.hbrBadNew   = CreateSolidBrush(RGB(0xC0, 0x00, 0x00));
  G.hbrBadOld   = CreateSolidBrush(RGB(0x60, 0x00, 0x00));
  G.hbrWrite    = CreateSolidBrush(RGB(0x00, 0x00, 0xC0));
  G.hbrVerify   = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
  G.hbrWrongCyl = CreateSolidBrush(RGB(0xFF, 0x80, 0x00));
  G.hbrNoAM     = CreateSolidBrush(RGB(0x80, 0x00, 0x80));

  /*  Fragmentation palette - four contrasting hues so adjacent files in
      the FAT visibly separate when fragmented.  */
  G.hbrData[0] = CreateSolidBrush(RGB(0x40, 0x80, 0xC8));   /*  sky blue  */
  G.hbrData[1] = CreateSolidBrush(RGB(0x60, 0xA8, 0x60));   /*  forest  */
  G.hbrData[2] = CreateSolidBrush(RGB(0xC8, 0x90, 0x40));   /*  warm tan  */
  G.hbrData[3] = CreateSolidBrush(RGB(0x90, 0x60, 0xC0));   /*  purple  */

  /*  Scan trail - fades from hot yellow to mid grey before the cell
      settles into its permanent colour.  */
  G.hbrTrail[0] = CreateSolidBrush(RGB(0xFF, 0xFF, 0x80));
  G.hbrTrail[1] = CreateSolidBrush(RGB(0xE0, 0xD0, 0x80));
  G.hbrTrail[2] = CreateSolidBrush(RGB(0xB0, 0xA8, 0x80));
  G.hbrTrail[3] = CreateSolidBrush(RGB(0x90, 0x90, 0x90));

  G.penShadow = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
  G.penHilite = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNHIGHLIGHT));

  G.scan_anim_phase = 0;
  trail_clear();
  update_scan_brush();
}

static void destroy_gdi_objects(void) {
  if (G.hFont)      DeleteObject(G.hFont);
  if (G.hbrShadow)  DeleteObject(G.hbrShadow);
  if (G.hbrUntested) DeleteObject(G.hbrUntested);
  if (G.hbrSystem)  DeleteObject(G.hbrSystem);
  if (G.hbrBadNew)  DeleteObject(G.hbrBadNew);
  if (G.hbrBadOld)  DeleteObject(G.hbrBadOld);
  if (G.hbrWrite)   DeleteObject(G.hbrWrite);
  if (G.hbrVerify)  DeleteObject(G.hbrVerify);
  if (G.hbrWrongCyl) DeleteObject(G.hbrWrongCyl);
  if (G.hbrNoAM)    DeleteObject(G.hbrNoAM);
  for (int i = 0; i < 4; ++i) {
    if (G.hbrData[i])  DeleteObject(G.hbrData[i]);
    if (G.hbrTrail[i]) DeleteObject(G.hbrTrail[i]);
  }
  if (G.hbrScan)    DeleteObject(G.hbrScan);
  if (G.hbmScan)    DeleteObject(G.hbmScan);
  if (G.penShadow)  DeleteObject(G.penShadow);
  if (G.penHilite)  DeleteObject(G.penHilite);
}

/*  Main window procedure.  */

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE:
      G.hMain = h;
      create_controls(h);
      return 0;

    case WM_SIZE:
      layout_apply(h);
      return 0;

    case WM_GETMINMAXINFO: {
      MINMAXINFO * mmi = (MINMAXINFO *) lp;
      mmi->ptMinTrackSize.x = 520;
      mmi->ptMinTrackSize.y = 480;
      return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      SetBkColor((HDC) wp, GetSysColor(COLOR_BTNFACE));
      return (LRESULT) G.hbrFace;

    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(h, &r);
      FillRect((HDC) wp, &r, G.hbrFace);
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(h, &ps);
      paint_legend(hdc);
      EndPaint(h, &ps);
      return 0;
    }

    case WM_TIMER:
      if (wp == 1) {
        /*  refresh the progress bar + status plinth  */
        if (G.total_sec > 0) {
          DWORD pct = (G.scanned * 1000u) / (DWORD) G.total_sec;
          SendMessageA(G.hProgress, PBM_SETPOS, pct, 0);
        }
        InvalidateRect(h, &G.rcPlinth, FALSE);
      } else if (wp == 2) {
        /*  animate the hatch, age the scan trail  */
        G.scan_anim_phase = (G.scan_anim_phase + 1) & 3;
        update_scan_brush();
        trail_tick();
        if (G.current_sec >= 0) grid_invalidate_cell(G.current_sec);
      }
      return 0;

    /*  Alt+mnemonics.  */
    case WM_SYSCHAR: {
      HWND t = NULL;
      switch (wp | 0x20) {                   /*  fold to lower case  */
        case 's': t = G.hStandard;   break;
        case 't': t = G.hThorough;   break;
        case 'd': t = G.hDiagnostic; break;
        case 'f': t = G.hChkfs;      break;
        case 'm': t = G.hAutoFix;    break;
        case 'b': t = G.hBatch;      break;
        case 'a': t = G.hStart;      break;
        case 'p': t = G.hStop;       break;
        case 'r': t = G.hRecover;    break;
        case 'o': t = G.hFormat;     break;
        case 'g': t = G.hDefrag;     break;
        case 'l': t = G.hLogs;       break;
        case 'u': t = G.hAbout;      break;
        case 'c': t = G.hClose;      break;
        default: break;
      }
      if (t && IsWindowVisible(t) && IsWindowEnabled(t)) {
        SetFocus(t);
        SendMessageA(t, BM_CLICK, 0, 0);
        return 1;                            /*  handled - see the loop  */
      }
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDOK:
          if (IsWindowEnabled(G.hStart)) start_scan(h);
          return 0;
        case ID_START:   start_scan(h);       return 0;
        case ID_STOP:    stop_scan(h);        return 0;
        case ID_RECOVER: do_recover_flow(h);  return 0;
        case ID_FORMAT:  do_format_flow(h);   return 0;
        case ID_DEFRAG:  do_defrag_flow(h);   return 0;
        case ID_LOGS:    do_logs(h);          return 0;
        case ID_ABOUT:   do_about(h);         return 0;
        case ID_CLOSE:
          if (G.running) {
            InterlockedExchange(&G.closing,   1);
            InterlockedExchange(&G.abort_req, 1);
            EnableWindow(G.hStart,      FALSE);
            EnableWindow(G.hStop,       FALSE);
            EnableWindow(G.hRecover,    FALSE);
            EnableWindow(G.hFormat,     FALSE);
            EnableWindow(G.hDefrag,     FALSE);
            EnableWindow(G.hClose,      FALSE);
            EnableWindow(G.hAbout,      FALSE);
            EnableWindow(G.hLogs,       FALSE);
            EnableWindow(G.hStandard,   FALSE);
            EnableWindow(G.hThorough,   FALSE);
            EnableWindow(G.hDiagnostic, FALSE);
            EnableWindow(G.hChkfs,      FALSE);
            EnableWindow(G.hAutoFix,    FALSE);
            EnableWindow(G.hBatch,      FALSE);
            EnableWindow(G.hDriveA,     FALSE);
            EnableWindow(G.hDriveB,     FALSE);
            HMENU hm = GetSystemMenu(h, FALSE);
            if (hm) EnableMenuItem(hm, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
            SendMessageA(G.hStatus, SB_SETTEXTA, 2,
                (LPARAM) "Stopping safely - restoring current sector...");
            SetCursor(LoadCursorA(NULL, MAKEINTRESOURCEA(32514)));  /*  WAIT  */
            return 0;
          }
          DestroyWindow(h);
          return 0;
      }
      return 0;

    case WM_APP_PROGRESS:
      if (lp > 0) {
        DWORD pct = ((DWORD) wp * 1000u) / (DWORD) lp;
        SendMessageA(G.hProgress, PBM_SETPOS, pct, 0);
      }
      return 0;

    case WM_APP_REPAINT:
      grid_calc(G.hGrid);
      InvalidateRect(G.hGrid, NULL, FALSE);
      return 0;

    case WM_APP_DONE:
      on_done(h);
      if (G.closing) DestroyWindow(h);   /*  queued close; worker is out  */
      return 0;

    case WM_QUERYENDSESSION:
      /*  Refuse shutdown while writing; signal abort to buy time.  */
      if (G.running) {
        InterlockedExchange(&G.abort_req, 1);
        return FALSE;
      }
      return TRUE;

    case WM_ENDSESSION:
      if (wp && G.running && G.hThread)
        WaitForSingleObject(G.hThread, 3000);
      return 0;

    case WM_CLOSE:
      /*  Route Alt-F4 / system-menu X through the safe-abort path.  */
      SendMessageA(h, WM_COMMAND, ID_CLOSE, 0);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

/*  Window-class registration and entry point.  */

static ATOM register_classes(HINSTANCE hi) {
  WNDCLASSA wc;
  memzero(&wc, sizeof wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc   = main_proc;
  wc.hInstance     = hi;
  wc.hIcon         = LoadIconA(hi, MAKEINTRESOURCEA(1));
  wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
  wc.lpszClassName = APP_CLASS;
  if (!RegisterClassA(&wc)) return 0;
  memzero(&wc, sizeof wc);
  wc.lpfnWndProc   = grid_proc;
  wc.hInstance     = hi;
  wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
  wc.hbrBackground = NULL;
  wc.lpszClassName = "FdchkGrid";
  return RegisterClassA(&wc);
}

/*  Entry point.  */
void WinMainCRTStartup(void) {
  G.hInst = GetModuleHandleA(NULL);
  G.current_sec = -1;

  /*  Heap-allocate large buffers.  */
  G.state   = (BYTE *)  LocalAlloc(LPTR, MAX_SECTORS);
  G.bad_lba = (DWORD *) LocalAlloc(LPTR, MAX_BAD * sizeof(DWORD));
  if (!G.state || !G.bad_lba) ExitProcess(1);

  init_log_dir();

  /*  Probe which floppy letters exist so we can hide the absent radio.  */
  G.has_a = (GetDriveTypeA("A:\\") == DRIVE_REMOVABLE);
  G.has_b = (GetDriveTypeA("B:\\") == DRIVE_REMOVABLE);

  /*  Attach the FDC VxD.  */
  vxd_open();
  G.drive_type[0] = G.drive_type[1] = DEV_UNKNOWN;
  for (int i = 0; i < 2; ++i) {
    DiskHandle d;
    if (!(i ? G.has_b : G.has_a)) continue;
    if (!disk_open(&d, i)) continue;
    G.drive_type[i] = disk_probe_drive_type(&d);
    disk_close(&d);
  }

  INITCOMMONCONTROLSEX icc;
  icc.dwSize = sizeof icc;
  icc.dwICC  = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&icc);

  create_gdi_objects();
  register_classes(G.hInst);

  int sw = GetSystemMetrics(SM_CXSCREEN);
  int sh = GetSystemMetrics(SM_CYSCREEN);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT rc = { 0, 0, WND_W, WND_H };
  AdjustWindowRect(&rc, style, FALSE);

  HWND hWnd = CreateWindowExA(0, APP_CLASS, APP_NAME " - A:", style,
      (sw - WND_W) / 2, (sh - WND_H) / 2,
      rc.right - rc.left, rc.bottom - rc.top,
      NULL, NULL, G.hInst, NULL);
  if (!hWnd) ExitProcess(1);

  lstrcpyA(G.status, "Ready.  Insert a floppy disk and click Start.");

  {
    const FloppyGeom * g = geom_for_drive_type(G.drive_type[0]);
    geom_apply(g ? g : geom_for_size(2880));
  }

  ShowWindow(hWnd, SW_SHOW);
  UpdateWindow(hWnd);

  MSG msg;
  while (GetMessageA(&msg, NULL, 0, 0)) {
    /*  IsDialogMessage must target the window owning the focused
        control, else keystrokes meant for a popup leak into main.  */
    HWND active = GetActiveWindow();
    if (!active) active = hWnd;
    if (msg.message == WM_SYSKEYDOWN && msg.wParam >= 'A' && msg.wParam <= 'Z' &&
        SendMessageA(active, WM_SYSCHAR, msg.wParam, msg.lParam))
      continue;
    if (!IsDialogMessageA(active, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }

  destroy_gdi_objects();
  vxd_close();
  ExitProcess((UINT) msg.wParam);
}
