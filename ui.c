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

#include "fdchk.h"

/*  Single-line bevel: dark top/left, light bottom/right when sunken.  */
static void draw_3d_rect(HDC hdc, const RECT * r, BOOL sunken) {
  HPEN tl = sunken ? G.penShadow : G.penHilite;
  HPEN br = sunken ? G.penHilite : G.penShadow;
  HPEN old = (HPEN) SelectObject(hdc, tl);
  POINT pt;
  MoveToEx(hdc, r->left, r->bottom - 1, &pt);
  LineTo(hdc, r->left, r->top);
  LineTo(hdc, r->right, r->top);
  SelectObject(hdc, br);
  MoveToEx(hdc, r->right - 1, r->top, &pt);
  LineTo(hdc, r->right - 1, r->bottom - 1);
  LineTo(hdc, r->left - 1, r->bottom - 1);
  SelectObject(hdc, old);
}

/*  8x8 dithered yellow/orange checker, rotated by phase, for the
    animated scanning brush.  */
static HBITMAP make_hatch_bitmap(int phase) {
  BITMAPINFO bi;
  BYTE bits[8 * 8 * 4];
  memzero(&bi, sizeof bi);
  bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth       = 8;
  bi.bmiHeader.biHeight      = 8;
  bi.bmiHeader.biPlanes      = 1;
  bi.bmiHeader.biBitCount    = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      DWORD c = (((x + y + phase) >> 1) & 1) ? 0x00FFFF00 : 0x00808000;
      BYTE * p = bits + (y * 8 + x) * 4;
      p[0] = (BYTE) c;
      p[1] = (BYTE) (c >> 8);
      p[2] = (BYTE) (c >> 16);
      p[3] = 0;
    }
  HDC sdc = GetDC(NULL);
  HBITMAP bmp = CreateDIBitmap(sdc, &bi.bmiHeader, CBM_INIT, bits,
                               &bi, DIB_RGB_COLORS);
  ReleaseDC(NULL, sdc);
  return bmp;
}

void update_scan_brush(void) {
  if (G.hbrScan) DeleteObject(G.hbrScan);
  if (G.hbmScan) DeleteObject(G.hbmScan);
  G.hbmScan = make_hatch_bitmap(G.scan_anim_phase);
  G.hbrScan = CreatePatternBrush(G.hbmScan);
}

/*  Cluster grid - owner-drawn child window.  */

#define CELL_SIZE 9
#define CELL_GAP  1      /*  1-px black gutter between cells  */
#define GRID_PAD  4

static struct { int cols, rows, cell; } grid_geom;

void grid_calc(HWND h) {
  RECT r;
  GetClientRect(h, &r);
  int w  = r.right  - r.left - GRID_PAD * 2;
  int hh = r.bottom - r.top  - GRID_PAD * 2;
  int total = G.total_sec > 0 ? G.total_sec : MAX_SECTORS;
  /*  Called before the first WM_SIZE: stash a placeholder so we never
      divide by zero; the real pass redoes the maths.  */
  if (w < CELL_SIZE * 2 || hh < CELL_SIZE * 2) {
    grid_geom.cols = 1;
    grid_geom.rows = total;
    grid_geom.cell = CELL_SIZE;
    return;
  }
  int cell = CELL_SIZE;
  int cols = w / cell; if (cols < 1) cols = 1;
  int rows = (total + cols - 1) / cols;
  while (rows * cell > hh && cell > 3) {
    cell--;
    cols = w / cell; if (cols < 1) cols = 1;
    rows = (total + cols - 1) / cols;
  }
  grid_geom.cols = cols;
  grid_geom.rows = rows;
  grid_geom.cell = cell;
}

static HBRUSH brush_for_state(BYTE s) {
  switch (s) {
    case ST_GOOD:      return G.hbrShadow;
    case ST_SYSTEM:    return G.hbrSystem;
    case ST_SCANNING:  return G.hbrScan;
    case ST_WRITING:   return G.hbrWrite;
    case ST_VERIFY:    return G.hbrVerify;
    case ST_BAD_NEW:   return G.hbrBadNew;
    case ST_BAD_OLD:   return G.hbrBadOld;
    case ST_WRONG_CYL: return G.hbrWrongCyl;
    case ST_NO_AM:     return G.hbrNoAM;
    case ST_DATA_0: case ST_DATA_1:
    case ST_DATA_2: case ST_DATA_3:
      return G.hbrData[s - ST_DATA_0];
    case ST_TRAIL_0: case ST_TRAIL_1:
    case ST_TRAIL_2: case ST_TRAIL_3:
      return G.hbrTrail[s - ST_TRAIL_0];
    default:           return G.hbrUntested;
  }
}

static void grid_cell_rect(int idx, RECT * out) {
  int col = idx % grid_geom.cols;
  int row = idx / grid_geom.cols;
  out->left   = GRID_PAD + col * grid_geom.cell;
  out->top    = GRID_PAD + row * grid_geom.cell;
  out->right  = out->left + grid_geom.cell - CELL_GAP;
  out->bottom = out->top  + grid_geom.cell - CELL_GAP;
}

/*  Invalidate just one cell so we don't repaint everything per sector.  */
void grid_invalidate_cell(int idx) {
  if (!G.hGrid) return;
  RECT cr;
  grid_cell_rect(idx, &cr);
  InvalidateRect(G.hGrid, &cr, FALSE);
}

/*  Scan trail - a 4-cell fading tail behind the scanner.  */

/*  Push a sector onto the trail; the newest entry replaces the oldest.  */
static void trail_push(int sec) {
  int slot = 0, max_age = G.trail_age[0];
  for (int i = 1; i < 4; ++i) {
    if (G.trail_age[i] < 0) { slot = i; break; }
    if (G.trail_age[i] > max_age) { max_age = G.trail_age[i]; slot = i; }
  }
  int prev = G.trail_sec[slot];
  G.trail_sec[slot] = sec;
  G.trail_age[slot] = 0;
  if (prev >= 0) grid_invalidate_cell(prev);
  grid_invalidate_cell(sec);
}

