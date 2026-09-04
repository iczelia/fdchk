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
#include <stdarg.h>

app G;

void * memset(void * d, int c, size_t n) {
  void * r = d;
  __asm__ volatile ("rep stosb" : "+D" (d), "+c" (n) : "a" (c) : "memory");
  return r;
}

void * memcpy(void * d, const void * s, size_t n) {
  void * r = d;
  __asm__ volatile ("rep movsb" : "+D" (d), "+S" (s), "+c" (n) : : "memory");
  return r;
}

int memcmp(const void * a, const void * b, size_t n) {
  int r = 0;
  if (n)
    __asm__ volatile ("repz cmpsb\n\t"
                     "movzbl -1(%1), %0\n\t"
                     "movzbl -1(%2), %%edx\n\t"
                     "subl %%edx, %0"
                     : "+a" (r), "+S" (a), "+D" (b), "+c" (n)
                     :
                     : "edx", "cc", "memory");
  return r;
}

int x_strlen(const char * s) {
  int n = -1;
  __asm__ volatile ("repne scasb"
                   : "+c" (n), "+D" (s)
                   : "a" (0)
                   : "cc", "memory");
  return -n - 2;
}

char * x_strcpy(char * d, const char * s) {
  char * r = d;
  __asm__ volatile ("1:\tlodsb\n\t"
                   "stosb\n\t"
                   "testb %%al, %%al\n\t"
                   "jnz 1b"
                   : "+S" (s), "+D" (d)
                   :
                   : "eax", "cc", "memory");
  return r;
}

char * x_strcpyn(char * d, const char * s, int n) {
  char * r = d;
  if (n > 0) {
    unsigned c = (unsigned) n - 1;
    __asm__ volatile ("jecxz 2f\n"
                     "1:\tlodsb\n\t"
                     "stosb\n\t"
                     "testb %%al, %%al\n\t"
                     "jz 3f\n\t"
                     "decl %%ecx\n\t"
                     "jnz 1b\n"
                     "2:\tmovb $0, (%%edi)\n"
                     "3:"
                     : "+S" (s), "+D" (d), "+c" (c)
                     :
                     : "eax", "cc", "memory");
  }
  return r;
}

/*  Minimal printf: - and 0 flags, width, ignored lengths, and the c, d, i,
    u, x, X, s, and % conversions.  */
int x_sprintf(char * out, const char * fmt, ...) {
  char * o = out;
  va_list ap;
  va_start(ap, fmt);
  for (; *fmt; fmt++) {
    if (*fmt != '%') { *o++ = *fmt; continue; }
    int left = 0, zero = 0, width = 0;
    for (fmt++; *fmt == '-' || *fmt == '0'; fmt++) {
      if (*fmt == '-') left = 1;
      else             zero = 1;
    }
    for (; *fmt >= '0' && *fmt <= '9'; fmt++)
      width = width * 10 + (*fmt - '0');
    while (*fmt == 'l' || *fmt == 'h') fmt++;

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

/*  Create <program-directory>\logs.  */
void init_log_dir(void) {
  char exe[MAX_PATH];
  char * p;
  GetModuleFileNameA(NULL, exe, MAX_PATH);
  char * last = exe;
  for (p = exe; *p; p++)
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
static DWORD rng_state = 0x13579BDFu;
DWORD rng_next(void) {
  DWORD x = rng_state;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  rng_state = x;
  return x;
}
void rng_seed(DWORD s) { rng_state = s ? s : 1; }

/*  GDI objects.  */

static void create_gdi_objects(void) {
  G.font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
      ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "MS Sans Serif");

  G.face_brush = (HBRUSH) GetSysColorBrush(COLOR_BTNFACE);
  G.black_brush = (HBRUSH) GetStockObject(BLACK_BRUSH);
  G.shadow_brush = CreateSolidBrush(RGB(0x80, 0x80, 0x80));
  G.untested_brush = CreateSolidBrush(RGB(0xC0, 0xC0, 0xC0));
  G.system_brush = CreateSolidBrush(RGB(0x00, 0x80, 0x80));
  G.bad_new_brush = CreateSolidBrush(RGB(0xC0, 0x00, 0x00));
  G.bad_old_brush = CreateSolidBrush(RGB(0x60, 0x00, 0x00));
  G.write_brush = CreateSolidBrush(RGB(0x00, 0x00, 0xC0));
  G.verify_brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
  G.wrong_cyl_brush = CreateSolidBrush(RGB(0xFF, 0x80, 0x00));
  G.no_am_brush = CreateSolidBrush(RGB(0x80, 0x00, 0x80));

  /*  Alternating file-chain colours.  */
  G.data_brush[0] = CreateSolidBrush(RGB(0x40, 0x80, 0xC8));   /*  sky blue  */
  G.data_brush[1] = CreateSolidBrush(RGB(0x60, 0xA8, 0x60));   /*  forest  */
  G.data_brush[2] = CreateSolidBrush(RGB(0xC8, 0x90, 0x40));   /*  warm tan  */
  G.data_brush[3] = CreateSolidBrush(RGB(0x90, 0x60, 0xC0));   /*  purple  */

  /*  Bright-to-grey scan trail.  */
  G.trail_brush[0] = CreateSolidBrush(RGB(0xFF, 0xFF, 0x80));
  G.trail_brush[1] = CreateSolidBrush(RGB(0xE0, 0xD0, 0x80));
  G.trail_brush[2] = CreateSolidBrush(RGB(0xB0, 0xA8, 0x80));
  G.trail_brush[3] = CreateSolidBrush(RGB(0x90, 0x90, 0x90));

  G.shadow_pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
  G.highlight_pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNHIGHLIGHT));

  G.scan_anim_phase = 0;
  trail_clear();
  update_scan_brush();
}

static void destroy_gdi_objects(void) {
  int i;
  if (G.font) DeleteObject(G.font);
  if (G.shadow_brush) DeleteObject(G.shadow_brush);
  if (G.untested_brush) DeleteObject(G.untested_brush);
  if (G.system_brush) DeleteObject(G.system_brush);
  if (G.bad_new_brush) DeleteObject(G.bad_new_brush);
  if (G.bad_old_brush) DeleteObject(G.bad_old_brush);
  if (G.write_brush) DeleteObject(G.write_brush);
  if (G.verify_brush) DeleteObject(G.verify_brush);
  if (G.wrong_cyl_brush) DeleteObject(G.wrong_cyl_brush);
  if (G.no_am_brush) DeleteObject(G.no_am_brush);
  Fi(4,
    if (G.data_brush[i])  DeleteObject(G.data_brush[i]);
    if (G.trail_brush[i]) DeleteObject(G.trail_brush[i]);
  );
  if (G.scan_brush) DeleteObject(G.scan_brush);
  if (G.scan_bitmap) DeleteObject(G.scan_bitmap);
  if (G.shadow_pen) DeleteObject(G.shadow_pen);
  if (G.highlight_pen) DeleteObject(G.highlight_pen);
}

/*  Main window procedure.  */

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE:
      G.main = h;
      ui_create(h);
      return 0;

    case WM_SIZE:
      ui_layout(h);
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
      return (LRESULT) G.face_brush;

    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(h, &r);
      FillRect((HDC) wp, &r, G.face_brush);
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(h, &ps);
      ui_paint_legend(hdc);
      EndPaint(h, &ps);
      return 0;
    }

    case WM_TIMER:
      if (wp == 1) {
        /*  Refresh progress and status.  */
        if (G.total_sec > 0) {
          DWORD pct = (G.scanned * 1000u) / (DWORD) G.total_sec;
          SendMessageA(G.progress, PBM_SETPOS, pct, 0);
        }
        InvalidateRect(h, &G.plinth, FALSE);
      } else if (wp == 2) {
        /*  Animate the scan.  */
        G.scan_anim_phase = (G.scan_anim_phase + 1) & 3;
        update_scan_brush();
        trail_tick();
        if (G.current_sec >= 0) grid_invalidate_cell(G.current_sec);
      }
      return 0;

    /*  Alt+mnemonics.  */
    case WM_SYSCHAR: {
      HWND t = NULL;
      switch (wp | 0x20) {                   /*  ASCII lower case  */
        case 's': t = G.standard;   break;
        case 't': t = G.thorough;   break;
        case 'd': t = G.diagnostic; break;
        case 'f': t = G.chkfs;      break;
        case 'm': t = G.auto_fix_button;    break;
        case 'b': t = G.batch_button;      break;
        case 'a': t = G.start_button;      break;
        case 'p': t = G.stop_button;       break;
        case 'r': t = G.recover_button;    break;
        case 'o': t = G.format_button;     break;
        case 'g': t = G.defrag_button;     break;
        case 'l': t = G.logs_button;       break;
        case 'u': t = G.about_button;      break;
        case 'c': t = G.close_button;      break;
        default: break;
      }
      if (t && IsWindowVisible(t) && IsWindowEnabled(t)) {
        SetFocus(t);
        SendMessageA(t, BM_CLICK, 0, 0);
        return 1;
      }
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDOK:
          if (IsWindowEnabled(G.start_button)) ui_start_scan(h);
          return 0;
        case ID_START:   ui_start_scan(h);       return 0;
        case ID_STOP:    ui_stop_scan(h);        return 0;
        case ID_RECOVER: ui_recover(h);  return 0;
        case ID_FORMAT:  ui_format(h);   return 0;
        case ID_DEFRAG:  ui_defrag(h);   return 0;
        case ID_LOGS:    ui_logs(h);          return 0;
        case ID_ABOUT:   ui_about(h);         return 0;
        case ID_CLOSE:
          if (G.running) {
            InterlockedExchange(&G.closing,   1);
            InterlockedExchange(&G.abort_req, 1);
            EnableWindow(G.start_button,      FALSE);
            EnableWindow(G.stop_button,       FALSE);
            EnableWindow(G.recover_button,    FALSE);
            EnableWindow(G.format_button,     FALSE);
            EnableWindow(G.defrag_button,     FALSE);
            EnableWindow(G.close_button,      FALSE);
            EnableWindow(G.about_button,      FALSE);
            EnableWindow(G.logs_button,       FALSE);
            EnableWindow(G.standard,   FALSE);
            EnableWindow(G.thorough,   FALSE);
            EnableWindow(G.diagnostic, FALSE);
            EnableWindow(G.chkfs,      FALSE);
            EnableWindow(G.auto_fix_button,    FALSE);
            EnableWindow(G.batch_button,      FALSE);
            EnableWindow(G.drive_a,     FALSE);
            EnableWindow(G.drive_b,     FALSE);
            HMENU hm = GetSystemMenu(h, FALSE);
            if (hm) EnableMenuItem(hm, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
            SendMessageA(G.statusbar, SB_SETTEXTA, 2,
                (LPARAM) "Stopping: writing the original sector data back...");
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
        SendMessageA(G.progress, PBM_SETPOS, pct, 0);
      }
      return 0;

    case WM_APP_REPAINT:
      grid_calc(G.grid);
      InvalidateRect(G.grid, NULL, FALSE);
      return 0;

    case WM_APP_DONE:
      ui_worker_done(h);
      if (G.closing) DestroyWindow(h);
      return 0;

    case WM_QUERYENDSESSION:
      /*  Block shutdown until the write stops.  */
      if (G.running) {
        InterlockedExchange(&G.abort_req, 1);
        return FALSE;
      }
      return TRUE;

    case WM_ENDSESSION:
      if (wp && G.running && G.thread)
        WaitForSingleObject(G.thread, 3000);
      return 0;

    case WM_CLOSE:
      /*  Stop the worker before closing.  */
      SendMessageA(h, WM_COMMAND, ID_CLOSE, 0);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

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

static int run(void) {
  int i;
  G.instance = GetModuleHandleA(NULL);
  G.current_sec = -1;

  G.state   = (BYTE *) LocalAlloc(LPTR, MAX_SECTORS);
  G.bad_lba = (DWORD *) LocalAlloc(LPTR, MAX_BAD * sizeof(DWORD));
  if (!G.state || !G.bad_lba) ExitProcess(1);

  init_log_dir();

  /*  Find the floppy drive letters.  */
  G.has_a = (GetDriveTypeA("A:\\") == DRIVE_REMOVABLE);
  G.has_b = (GetDriveTypeA("B:\\") == DRIVE_REMOVABLE);

  /*  Load the raw FDC driver.  */
  vxd_open();
  G.drive_type[0] = G.drive_type[1] = DEV_UNKNOWN;
  Fi(2,
    disk_handle d;
    if (!(i ? G.has_b : G.has_a)) continue;
    if (!disk_open(&d, i)) continue;
    G.drive_type[i] = disk_probe_drive_type(&d);
    disk_close(&d);
  );

  INITCOMMONCONTROLSEX icc;
  icc.dwSize = sizeof icc;
  icc.dwICC  = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&icc);

  create_gdi_objects();
  register_classes(G.instance);

  int sw = GetSystemMetrics(SM_CXSCREEN);
  int sh = GetSystemMetrics(SM_CYSCREEN);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT rc = { 0, 0, WND_W, WND_H };
  AdjustWindowRect(&rc, style, FALSE);

  HWND window = CreateWindowExA(0, APP_CLASS, APP_NAME " - A:", style,
      (sw - WND_W) / 2, (sh - WND_H) / 2,
      rc.right - rc.left, rc.bottom - rc.top,
      NULL, NULL, G.instance, NULL);
  if (!window) ExitProcess(1);

  lstrcpyA(G.status, "Ready. Insert a floppy and click Start.");

  {
    const floppy_geom * g = geom_for_drive_type(G.drive_type[0]);
    geom_apply(g ? g : geom_for_size(2880));
  }

  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);

  MSG msg;
  while (GetMessageA(&msg, NULL, 0, 0)) {
    HWND active = GetActiveWindow();
    if (!active) active = window;
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
  return (int) msg.wParam;
}

#ifdef DEBUG
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous,
                   LPSTR command, int show) {
  return run();
}
#else
void WinMainCRTStartup(void) { ExitProcess((UINT) run()); }
#endif