/*  Age every trail entry; expire those past the visible range.  */
void trail_tick(void) {
  for (int i = 0; i < 4; ++i) {
    if (G.trail_age[i] < 0) continue;
    int sec = G.trail_sec[i];
    if (++G.trail_age[i] >= 4) {
      G.trail_sec[i] = -1;
      G.trail_age[i] = -1;
    }
    if (sec >= 0) grid_invalidate_cell(sec);
  }
}

/*  Trail age (0..3) for a sector, or -1 if it is not in the trail.  */
static int trail_age_of(int sec) {
  for (int i = 0; i < 4; ++i)
    if (G.trail_sec[i] == sec && G.trail_age[i] >= 0)
      return G.trail_age[i];
  return -1;
}

void trail_clear(void) {
  for (int i = 0; i < 4; ++i) {
    int sec = G.trail_sec[i];
    G.trail_sec[i] = -1;
    G.trail_age[i] = -1;
    if (sec >= 0) grid_invalidate_cell(sec);
  }
}

static void grid_paint(HWND h, HDC hdc, const RECT * clip) {
  RECT cli;
  GetClientRect(h, &cli);
  draw_3d_rect(hdc, &cli, TRUE);

  RECT inner = { cli.left + 1, cli.top + 1, cli.right - 1, cli.bottom - 1 };
  FillRect(hdc, &inner, G.hbrBlack);
  if (grid_geom.cols == 0) return;

  int total = G.total_sec ? G.total_sec : MAX_SECTORS;
  for (int i = 0; i < total; ++i) {
    RECT cr;
    grid_cell_rect(i, &cr);
    if (cr.right < clip->left || cr.left > clip->right ||
        cr.bottom < clip->top || cr.top > clip->bottom)
      continue;
    BYTE st = G.state[i];
    /*  A freshly-scanned ST_GOOD cell wears its trail tint until it
        fades; every other state keeps its own colour.  */
    int age = (st == ST_GOOD) ? trail_age_of(i) : -1;
    FillRect(hdc, &cr,
             (age >= 0 && age < 4) ? G.hbrTrail[age] : brush_for_state(st));
  }
}

LRESULT CALLBACK grid_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(h, &ps);
      grid_paint(h, hdc, &ps.rcPaint);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      grid_calc(h);
      InvalidateRect(h, NULL, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      /*  Clicking a cell shows its details in the status bar.  */
      int x = LOWORD(lp), y = HIWORD(lp);
      if (grid_geom.cell > 0) {
        int col = (x - GRID_PAD) / grid_geom.cell;
        int row = (y - GRID_PAD) / grid_geom.cell;
        if (col >= 0 && col < grid_geom.cols && row >= 0) {
          int idx = row * grid_geom.cols + col;
          if (idx >= 0 && idx < G.total_sec) {
            int cl = lba_to_cluster((DWORD) idx);
            char buf[80];
            wsprintfA(buf,
                cl >= 0 ? "Sector %d  /  Cluster %d  (state %d)"
                        : "Sector %d  (system area)  state %d",
                idx, cl >= 0 ? cl : G.state[idx], G.state[idx]);
            SendMessageA(G.hStatus, SB_SETTEXTA, 2, (LPARAM) buf);
          }
        }
      }
      return 0;
    }
  }
  return DefWindowProcA(h, m, wp, lp);
}

/*  Worker-thread -> UI bridge.  */

void ui_set_state(int sec, BYTE st) {
  if (!G.state || sec < 0 || sec >= MAX_SECTORS) return;
  BYTE was = G.state[sec];
  /*  Trail-mark only data-area cells turning into a clean ST_GOOD.  */
  if (st == ST_GOOD && was != ST_GOOD &&
      G.data_start_sec > 0 && sec >= G.data_start_sec)
    trail_push(sec);
  G.state[sec] = st;
  grid_invalidate_cell(sec);
}

void ui_status(const char * s) {
  lstrcpynA(G.status, s, sizeof G.status);
  if (G.hStatus)
    SendMessageA(G.hStatus, SB_SETTEXTA, 2, (LPARAM) G.status);
}

int ui_prompt(const char * msg, const char * caption, UINT flags) {
  return MessageBoxA(G.hMain, msg, caption, flags);
}

/*  Main-window controls.  Children are created at zero size; layout_apply
    places them here and on every WM_SIZE.  */

void create_controls(HWND hWnd) {
  HINSTANCE hi = G.hInst;

  G.hGrpDrive = CreateWindowExA(0, "BUTTON", "Select a drive to check",
      WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, hi, NULL);
  G.hDriveA = CreateWindowExA(0, "BUTTON", "&A:  (3.5\" Floppy)",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_DRIVE_A, hi, NULL);
  G.hDriveB = CreateWindowExA(0, "BUTTON", "&B:  (3.5\" Floppy)",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_DRIVE_B, hi, NULL);
  SendMessageA(G.hDriveA, BM_SETCHECK, BST_CHECKED, 0);

  G.hGrpType = CreateWindowExA(0, "BUTTON", "Type of test",
      WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hWnd, NULL, hi, NULL);
  G.hStandard = CreateWindowExA(0, "BUTTON",
      "&Standard - read every sector and report errors (non-destructive)",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_STANDARD, hi, NULL);
  G.hThorough = CreateWindowExA(0, "BUTTON",
      "&Thorough - full write-and-verify surface test (destructive)",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_THOROUGH, hi, NULL);
  G.hDiagnostic = CreateWindowExA(0, "BUTTON",
      "&Diagnostic - probe head/seek/alignment via the FDC",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_DIAGNOSTIC, hi, NULL);
  G.hChkfs = CreateWindowExA(0, "BUTTON",
      "Check &filesystem - walk FAT12 for cross-links and lost chains",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_CHKFS, hi, NULL);
  SendMessageA(G.hThorough, BM_SETCHECK, BST_CHECKED, 0);

  G.hAutoFix = CreateWindowExA(0, "BUTTON",
      "Automatically &mark bad clusters in FAT (auto-fix)",
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_AUTOFIX, hi, NULL);
  SendMessageA(G.hAutoFix, BM_SETCHECK, BST_CHECKED, 0);

  G.hBatch = CreateWindowExA(0, "BUTTON",
      "&Batch mode (after each scan, prompt to insert the next disk)",
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_BATCH, hi, NULL);

  G.hGrid = CreateWindowExA(0, "FdchkGrid", NULL, WS_CHILD | WS_VISIBLE,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_GRID, hi, NULL);

  G.hProgress = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
      WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd,
      (HMENU) (UINT_PTR) ID_PROGRESS, hi, NULL);
  SendMessageA(G.hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));

  G.hStart = CreateWindowExA(0, "BUTTON", "&Start",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_START, hi, NULL);
  G.hStop = CreateWindowExA(0, "BUTTON", "S&top",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_STOP, hi, NULL);
  G.hRecover = CreateWindowExA(0, "BUTTON", "&Recover...",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_RECOVER, hi, NULL);
  G.hFormat = CreateWindowExA(0, "BUTTON", "F&ormat...",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_FORMAT, hi, NULL);
  G.hDefrag = CreateWindowExA(0, "BUTTON", "&Defrag...",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_DEFRAG, hi, NULL);
  G.hLogs = CreateWindowExA(0, "BUTTON", "&Logs...",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_LOGS, hi, NULL);
  G.hAbout = CreateWindowExA(0, "BUTTON", "About...",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_ABOUT, hi, NULL);
  G.hClose = CreateWindowExA(0, "BUTTON", "&Close",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_CLOSE, hi, NULL);

  /*  Propagate the UI font to every child in one sweep.  */
  HWND children[] = {
    G.hGrpDrive, G.hDriveA, G.hDriveB,
    G.hGrpType, G.hStandard, G.hThorough, G.hDiagnostic, G.hChkfs,
    G.hAutoFix, G.hBatch,
    G.hStart, G.hStop, G.hRecover, G.hFormat, G.hDefrag,
    G.hLogs, G.hAbout, G.hClose
  };
  for (size_t i = 0; i < sizeof children / sizeof children[0]; ++i)
    SendMessageA(children[i], WM_SETFONT, (WPARAM) G.hFont, 0);

  /*  Hide a drive radio with no physical drive behind it.  With fewer
      than two floppies the choice is implicit, so drop the group.  */
  if (!G.has_a) ShowWindow(G.hDriveA, SW_HIDE);
  if (!G.has_b) ShowWindow(G.hDriveB, SW_HIDE);
  if (G.has_b && !G.has_a)
    SendMessageA(G.hDriveB, BM_SETCHECK, BST_CHECKED, 0);
  if (!(G.has_a && G.has_b))
    ShowWindow(G.hGrpDrive, SW_HIDE);

  G.hStatus = CreateWindowExA(0, STATUSCLASSNAMEA, "Ready.",
      WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
      0, 0, 0, 0, hWnd, (HMENU) (UINT_PTR) ID_STATUSBAR, hi, NULL);
  SendMessageA(G.hStatus, WM_SETFONT, (WPARAM) G.hFont, 0);
  int parts[3] = { 80, 180, -1 };
  SendMessageA(G.hStatus, SB_SETPARTS, 3, (LPARAM) parts);
  SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM) "Drive: ?");
  SendMessageA(G.hStatus, SB_SETTEXTA, 1, (LPARAM) "FAT?");
  SendMessageA(G.hStatus, SB_SETTEXTA, 2, (LPARAM) "Ready.");
}

/*  Recompute child positions from the client rect on every WM_SIZE.  */
void layout_apply(HWND hWnd) {
  if (!G.hMain || !G.hGrid) return;
  RECT cli;
  GetClientRect(hWnd, &cli);

  /*  The status bar self-sizes; trim it off the working area.  */
  if (G.hStatus) {
    SendMessageA(G.hStatus, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(G.hStatus, &sb);
    POINT pt = { sb.left, sb.top };
    ScreenToClient(hWnd, &pt);
    cli.bottom = pt.y;
    int parts[3] = { 80, 180, -1 };
    SendMessageA(G.hStatus, SB_SETPARTS, 3, (LPARAM) parts);
  }

  const int PAD = 8, GAP = 6, RADIO_H = 18, CHK_H = 18, BTNH = 24;
  int x = cli.left + PAD;
  int W = cli.right - cli.left - PAD * 2;
  int y = cli.top + PAD;

  /*  Drive group - skipped when hidden (0 or 1 floppies).  */
  if (IsWindowVisible(G.hGrpDrive)) {
    int h = 18 + RADIO_H + 12;
    MoveWindow(G.hGrpDrive, x, y, W, h, TRUE);
    int rw = (W - 36) / 2;
    MoveWindow(G.hDriveA, x + 12,      y + 16, rw, RADIO_H, TRUE);
    MoveWindow(G.hDriveB, x + 24 + rw, y + 16, rw, RADIO_H, TRUE);
    y += h + GAP;
  }

  /*  Test-type group.  */
  {
    int h = 18 + 4 * RADIO_H + 6 + CHK_H + 4 + CHK_H + 8;
    MoveWindow(G.hGrpType, x, y, W, h, TRUE);
    int tw = W - 24, ty = y + 16;
    MoveWindow(G.hStandard,   x + 12, ty,                 tw, RADIO_H, TRUE);
    MoveWindow(G.hThorough,   x + 12, ty + RADIO_H,       tw, RADIO_H, TRUE);
    MoveWindow(G.hDiagnostic, x + 12, ty + 2 * RADIO_H,   tw, RADIO_H, TRUE);
    MoveWindow(G.hChkfs,      x + 12, ty + 3 * RADIO_H,   tw, RADIO_H, TRUE);
    ty += 4 * RADIO_H + 6;
    MoveWindow(G.hAutoFix,    x + 12, ty,                 tw, CHK_H, TRUE);
    MoveWindow(G.hBatch,      x + 12, ty + CHK_H + 4,     tw, CHK_H, TRUE);
    y += h + GAP;
  }

  /*  Button row, anchored to the bottom.  */
  int by = cli.bottom - PAD - BTNH;
  HWND btns[] = { G.hStart, G.hStop, G.hRecover, G.hFormat,
                  G.hDefrag, G.hLogs, G.hAbout, G.hClose };
  int n_btn = (int) (sizeof btns / sizeof btns[0]);
  int btn_w = (W - GAP * (n_btn - 1)) / n_btn;
  if (btn_w < 60) btn_w = 60;
  for (int i = 0, bx = x; i < n_btn; ++i, bx += btn_w + GAP)
    MoveWindow(btns[i], bx, by, btn_w, BTNH, TRUE);

  /*  Status plinth: room for one legend row plus two text lines.  */
  int plinth_h = 6 + 14 + 4 + 14 + 4 + 14 + 6;
  int plinth_y = by - GAP - plinth_h;
  SetRect(&G.rcPlinth, x, plinth_y, x + W, plinth_y + plinth_h);

  /*  Progress bar above the plinth.  */
  int prog_h = 18, prog_y = plinth_y - GAP - prog_h;
  MoveWindow(G.hProgress, x, prog_y, W, prog_h, TRUE);

  /*  Cluster grid fills the flexible middle band.  */
  int grid_h = prog_y - GAP - y;
  if (grid_h < 60) grid_h = 60;
  MoveWindow(G.hGrid, x, y, W, grid_h, TRUE);

  InvalidateRect(hWnd, NULL, TRUE);
}

/*  Paint the legend strip and the dynamic status text on the plinth.  */
void paint_legend(HDC hdc) {
  RECT plinth = G.rcPlinth;
  if (plinth.right <= plinth.left || plinth.bottom <= plinth.top) return;
  DrawEdge(hdc, &plinth, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
  FillRect(hdc, &plinth, G.hbrFace);
  int lx = plinth.left + 6, ly = plinth.top + 6;

  HFONT oldf = (HFONT) SelectObject(hdc, G.hFont);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

  /*  The legend tracks G.mode, which persists after a scan, so it always
      matches what the grid is showing.  */
  HBRUSH brs[8];
  const char * labs[8];
  int n_items;
  if (G.mode == MODE_DIAGNOSTIC) {
    HBRUSH b[] = { G.hbrUntested, G.hbrShadow, G.hbrScan,
                   G.hbrWrongCyl, G.hbrNoAM, G.hbrBadNew };
    const char * l[] = { "Untested", "Head OK", "Scanning",
                         "Wrong cyl", "No AM", "Other err" };
    n_items = 6;
    for (int i = 0; i < n_items; ++i) { brs[i] = b[i]; labs[i] = l[i]; }
  } else if (G.mode == MODE_CHKFS) {
    HBRUSH b[] = { G.hbrUntested, G.hbrSystem, G.hbrData[0], G.hbrData[1],
                   G.hbrData[2], G.hbrData[3], G.hbrBadNew, G.hbrBadOld };
    const char * l[] = { "Free", "System", "File A", "File B",
                         "File C", "File D", "Lost/X-link", "Bad (FAT)" };
    n_items = 8;
    for (int i = 0; i < n_items; ++i) { brs[i] = b[i]; labs[i] = l[i]; }
  } else {
    HBRUSH b[] = { G.hbrUntested, G.hbrShadow, G.hbrSystem,
                   G.hbrScan, G.hbrBadNew, G.hbrBadOld };
    const char * l[] = { "Unscanned", "Good", "System",
                         "Scanning", "Bad (new)", "Bad (FAT)" };
    n_items = 6;
    for (int i = 0; i < n_items; ++i) { brs[i] = b[i]; labs[i] = l[i]; }
  }

  int x = lx;
  for (int i = 0; i < n_items; ++i) {
    RECT sw = { x, ly, x + 12, ly + 12 };
    FillRect(hdc, &sw, brs[i]);
    FrameRect(hdc, &sw, G.hbrBlack);
    TextOutA(hdc, x + 16, ly - 1, labs[i], lstrlenA(labs[i]));
    SIZE sz;
    GetTextExtentPoint32A(hdc, labs[i], lstrlenA(labs[i]), &sz);
    x += 16 + sz.cx + 10;
  }

  /*  Second + third lines: live progress detail and the status text.  */
  ly += 16;
  DWORD el = (G.running || G.t_start) ? (now_ms() - G.t_start) : 0;
  char els[16];
  fmt_hms(el, els);
  char rems[16] = "--:--:--";
  if (G.scanned > 0 && G.total_sec > 0 && G.running) {
    /*  32-bit-safe ETA: divide before multiplying.  */
    DWORD per = el / G.scanned;
    fmt_hms(per * ((DWORD) G.total_sec - G.scanned), rems);
  }
  int cl = G.current_sec >= 0 ? lba_to_cluster(G.current_sec) : -1;
  char line1[160];
  wsprintfA(line1,
      "Sector %5d / %5d   Cluster %4d   Elapsed %s   Remaining %s",
      G.current_sec, G.total_sec, cl, els, rems);
  TextOutA(hdc, lx, ly, line1, lstrlenA(line1));
  ly += 14;
  TextOutA(hdc, lx, ly, G.status, lstrlenA(G.status));

  SelectObject(hdc, oldf);
}

/*  Scan lifecycle.  */

void start_scan(HWND hWnd) {
  if (G.running) return;

  G.drive = SendMessageA(G.hDriveB, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
  if (SendMessageA(G.hChkfs, BM_GETCHECK, 0, 0) == BST_CHECKED)
    G.mode = MODE_CHKFS;
  else if (SendMessageA(G.hDiagnostic, BM_GETCHECK, 0, 0) == BST_CHECKED)
    G.mode = MODE_DIAGNOSTIC;
  else if (SendMessageA(G.hThorough, BM_GETCHECK, 0, 0) == BST_CHECKED)
    G.mode = MODE_THOROUGH;
  else
    G.mode = MODE_STANDARD;
  G.autofix = SendMessageA(G.hAutoFix, BM_GETCHECK, 0, 0) == BST_CHECKED;
  G.batch   = SendMessageA(G.hBatch,   BM_GETCHECK, 0, 0) == BST_CHECKED;

  /*  Reset per-scan counters here, not in the worker - a BPB-read
      failure must not leave the previous scan's counts behind.  */
  G.running     = 1;
  G.abort_req   = 0;
  G.current_sec = -1;
  G.good_count  = 0;
  G.bad_count   = 0;
  G.scanned     = 0;
  G.bad_n       = 0;
  G.t_start     = 0;
  G.has_fat     = 0;

  if (G.mode == MODE_THOROUGH &&
      MessageBoxA(hWnd,
          "Thorough (write-test) mode writes to every sector of the "
          "disk.\r\nData is restored after each write, but a hard "
          "interruption (power loss, eject) may damage files.\r\n\r\n"
          "Continue?",
          APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) {
    G.running = 0;
    return;
  }

  EnableWindow(G.hStart,   FALSE);
  EnableWindow(G.hStop,    TRUE);
  EnableWindow(G.hRecover, FALSE);
  EnableWindow(G.hFormat,  FALSE);
  SendMessageA(G.hStatus, SB_SETTEXTA, 0,
               (LPARAM) (G.drive ? "Drive: B:" : "Drive: A:"));
  SendMessageA(G.hStatus, SB_SETTEXTA, 2, (LPARAM) "Starting...");

  for (int i = 0; i < MAX_SECTORS; ++i) G.state[i] = ST_UNTESTED;
  InvalidateRect(G.hGrid, NULL, FALSE);
  InvalidateRect(hWnd, NULL, FALSE);

  G.hThread = CreateThread(NULL, 0, worker_proc, NULL, 0, &G.tid);
  SetTimer(hWnd, 1, 100, NULL);   /*  UI refresh  */
  SetTimer(hWnd, 2, 150, NULL);   /*  hatch animation  */
}

void stop_scan(HWND hWnd) {
  (void) hWnd;
  if (G.running) InterlockedExchange(&G.abort_req, 1);
}

void on_done(HWND hWnd) {
  if (G.hThread) { CloseHandle(G.hThread); G.hThread = NULL; }
  KillTimer(hWnd, 1);
  KillTimer(hWnd, 2);
  /*  The animation timer is gone; without this the last cells would
      freeze mid-fade.  */
  trail_clear();

  if (G.closing) return;          /*  about to be destroyed - skip dialogs  */

  EnableWindow(G.hStart,      TRUE);
  EnableWindow(G.hStop,       FALSE);
  EnableWindow(G.hFormat,     TRUE);
  EnableWindow(G.hDefrag,     TRUE);
  EnableWindow(G.hStandard,   TRUE);
  EnableWindow(G.hThorough,   TRUE);
  EnableWindow(G.hDiagnostic, TRUE);
  EnableWindow(G.hChkfs,      TRUE);
  EnableWindow(G.hRecover,    G.bad_n > 0);
  InvalidateRect(hWnd, NULL, FALSE);

  /*  Defrag reports its own outcome from the G.defrag_* fields.  */
  if (G.mode == MODE_DEFRAG) {
    char msg[300];
    if (G.defrag_rc == 0) {
      char els[16];
      fmt_hms(G.t_start ? now_ms() - G.t_start : 0, els);
      wsprintfA(msg,
          "Defragmentation complete.\r\n\r\n"
          "Clusters moved:        %d\r\n"
          "Free clusters zeroed:  %d\r\n"
          "Elapsed:               %s",
          G.defrag_moved, G.defrag_zeroed, els);
      MessageBoxA(hWnd, msg, "Defragmenter", MB_OK | MB_ICONINFORMATION);
    } else if (G.abort_req) {
      MessageBoxA(hWnd,
          "Defrag aborted by user.\r\n\r\n"
          "If the abort happened during the write phase the disk may be\r\n"
          "inconsistent - run Check FS.",
          "Defragmenter", MB_OK | MB_ICONWARNING);
    } else {
      MessageBoxA(hWnd,
          "Defrag failed.\r\n\r\n"
          "The disk may be inconsistent - run Check FS to verify.",
          "Defragmenter", MB_OK | MB_ICONERROR);
    }
    return;
  }

  /*  Only the surface-scan modes get the results dialog; Diagnostic and
      Check-FS show their own summaries.  Skip on a no-disk abort.  */
  if ((G.mode == MODE_STANDARD || G.mode == MODE_THOROUGH) &&
      G.scanned > 0) {
    char els[16];
    fmt_hms(G.t_start ? now_ms() - G.t_start : 0, els);
    char buf[400];
    if (G.bad_count > 0) {
      wsprintfA(buf,
          "Scan complete.\r\n\r\n"
          "%lu of %lu sectors verified.\r\n"
          "%lu bad sector(s) found and %s.\r\nElapsed: %s\r\n\r\n%s",
          (unsigned long) G.good_count, (unsigned long) G.total_sec,
          (unsigned long) G.bad_count,
          G.has_fat ? (G.autofix ? "marked in the FAT" : "logged")
                    : "logged (no FAT - cannot mark)",
          els,
          G.has_fat ? "Click Recover... to copy out affected files."
                    : "No FAT was present; recovery is not available.");
      MessageBoxA(hWnd, buf, APP_NAME, MB_OK | MB_ICONWARNING);
    } else {
      wsprintfA(buf,
          "Scan complete.\r\n\r\n"
          "All %lu sectors verified - no bad sectors found.\r\n"
          "Filesystem: %s\r\nElapsed: %s",
          (unsigned long) G.good_count,
          G.has_fat ? G.fs_type : "RAW (no valid FAT)", els);
      MessageBoxA(hWnd, buf, APP_NAME, MB_OK | MB_ICONINFORMATION);
    }
  }

  /*  Batch mode: prompt for the next disk.  */
  if (G.batch && !G.abort_req &&
      MessageBoxA(hWnd,
          "Insert the next disk into the drive, then click OK to scan "
          "it.\r\n\r\nClick Cancel to end batch mode.",
          "Batch Mode", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK)
    start_scan(hWnd);
}

/*  About box - the app icon plus a one-line copyright.  */

static LRESULT CALLBACK about_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      HWND ok = CreateWindowExA(0, "BUTTON", "OK",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          300 - 75 - 8, 110 - 23 - 8, 75, 23,
          h, (HMENU) (UINT_PTR) ID_ABOUT_OK, G.hInst, NULL);
      SendMessageA(ok, WM_SETFONT, (WPARAM) G.hFont, 0);
      SetFocus(ok);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(h, &ps);
      RECT cli;
      GetClientRect(h, &cli);
      FillRect(hdc, &cli, G.hbrFace);
      /*  LR_SHARED: the system caches the icon, so we must not (and
          need not) destroy it - no per-paint handle leak.  */
      HICON ico = (HICON) LoadImageA(G.hInst, MAKEINTRESOURCEA(1),
          IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED);
      if (ico) DrawIconEx(hdc, 18, 24, ico, 32, 32, 0, NULL, DI_NORMAL);
      HFONT oldf = (HFONT) SelectObject(hdc, G.hFont);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
      const char * l1 = "fdchk " APP_VERSION " - (c) iczelia 2026, gplv3";
      const char * l2 = "https://iczelia.net";
      TextOutA(hdc, 70, 26, l1, lstrlenA(l1));
      TextOutA(hdc, 70, 42, l2, lstrlenA(l2));
      SelectObject(hdc, oldf);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == ID_ABOUT_OK || LOWORD(wp) == IDCANCEL) {
        DestroyWindow(h);
        return 0;
      }
      break;
    case WM_KEYDOWN:
      if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_SPACE) {
        DestroyWindow(h);
        return 0;
      }
      break;
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (G.hMain) {
        EnableWindow(G.hMain, TRUE);
        SetForegroundWindow(G.hMain);
      }
      return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

void do_about(HWND hWnd) {
  static int registered = 0;
  if (!registered) {
    WNDCLASSA wc;
    memzero(&wc, sizeof wc);
    wc.lpfnWndProc   = about_proc;
    wc.hInstance     = G.hInst;
    wc.hIcon         = LoadIconA(G.hInst, MAKEINTRESOURCEA(1));
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = "FdchkAbout";
    RegisterClassA(&wc);
    registered = 1;
  }
  int w = 300, h = 110;
  RECT pr;
  GetWindowRect(hWnd, &pr);
  RECT rc = { 0, 0, w, h };
  AdjustWindowRect(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
  HWND about = CreateWindowExA(WS_EX_DLGMODALFRAME,
      "FdchkAbout", "About fdchk", WS_POPUP | WS_CAPTION | WS_SYSMENU,
      pr.left + ((pr.right - pr.left) - w) / 2,
      pr.top  + ((pr.bottom - pr.top) - h) / 2,
      rc.right - rc.left, rc.bottom - rc.top, hWnd, NULL, G.hInst, NULL);
  if (!about) return;
  EnableWindow(hWnd, FALSE);
  ShowWindow(about, SW_SHOW);
  UpdateWindow(about);
}

/*  In-app log viewer: a list of *.log files on the left, a read-only
    edit control showing the selected file on the right.  */

#define ID_LV_LIST    2001
#define ID_LV_EDIT    2002
#define ID_LV_REFRESH 2003
#define ID_LV_OPENDIR 2004
#define ID_LV_CLOSE   2005

static HWND  g_log_viewer = NULL;
static HWND  g_lv_list    = NULL;
static HWND  g_lv_edit    = NULL;
static HFONT g_lv_font    = NULL;

static void lv_populate(HWND hList) {
  SendMessageA(hList, LB_RESETCONTENT, 0, 0);
  char pattern[MAX_PATH + 8];
  wsprintfA(pattern, "%s\\*.log", G.log_dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM) "(no logs yet)");
    return;
  }
  do {
    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM) fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

static void lv_load_selected(HWND hList, HWND hEdit) {
  int idx = (int) SendMessageA(hList, LB_GETCURSEL, 0, 0);
  if (idx < 0) { SetWindowTextA(hEdit, ""); return; }
  char name[MAX_PATH];
  SendMessageA(hList, LB_GETTEXT, idx, (LPARAM) name);
  if (name[0] == '(') { SetWindowTextA(hEdit, ""); return; }
  char path[MAX_PATH];
  wsprintfA(path, "%s\\%s", G.log_dir, name);
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) {
    SetWindowTextA(hEdit, "(cannot open log file)");
    return;
  }
  DWORD sz = GetFileSize(f, NULL);
  if (sz > 256 * 1024) sz = 256 * 1024;     /*  cap at 256 KB  */
  char * buf = (char *) LocalAlloc(LPTR, sz + 1);
  if (!buf) { CloseHandle(f); return; }
  DWORD cb;
  ReadFile(f, buf, sz, &cb, NULL);
  buf[cb] = 0;
  CloseHandle(f);
  SetWindowTextA(hEdit, buf);
  LocalFree(buf);
}

static void lv_layout(HWND h) {
  if (!g_lv_list || !g_lv_edit) return;
  RECT cli;
  GetClientRect(h, &cli);
  int pad = 8, gap = 6, bh = 24, list_w = 180;
  int top = pad, bottom = cli.bottom - pad - bh - gap;
  if (bottom < top + 60) bottom = top + 60;
  MoveWindow(g_lv_list, pad, top, list_w, bottom - top, TRUE);
  MoveWindow(g_lv_edit, pad + list_w + gap, top,
             cli.right - 2 * pad - list_w - gap, bottom - top, TRUE);
  int by = cli.bottom - pad - bh;
  int bx = cli.right - pad - 80;
  MoveWindow(GetDlgItem(h, ID_LV_CLOSE),   bx, by, 80, bh, TRUE);
  bx -= 100 + gap;
  MoveWindow(GetDlgItem(h, ID_LV_OPENDIR), bx, by, 100, bh, TRUE);
  bx -= 80 + gap;
  MoveWindow(GetDlgItem(h, ID_LV_REFRESH), bx, by, 80, bh, TRUE);
}

static LRESULT CALLBACK lv_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      g_lv_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          LBS_NOTIFY | LBS_HASSTRINGS,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_LIST, G.hInst, NULL);
      g_lv_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
          ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_EDIT, G.hInst, NULL);
      SendMessageA(g_lv_edit, EM_LIMITTEXT, 0x80000, 0);
      /*  A fixed-pitch font for log readability; freed in WM_DESTROY.  */
      g_lv_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
          ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
      SendMessageA(g_lv_edit, WM_SETFONT, (WPARAM) g_lv_font, 0);
      SendMessageA(g_lv_list, WM_SETFONT, (WPARAM) G.hFont, 0);
      HWND b1 = CreateWindowExA(0, "BUTTON", "&Refresh",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_REFRESH, G.hInst, NULL);
      HWND b2 = CreateWindowExA(0, "BUTTON", "Open &Folder",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_OPENDIR, G.hInst, NULL);
      HWND b3 = CreateWindowExA(0, "BUTTON", "&Close",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_CLOSE, G.hInst, NULL);
      SendMessageA(b1, WM_SETFONT, (WPARAM) G.hFont, 0);
      SendMessageA(b2, WM_SETFONT, (WPARAM) G.hFont, 0);
      SendMessageA(b3, WM_SETFONT, (WPARAM) G.hFont, 0);
      lv_populate(g_lv_list);
      if (SendMessageA(g_lv_list, LB_GETCOUNT, 0, 0) > 0) {
        SendMessageA(g_lv_list, LB_SETCURSEL, 0, 0);
        lv_load_selected(g_lv_list, g_lv_edit);
      }
      return 0;
    }
    case WM_SIZE:
      lv_layout(h);
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(wp), code = HIWORD(wp);
      if (id == ID_LV_LIST && code == LBN_SELCHANGE) {
        lv_load_selected(g_lv_list, g_lv_edit);
      } else if (id == ID_LV_REFRESH) {
        lv_populate(g_lv_list);
        SendMessageA(g_lv_list, LB_SETCURSEL, 0, 0);
        lv_load_selected(g_lv_list, g_lv_edit);
      } else if (id == ID_LV_OPENDIR) {
        char cmd[MAX_PATH + 32];
        wsprintfA(cmd, "explorer.exe \"%s\"", G.log_dir);
        WinExec(cmd, SW_SHOWNORMAL);
      } else if (id == ID_LV_CLOSE || id == IDCANCEL) {
        DestroyWindow(h);
      }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (g_lv_font) { DeleteObject(g_lv_font); g_lv_font = NULL; }
      g_log_viewer = NULL;
      g_lv_list = g_lv_edit = NULL;
      return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

void do_logs(HWND parent) {
  static int registered = 0;
  if (!registered) {
    WNDCLASSA wc;
    memzero(&wc, sizeof wc);
    wc.lpfnWndProc   = lv_proc;
    wc.hInstance     = G.hInst;
    wc.hIcon         = LoadIconA(G.hInst, MAKEINTRESOURCEA(1));
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = "FdchkLogs";
    RegisterClassA(&wc);
    registered = 1;
  }
  if (g_log_viewer && IsWindow(g_log_viewer)) {
    SetForegroundWindow(g_log_viewer);
    return;
  }
  int w = 640, h = 420;
  RECT pr;
  GetWindowRect(parent, &pr);
  g_log_viewer = CreateWindowExA(0, "FdchkLogs", "fdchk - Log Viewer",
      WS_OVERLAPPEDWINDOW,
      pr.left + ((pr.right - pr.left) - w) / 2,
      pr.top  + ((pr.bottom - pr.top) - h) / 2,
      w, h, parent, NULL, G.hInst, NULL);
  if (g_log_viewer) {
    ShowWindow(g_log_viewer, SW_SHOW);
    UpdateWindow(g_log_viewer);
  }
}

/*  Command flows: Format, Defrag, Recover.  */

void do_format_flow(HWND hWnd) {
  int drive = SendMessageA(G.hDriveB, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? 1 : 0;
  char msg[400];
  wsprintfA(msg,
      "Format drive %c: as FAT12?\r\n\r\n"
      "ALL DATA on the disk will be erased.\r\n\r\n"
      "Click YES    for a Quick format (boot/FAT/root only).\r\n"
      "Click NO     for a Full  format (also fill the data area).\r\n"
      "Click CANCEL to abort.",
      'A' + drive);
  int ans = MessageBoxA(hWnd, msg, "Format Floppy",
      MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
  if (ans == IDCANCEL) return;
  int full = (ans == IDNO);

  DiskHandle dh;
  if (!disk_open(&dh, drive)) {
    MessageBoxA(hWnd, "Cannot open drive.", APP_NAME, MB_OK | MB_ICONERROR);
    return;
  }
  if (probe_disk_present(&dh) == 0) {
    MessageBoxA(hWnd, "No disk in the drive.", APP_NAME,
                MB_OK | MB_ICONWARNING);
    disk_close(&dh);
    return;
  }
  /*  Lock with the level 2 -> 1 -> 0 ladder: VWIN32 is fussy about who
      else holds the volume open (Explorer views, DIR prompts).  */
  int lock_rc = -1;
  for (int lvl = 2; lvl >= 0; --lvl) {
    lock_rc = disk_lock(&dh, lvl);
    if (lock_rc == 0) break;
  }
  if (lock_rc < 0) {
    char err[200];
    wsprintfA(err,
        "Cannot lock drive %c: for format (%s).\r\n\r\n"
        "Close anything that might be using the disk and try again.",
        'A' + drive, dos_err_str(lock_rc));
    MessageBoxA(hWnd, err, APP_NAME, MB_OK | MB_ICONERROR);
    disk_close(&dh);
    return;
  }

  int rc = format_disk(&dh, "FDCHK", full);
  disk_unlock(&dh);
  disk_close(&dh);

  if (rc == 0) {
    char done[200];
    wsprintfA(done,
        "Drive %c: formatted successfully.\r\n\r\n"
        "Style: %s\r\nFilesystem: FAT12\r\nLabel: FDCHK",
        'A' + drive, full ? "Full (data area zeroed)" : "Quick");
    MessageBoxA(hWnd, done, "Format", MB_OK | MB_ICONINFORMATION);
    SendMessageA(G.hStatus, SB_SETTEXTA, 1, (LPARAM) "FAT12");
  } else {
    char err[200];
    wsprintfA(err, "Format failed: %s", dos_err_str(rc < 0 ? rc : -rc));
    MessageBoxA(hWnd, err, "Format", MB_OK | MB_ICONERROR);
  }
}

void do_defrag_flow(HWND hWnd) {
  if (G.running) return;
  int drive = (G.has_b && !G.has_a) ? 1
            : (SendMessageA(G.hDriveB, BM_GETCHECK, 0, 0) == BST_CHECKED
               ? 1 : 0);
  char msg[400];
  wsprintfA(msg,
      "Defragment drive %c:?\r\n\r\n"
      "Every file and subdirectory is repacked into contiguous low\r\n"
      "clusters, alphabetical at each directory level, depth-first.\r\n"
      "Bad clusters are preserved; all free clusters are zeroed.\r\n\r\n"
      "Power loss midway may corrupt the disk - be sure before you go.",
      'A' + drive);
  if (MessageBoxA(hWnd, msg, "Defragment Floppy",
      MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
    return;

  /*  All disk work runs on a worker thread so the UI stays responsive
      and Stop keeps working; mirrors start_scan's setup.  */
  G.drive       = drive;
  G.mode        = MODE_DEFRAG;
  G.abort_req   = 0;
  G.running     = 1;
  G.closing     = 0;
  G.current_sec = -1;
  G.good_count  = 0;
  G.bad_count   = 0;
  G.scanned     = 0;
  G.bad_n       = 0;
  G.t_start     = now_ms();
  G.has_fat     = 0;

  EnableWindow(G.hStart,      FALSE);
  EnableWindow(G.hStop,       TRUE);
  EnableWindow(G.hRecover,    FALSE);
  EnableWindow(G.hFormat,     FALSE);
  EnableWindow(G.hDefrag,     FALSE);
  EnableWindow(G.hStandard,   FALSE);
  EnableWindow(G.hThorough,   FALSE);
  EnableWindow(G.hDiagnostic, FALSE);
  EnableWindow(G.hChkfs,      FALSE);

  for (int i = 0; i < MAX_SECTORS; ++i) G.state[i] = ST_UNTESTED;
  mark_system_sectors();
  trail_clear();
  InvalidateRect(G.hGrid, NULL, FALSE);

  SendMessageA(G.hStatus, SB_SETTEXTA, 0,
               (LPARAM) (drive ? "Drive: B:" : "Drive: A:"));
  SendMessageA(G.hStatus, SB_SETTEXTA, 2, (LPARAM) "Defragmenting...");

  SetTimer(hWnd, 1, 100, NULL);
  SetTimer(hWnd, 2, 150, NULL);
  G.hThread = CreateThread(NULL, 0, defrag_thread_proc, NULL, 0, &G.tid);
}

void do_recover_flow(HWND hWnd) {
  if (G.bad_n == 0) {
    MessageBoxA(hWnd, "No bad sectors to recover from.", APP_NAME,
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  char folder[MAX_PATH];
  GetCurrentDirectoryA(MAX_PATH, folder);
  char buf[MAX_PATH + 200];
  wsprintfA(buf,
      "Recovery will copy readable files from drive %c: into:\r\n\r\n"
      "%s\r\n\r\n"
      "Files containing bad sectors are copied with zero bytes where\r\n"
      "the bad sectors were.  A log is saved to fdchk-recovery.log.\r\n\r\n"
      "Continue?",
      'A' + G.drive, folder);
  if (MessageBoxA(hWnd, buf, "Recover Files",
                  MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
    do_recovery(hWnd, folder);
}
