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

/*  Native Windows user interface.  */

/*  Draw one raised or sunken edge.  */
static void draw_3d_rect(HDC hdc, const RECT * r, BOOL sunken) {
  HPEN tl = sunken ? G.shadow_pen : G.highlight_pen;
  HPEN br = sunken ? G.highlight_pen : G.shadow_pen;
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

/*  Rotate an 8x8 scan hatch by phase.  */
static HBITMAP make_hatch_bitmap(int phase) {
  BITMAPINFO bi;  BYTE bits[8 * 8 * 4];  memzero(&bi, sizeof bi);
  int i, j;
  bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth       = 8;
  bi.bmiHeader.biHeight      = 8;
  bi.bmiHeader.biPlanes      = 1;
  bi.bmiHeader.biBitCount    = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  Fi(8, Fj(8,
      DWORD c = (((j + i + phase) >> 1) & 1) ? 0x00FFFF00 : 0x00808000;
      BYTE * p = bits + (i * 8 + j) * 4;
      p[0] = (BYTE) c;
      p[1] = (BYTE) (c >> 8);
      p[2] = (BYTE) (c >> 16);
      p[3] = 0;
    ));
  HDC sdc = GetDC(NULL);
  HBITMAP bmp = CreateDIBitmap(sdc, &bi.bmiHeader, CBM_INIT, bits,
                               &bi, DIB_RGB_COLORS);
  ReleaseDC(NULL, sdc);
  return bmp;
}

void update_scan_brush(void) {
  if (G.scan_brush) DeleteObject(G.scan_brush);
  if (G.scan_bitmap) DeleteObject(G.scan_bitmap);
  G.scan_bitmap = make_hatch_bitmap(G.scan_anim_phase);
  G.scan_brush = CreatePatternBrush(G.scan_bitmap);
}

/*  Owner-drawn sector grid.  */
#define CELL_SIZE 9
#define CELL_GAP  1
#define GRID_PAD  4

static struct { int cols, rows, cell; } grid_geom;

void grid_calc(HWND h) {
  RECT r;
  GetClientRect(h, &r);
  int w  = r.right  - r.left - GRID_PAD * 2;
  int hh = r.bottom - r.top  - GRID_PAD * 2;
  int total = G.total_sec > 0 ? G.total_sec : MAX_SECTORS;
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
    case ST_GOOD:      return G.shadow_brush;
    case ST_SYSTEM:    return G.system_brush;
    case ST_SCANNING:  return G.scan_brush;
    case ST_WRITING:   return G.write_brush;
    case ST_VERIFY:    return G.verify_brush;
    case ST_BAD_NEW:   return G.bad_new_brush;
    case ST_BAD_OLD:   return G.bad_old_brush;
    case ST_WRONG_CYL: return G.wrong_cyl_brush;
    case ST_NO_AM:     return G.no_am_brush;
    case ST_DATA_0: case ST_DATA_1:
    case ST_DATA_2: case ST_DATA_3:
      return G.data_brush[s - ST_DATA_0];
    case ST_TRAIL_0: case ST_TRAIL_1:
    case ST_TRAIL_2: case ST_TRAIL_3:
      return G.trail_brush[s - ST_TRAIL_0];
    default:           return G.untested_brush;
  }
}

static const char * state_name(BYTE s) {
  switch (s) {
    case ST_UNTESTED:  return "not tested";
    case ST_SYSTEM:    return "system area";
    case ST_GOOD:      return "good";
    case ST_SCANNING:  return "being read";
    case ST_WRITING:   return "being written";
    case ST_VERIFY:    return "being checked";
    case ST_BAD_NEW:   return "bad";
    case ST_BAD_OLD:   return "marked bad in the FAT";
    case ST_WRONG_CYL: return "wrong cylinder";
    case ST_NO_AM:     return "no address mark";
    case ST_DATA_0: case ST_DATA_1:
    case ST_DATA_2: case ST_DATA_3:
      return "file data";
    case ST_TRAIL_0: case ST_TRAIL_1:
    case ST_TRAIL_2: case ST_TRAIL_3:
      return "good";
    default:           return "unknown";
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

/*  Repaint one cell.  */
void grid_invalidate_cell(int idx) {
  if (!G.grid) return;
  RECT cr;
  grid_cell_rect(idx, &cr);
  InvalidateRect(G.grid, &cr, FALSE);
}

/*  Replace the oldest trail cell.  */
static void trail_push(int sec) {
  int i, slot = 0, max_age = G.trail_age[0];
  for (i = 1; i < 4; i++) {
    if (G.trail_age[i] < 0) { slot = i; break; }
    if (G.trail_age[i] > max_age) { max_age = G.trail_age[i]; slot = i; }
  }
  int prev = G.trail_sec[slot];
  G.trail_sec[slot] = sec;
  G.trail_age[slot] = 0;
  if (prev >= 0) grid_invalidate_cell(prev);
  grid_invalidate_cell(sec);
}

void trail_tick(void) {
  int i;
  Fi(4,
    if (G.trail_age[i] < 0) continue;
    int sec = G.trail_sec[i];
    G.trail_age[i]++;
    if (G.trail_age[i] >= 4) {
      G.trail_sec[i] = -1;
      G.trail_age[i] = -1;
    }
    if (sec >= 0) grid_invalidate_cell(sec);
  );
}

/*  Return trail age or -1.  */
static int trail_age_of(int sec) {
  int i;
  Fi(4, if (G.trail_sec[i] == sec && G.trail_age[i] >= 0)
          return G.trail_age[i]);
  return -1;
}

void trail_clear(void) {
  int i;
  Fi(4,
    int sec = G.trail_sec[i];
    G.trail_sec[i] = -1;
    G.trail_age[i] = -1;
    if (sec >= 0) grid_invalidate_cell(sec);
  );
}

static void grid_paint(HWND h, HDC hdc, const RECT * clip) {
  RECT cli;
  int i;
  GetClientRect(h, &cli);
  draw_3d_rect(hdc, &cli, TRUE);

  RECT inner = { cli.left + 1, cli.top + 1, cli.right - 1, cli.bottom - 1 };
  FillRect(hdc, &inner, G.black_brush);
  if (grid_geom.cols == 0) return;

  int total = G.total_sec ? G.total_sec : MAX_SECTORS;
  Fi(total,
    RECT cr;
    grid_cell_rect(i, &cr);
    if (cr.right < clip->left || cr.left > clip->right ||
        cr.bottom < clip->top || cr.top > clip->bottom)
      continue;
    BYTE st = G.state[i];
    /*  Tint new good sectors.  */
    int age = (st == ST_GOOD) ? trail_age_of(i) : -1;
    FillRect(hdc, &cr,
             (age >= 0 && age < 4) ? G.trail_brush[age] : brush_for_state(st));
  );
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
      int x = LOWORD(lp), y = HIWORD(lp);
      if (grid_geom.cell > 0) {
        int col = (x - GRID_PAD) / grid_geom.cell;
        int row = (y - GRID_PAD) / grid_geom.cell;
        if (col >= 0 && col < grid_geom.cols && row >= 0) {
          int idx = row * grid_geom.cols + col;
          if (idx >= 0 && idx < G.total_sec) {
            int cl = lba_to_cluster((DWORD) idx);
            char buf[80];
            if (cl >= 0)
              wsprintfA(buf, "Sector %d, cluster %d: %s",
                        idx, cl, state_name(G.state[idx]));
            else
              wsprintfA(buf, "Sector %d, system area: %s",
                        idx, state_name(G.state[idx]));
            SendMessageA(G.statusbar, SB_SETTEXTA, 2, (LPARAM) buf);
          }
        }
      }
      return 0;
    }
  }
  return DefWindowProcA(h, m, wp, lp);
}

void ui_set_state(int sec, BYTE st) {
  if (!G.state || sec < 0 || sec >= MAX_SECTORS) return;
  BYTE was = G.state[sec];
  /*  Show a trail on new good data sectors.  */
  if (st == ST_GOOD && was != ST_GOOD &&
      G.data_start_sec > 0 && sec >= G.data_start_sec)
    trail_push(sec);
  G.state[sec] = st;
  grid_invalidate_cell(sec);
}

void ui_status(const char * s) {
  lstrcpynA(G.status, s, sizeof G.status);
  if (G.statusbar)
    SendMessageA(G.statusbar, SB_SETTEXTA, 2, (LPARAM) G.status);
}

void ui_drive_labels(void) {
  char t[64];
  if (G.drive_a) {
    wsprintfA(t, "A:  (%s)", drive_type_str(G.drive_type[0]));
    SetWindowTextA(G.drive_a, t);
  }
  if (G.drive_b) {
    wsprintfA(t, "B:  (%s)", drive_type_str(G.drive_type[1]));
    SetWindowTextA(G.drive_b, t);
  }
}

void ui_progress(DWORD done, DWORD total) {
  if (G.main)
    PostMessageA(G.main, WM_APP_PROGRESS, (WPARAM) done, (LPARAM) total);
}

int ui_prompt(const char * msg, const char * caption, UINT flags) {
  return MessageBoxA(G.main, msg, caption, flags);
}

void ui_create(HWND window) {
  HINSTANCE hi = G.instance;
  size_t i;

  G.drive_group = CreateWindowExA(0, "BUTTON", "Drive",
      WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, window, NULL, hi, NULL);
  G.drive_a = CreateWindowExA(0, "BUTTON", "A:",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_DRIVE_A, hi, NULL);
  G.drive_b = CreateWindowExA(0, "BUTTON", "B:",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_DRIVE_B, hi, NULL);
  SendMessageA(G.drive_a, BM_SETCHECK, BST_CHECKED, 0);
  ui_drive_labels();

  G.test_group = CreateWindowExA(0, "BUTTON", "Test",
      WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, window, NULL, hi, NULL);
  G.standard = CreateWindowExA(0, "BUTTON",
      "&Standard - read every sector",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_STANDARD, hi, NULL);
  G.thorough = CreateWindowExA(0, "BUTTON",
      "&Thorough - write, check and restore every sector",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_THOROUGH, hi, NULL);
  G.diagnostic = CreateWindowExA(0, "BUTTON",
      "&Diagnostic - test seeks, alignment and track IDs",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_DIAGNOSTIC, hi, NULL);
  G.chkfs = CreateWindowExA(0, "BUTTON",
      "Check &filesystem - find FAT12 errors",
      WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_CHKFS, hi, NULL);
  SendMessageA(G.thorough, BM_SETCHECK, BST_CHECKED, 0);

  G.auto_fix_button = CreateWindowExA(0, "BUTTON",
      "Automatically &mark new bad clusters",
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_AUTOFIX, hi, NULL);
  SendMessageA(G.auto_fix_button, BM_SETCHECK, BST_CHECKED, 0);

  G.batch_button = CreateWindowExA(0, "BUTTON",
      "&Batch mode - ask for another disk after each scan",
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_BATCH, hi, NULL);

  G.grid = CreateWindowExA(0, "FdchkGrid", NULL, WS_CHILD | WS_VISIBLE,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_GRID, hi, NULL);

  G.progress = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
      WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
      (HMENU) (UINT_PTR) ID_PROGRESS, hi, NULL);
  SendMessageA(G.progress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));

  G.start_button = CreateWindowExA(0, "BUTTON", "St&art",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_START, hi, NULL);
  G.stop_button = CreateWindowExA(0, "BUTTON", "Sto&p",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_STOP, hi, NULL);
  G.recover_button = CreateWindowExA(0, "BUTTON", "&Recover...",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_RECOVER, hi, NULL);
  G.format_button = CreateWindowExA(0, "BUTTON", "F&ormat...",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_FORMAT, hi, NULL);
  G.defrag_button = CreateWindowExA(0, "BUTTON", "Defra&g...",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_DEFRAG, hi, NULL);
  G.logs_button = CreateWindowExA(0, "BUTTON", "&Logs...",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_LOGS, hi, NULL);
  G.about_button = CreateWindowExA(0, "BUTTON", "Abo&ut...",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_ABOUT, hi, NULL);
  G.close_button = CreateWindowExA(0, "BUTTON", "&Close",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_CLOSE, hi, NULL);

  /*  Set one font on every control.  */
  HWND children[] = {
    G.drive_group, G.drive_a, G.drive_b,
    G.test_group, G.standard, G.thorough, G.diagnostic, G.chkfs,
    G.auto_fix_button, G.batch_button,
    G.start_button, G.stop_button, G.recover_button, G.format_button,
    G.defrag_button,
    G.logs_button, G.about_button, G.close_button
  };
  Fi(sizeof children / sizeof children[0],
    SendMessageA(children[i], WM_SETFONT, (WPARAM) G.font, 0));

  /*  Hide absent drive letters.  */
  if (!G.has_a) ShowWindow(G.drive_a, SW_HIDE);
  if (!G.has_b) ShowWindow(G.drive_b, SW_HIDE);
  if (G.has_b && !G.has_a)
    SendMessageA(G.drive_b, BM_SETCHECK, BST_CHECKED, 0);
  if (!(G.has_a && G.has_b))
    ShowWindow(G.drive_group, SW_HIDE);

  G.statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "Ready.",
      WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
      0, 0, 0, 0, window, (HMENU) (UINT_PTR) ID_STATUSBAR, hi, NULL);
  SendMessageA(G.statusbar, WM_SETFONT, (WPARAM) G.font, 0);
  int parts[3] = { 80, 180, -1 };
  SendMessageA(G.statusbar, SB_SETPARTS, 3, (LPARAM) parts);
  SendMessageA(G.statusbar, SB_SETTEXTA, 0, (LPARAM) "Drive: ?");
  SendMessageA(G.statusbar, SB_SETTEXTA, 1, (LPARAM) "FAT?");
  SendMessageA(G.statusbar, SB_SETTEXTA, 2, (LPARAM) "Ready.");
}

void ui_layout(HWND window) {
  if (!G.main || !G.grid) return;
  RECT cli;
  GetClientRect(window, &cli);

  /*  Remove the self-sized status bar from the work area.  */
  if (G.statusbar) {
    SendMessageA(G.statusbar, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(G.statusbar, &sb);
    POINT pt = { sb.left, sb.top };
    ScreenToClient(window, &pt);
    cli.bottom = pt.y;
    int parts[3] = { 80, 180, -1 };
    SendMessageA(G.statusbar, SB_SETPARTS, 3, (LPARAM) parts);
  }

  const int pad = 8, gap = 6, radio_h = 18, check_h = 18, btn_h = 24;
  int x = cli.left + pad;
  int w = cli.right - cli.left - pad * 2;
  int y = cli.top + pad;

  if (IsWindowVisible(G.drive_group)) {
    int h = 18 + radio_h + 12;
    MoveWindow(G.drive_group, x, y, w, h, TRUE);
    int rw = (w - 36) / 2;
    MoveWindow(G.drive_a, x + 12,      y + 16, rw, radio_h, TRUE);
    MoveWindow(G.drive_b, x + 24 + rw, y + 16, rw, radio_h, TRUE);
    y += h + gap;
  }

  {
    int h = 18 + 4 * radio_h + 6 + check_h + 4 + check_h + 8;
    MoveWindow(G.test_group, x, y, w, h, TRUE);
    int tw = w - 24, ty = y + 16;
    MoveWindow(G.standard,   x + 12, ty,                 tw, radio_h, TRUE);
    MoveWindow(G.thorough,   x + 12, ty + radio_h,       tw, radio_h, TRUE);
    MoveWindow(G.diagnostic, x + 12, ty + 2 * radio_h,   tw, radio_h, TRUE);
    MoveWindow(G.chkfs,      x + 12, ty + 3 * radio_h,   tw, radio_h, TRUE);
    ty += 4 * radio_h + 6;
    MoveWindow(G.auto_fix_button, x + 12, ty, tw, check_h, TRUE);
    MoveWindow(G.batch_button, x + 12, ty + check_h + 4, tw, check_h, TRUE);
    y += h + gap;
  }

  int by = cli.bottom - pad - btn_h;
  HWND btns[] = { G.start_button, G.stop_button, G.recover_button, G.format_button,
                  G.defrag_button, G.logs_button, G.about_button, G.close_button };
  int n_btn = (int) (sizeof btns / sizeof btns[0]);
  int btn_w = (w - gap * (n_btn - 1)) / n_btn;
  int i, bx;
  if (btn_w < 60) btn_w = 60;
  for (i = 0, bx = x; i < n_btn; i++, bx += btn_w + gap)
    MoveWindow(btns[i], bx, by, btn_w, btn_h, TRUE);

  int plinth_h = 6 + 14 + 4 + 14 + 4 + 14 + 6;
  int plinth_y = by - gap - plinth_h;
  SetRect(&G.plinth, x, plinth_y, x + w, plinth_y + plinth_h);

  int prog_h = 18, prog_y = plinth_y - gap - prog_h;
  MoveWindow(G.progress, x, prog_y, w, prog_h, TRUE);

  int grid_h = prog_y - gap - y;
  if (grid_h < 60) grid_h = 60;
  MoveWindow(G.grid, x, y, w, grid_h, TRUE);

  InvalidateRect(window, NULL, TRUE);
}

/*  Draw the legend, progress, and status.  */
void ui_paint_legend(HDC hdc) {
  RECT plinth = G.plinth;
  int i;
  if (plinth.right <= plinth.left || plinth.bottom <= plinth.top) return;
  DrawEdge(hdc, &plinth, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
  FillRect(hdc, &plinth, G.face_brush);
  int lx = plinth.left + 6, ly = plinth.top + 6;

  HFONT oldf = (HFONT) SelectObject(hdc, G.font);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

  HBRUSH brs[8];
  const char * labs[8];
  int n_items;
  if (G.mode == MODE_DIAGNOSTIC) {
    HBRUSH b[] = { G.untested_brush, G.shadow_brush, G.scan_brush,
                   G.wrong_cyl_brush, G.no_am_brush, G.bad_new_brush };
    const char * l[] = { "Untested", "Good", "Scanning",
                         "Wrong cylinder", "No address mark", "Read error" };
    n_items = 6;
    Fi(n_items, brs[i] = b[i];  labs[i] = l[i]);
  } else if (G.mode == MODE_CHKFS) {
    HBRUSH b[] = { G.untested_brush, G.system_brush, G.data_brush[0], G.data_brush[1],
                   G.data_brush[2], G.data_brush[3], G.bad_new_brush, G.bad_old_brush };
    const char * l[] = { "Free", "System", "File A", "File B",
                         "File C", "File D", "Lost/X-link", "Bad (FAT)" };
    n_items = 8;
    Fi(n_items, brs[i] = b[i];  labs[i] = l[i]);
  } else {
    HBRUSH b[] = { G.untested_brush, G.shadow_brush, G.system_brush,
                   G.scan_brush, G.bad_new_brush, G.bad_old_brush };
    const char * l[] = { "Unscanned", "Good", "System",
                         "Scanning", "Bad (new)", "Bad (FAT)" };
    n_items = 6;
    Fi(n_items, brs[i] = b[i];  labs[i] = l[i]);
  }

  int x = lx;
  Fi(n_items,
    RECT sw = { x, ly, x + 12, ly + 12 };
    FillRect(hdc, &sw, brs[i]);
    FrameRect(hdc, &sw, G.black_brush);
    TextOutA(hdc, x + 16, ly - 1, labs[i], lstrlenA(labs[i]));
    SIZE sz;
    GetTextExtentPoint32A(hdc, labs[i], lstrlenA(labs[i]), &sz);
    x += 16 + sz.cx + 10;
  );

  ly += 16;
  DWORD el = (G.running || G.t_start) ? (now_ms() - G.t_start) : 0;
  char els[16];
  fmt_hms(el, els);
  char rems[16] = "--:--:--";
  if (G.scanned > 0 && G.total_sec > 0 && G.running) {
    /*  Divide first to keep the ETA in 32 bits.  */
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

void ui_start_scan(HWND window) {
  int i;
  if (G.running) return;

  G.drive = SendMessageA(G.drive_b, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
  if (SendMessageA(G.chkfs, BM_GETCHECK, 0, 0) == BST_CHECKED)
    G.mode = MODE_CHKFS;
  else if (SendMessageA(G.diagnostic, BM_GETCHECK, 0, 0) == BST_CHECKED)
    G.mode = MODE_DIAGNOSTIC;
  else if (SendMessageA(G.thorough, BM_GETCHECK, 0, 0) == BST_CHECKED)
    G.mode = MODE_THOROUGH;
  else
    G.mode = MODE_STANDARD;
  G.auto_fix = SendMessageA(G.auto_fix_button, BM_GETCHECK, 0, 0) == BST_CHECKED;
  G.batch = SendMessageA(G.batch_button, BM_GETCHECK, 0, 0) == BST_CHECKED;

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
      MessageBoxA(window,
          "The thorough surface test writes to every sector, then writes the "
          "original data back. A power failure or disk ejection may damage "
          "files.\r\n\r\n"
          "Continue?",
          APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) {
    G.running = 0;
    return;
  }

  EnableWindow(G.start_button,   FALSE);
  EnableWindow(G.stop_button,    TRUE);
  EnableWindow(G.recover_button, FALSE);
  EnableWindow(G.format_button,  FALSE);
  SendMessageA(G.statusbar, SB_SETTEXTA, 0,
               (LPARAM) (G.drive ? "Drive: B:" : "Drive: A:"));
  SendMessageA(G.statusbar, SB_SETTEXTA, 2, (LPARAM) "Starting...");

  Fi(MAX_SECTORS, G.state[i] = ST_UNTESTED);
  InvalidateRect(G.grid, NULL, FALSE);
  InvalidateRect(window, NULL, FALSE);

  G.thread = CreateThread(NULL, 0, scan_thread, NULL, 0, &G.tid);
  SetTimer(window, 1, 100, NULL);
  SetTimer(window, 2, 150, NULL);
  if (!G.thread) { G.running = 0; ui_worker_done(window); }
}

void ui_stop_scan(HWND window) {
  (void) window;
  if (G.running) InterlockedExchange(&G.abort_req, 1);
}

void ui_worker_done(HWND window) {
  if (G.thread) { CloseHandle(G.thread); G.thread = NULL; }
  KillTimer(window, 1);
  KillTimer(window, 2);
  /*  Remove the scan trail.  */
  trail_clear();

  if (G.closing) return;

  EnableWindow(G.start_button,      TRUE);
  EnableWindow(G.stop_button,       FALSE);
  EnableWindow(G.format_button,     TRUE);
  EnableWindow(G.defrag_button,     TRUE);
  EnableWindow(G.standard,   TRUE);
  EnableWindow(G.thorough,   TRUE);
  EnableWindow(G.diagnostic, TRUE);
  EnableWindow(G.chkfs,      TRUE);
  EnableWindow(G.recover_button,    G.bad_n > 0);
  InvalidateRect(window, NULL, FALSE);

  if (G.mode == MODE_FORMAT) {
    char msg[640];
    const floppy_geom * g = G.fmt_geom;
    if (G.fmt_rc == 0) {
      char els[16];
      fmt_hms(G.t_start ? now_ms() - G.t_start : 0, els);
      wsprintfA(msg,
          "Drive %c: formatted.\r\n\r\n"
          "Capacity:    %s\r\n"
          "Type:        %s\r\n"
          "Bad tracks:  %d%s\r\n"
          "Elapsed:     %s%s%s",
          'A' + G.drive, g ? g->name : "1.44 MB",
          G.fmt_style == FMT_FULL
              ? (G.fmt_low_level_ok ? "Full; tracks formatted and checked"
                                   : "Full; sectors written because track "
                                     "format failed")
              : "Quick; boot sector, FATs and root directory written",
          G.fmt_bad_tracks,
          G.fmt_bad_tracks ? " (clusters marked bad)" : "",
          els,
          G.fmt_msg[0] ? "\r\n\r\n" : "", G.fmt_msg);
      if (G.fmt_sys_bad)
        lstrcpynA(msg + lstrlenA(msg),
            "\r\n\r\nThe boot sector, a FAT, or the root directory is on a "
            "bad track. Do not store data on this disk.",
            (int) sizeof msg - lstrlenA(msg));
      MessageBoxA(window, msg, "Format",
                  MB_OK | ((G.fmt_bad_tracks || G.fmt_sys_bad || G.fmt_msg[0])
                           ? MB_ICONWARNING : MB_ICONINFORMATION));
      if (!G.fmt_sys_bad)
        SendMessageA(G.statusbar, SB_SETTEXTA, 1, (LPARAM) "FAT12");
    } else if (G.fmt_rc == 1) {
      MessageBoxA(window,
          "Formatting stopped after writing began. Format the disk again "
          "before using it.",
          "Format", MB_OK | MB_ICONWARNING);
    } else {
      wsprintfA(msg, "Format failed: %s",
                G.fmt_msg[0] ? G.fmt_msg : "unknown error");
      MessageBoxA(window, msg, "Format", MB_OK | MB_ICONERROR);
    }
    return;
  }

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
      MessageBoxA(window, msg, "Defragmentation", MB_OK | MB_ICONINFORMATION);
    } else if (G.defrag_rc == 1) {
      MessageBoxA(window,
          "Defragmentation stopped before fdchk wrote any data.",
          "Defragmentation", MB_OK | MB_ICONWARNING);
    } else {
      MessageBoxA(window,
          "Defragmentation failed while writing. The disk may be "
          "corrupt.\r\n\r\nRun a filesystem check.",
          "Defragmentation", MB_OK | MB_ICONERROR);
    }
    return;
  }

  /*  Diagnostic and filesystem tests report their own results.  */
  if ((G.mode == MODE_STANDARD || G.mode == MODE_THOROUGH) &&
      G.scanned > 0) {
    char els[16];
    fmt_hms(G.t_start ? now_ms() - G.t_start : 0, els);
    char buf[400];
    if (G.bad_count > 0) {
      wsprintfA(buf,
          "Scan complete.\r\n\r\n"
          "Checked:     %lu of %lu sectors\r\n"
          "Bad sectors: %lu; %s\r\n"
          "Elapsed:     %s\r\n\r\n%s",
          (unsigned long) G.good_count, (unsigned long) G.total_sec,
          (unsigned long) G.bad_count,
          G.has_fat ? (G.auto_fix ? "written to the FAT" : "not written")
                    : "not written; no valid FAT",
          els,
          G.has_fat ? "Use file recovery to copy readable data."
                    : "File recovery needs a valid FAT.");
      MessageBoxA(window, buf, APP_NAME, MB_OK | MB_ICONWARNING);
    } else {
      wsprintfA(buf,
          "Scan complete.\r\n\r\n"
          "Checked:     %lu sectors\r\n"
          "Bad sectors: 0\r\n"
          "Filesystem:  %s\r\n"
          "Elapsed:     %s",
          (unsigned long) G.good_count,
          G.has_fat ? G.fs_type : "RAW (no valid FAT)", els);
      MessageBoxA(window, buf, APP_NAME, MB_OK | MB_ICONINFORMATION);
    }
  }

  if (G.batch && !G.abort_req &&
      MessageBoxA(window,
          "Insert the next disk and click OK. Cancel ends batch mode.",
          "Batch mode", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK)
    ui_start_scan(window);
}

static LRESULT CALLBACK about_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      HWND ok = CreateWindowExA(0, "BUTTON", "OK",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          300 - 75 - 8, 110 - 23 - 8, 75, 23,
          h, (HMENU) (UINT_PTR) ID_ABOUT_OK, G.instance, NULL);
      SendMessageA(ok, WM_SETFONT, (WPARAM) G.font, 0);
      SetFocus(ok);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(h, &ps);
      RECT cli;
      GetClientRect(h, &cli);
      FillRect(hdc, &cli, G.face_brush);
      /*  With LR_SHARED, the system owns the icon.  */
      HICON ico = (HICON) LoadImageA(G.instance, MAKEINTRESOURCEA(1),
          IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED);
      if (ico) DrawIconEx(hdc, 18, 24, ico, 32, 32, 0, NULL, DI_NORMAL);
      HFONT oldf = (HFONT) SelectObject(hdc, G.font);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
      const char * l1 = "fdchk " APP_VERSION " -- floppy disk checker";
      const char * l2 = "Kamila Szewczyk, GNU GPL version 3";
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
      if (G.main) {
        EnableWindow(G.main, TRUE);
        SetForegroundWindow(G.main);
      }
      return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

void ui_about(HWND window) {
  static int registered = 0;
  if (!registered) {
    WNDCLASSA wc;  memzero(&wc, sizeof wc);
    wc.lpfnWndProc   = about_proc;
    wc.hInstance     = G.instance;
    wc.hIcon         = LoadIconA(G.instance, MAKEINTRESOURCEA(1));
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = "FdchkAbout";
    RegisterClassA(&wc);
    registered = 1;
  }
  int w = 300, h = 110;
  RECT pr;
  GetWindowRect(window, &pr);
  RECT rc = { 0, 0, w, h };
  AdjustWindowRect(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
  HWND about = CreateWindowExA(WS_EX_DLGMODALFRAME,
      "FdchkAbout", "About fdchk", WS_POPUP | WS_CAPTION | WS_SYSMENU,
      pr.left + ((pr.right - pr.left) - w) / 2,
      pr.top  + ((pr.bottom - pr.top) - h) / 2,
      rc.right - rc.left, rc.bottom - rc.top, window, NULL, G.instance, NULL);
  if (!about) return;
  EnableWindow(window, FALSE);
  ShowWindow(about, SW_SHOW);
  UpdateWindow(about);
}

#define ID_LV_LIST    2001
#define ID_LV_EDIT    2002
#define ID_LV_REFRESH 2003
#define ID_LV_OPENDIR 2004
#define ID_LV_CLOSE   2005

static HWND  log_viewer = NULL;
static HWND  log_list    = NULL;
static HWND  log_edit    = NULL;
static HFONT log_font    = NULL;

static void logs_populate(HWND list) {
  SendMessageA(list, LB_RESETCONTENT, 0, 0);
  char pattern[MAX_PATH + 8];
  wsprintfA(pattern, "%s\\*.log", G.log_dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    SendMessageA(list, LB_ADDSTRING, 0, (LPARAM) "(no logs yet)");
    return;
  }
  do {
    SendMessageA(list, LB_ADDSTRING, 0, (LPARAM) fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

static void logs_load(HWND list, HWND edit) {
  int idx = (int) SendMessageA(list, LB_GETCURSEL, 0, 0);
  if (idx < 0) { SetWindowTextA(edit, ""); return; }
  char name[MAX_PATH];
  SendMessageA(list, LB_GETTEXT, idx, (LPARAM) name);
  if (name[0] == '(') { SetWindowTextA(edit, ""); return; }
  char path[MAX_PATH];
  wsprintfA(path, "%s\\%s", G.log_dir, name);
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) {
    SetWindowTextA(edit, "(cannot open log file)");
    return;
  }
  DWORD sz = GetFileSize(f, NULL);
  if (sz > 256 * 1024) sz = 256 * 1024;
  char * buf = (char *) LocalAlloc(LPTR, sz + 1);
  if (!buf) { CloseHandle(f); return; }
  DWORD cb;
  ReadFile(f, buf, sz, &cb, NULL);
  buf[cb] = 0;
  CloseHandle(f);
  SetWindowTextA(edit, buf);
  LocalFree(buf);
}

static void logs_layout(HWND h) {
  if (!log_list || !log_edit) return;
  RECT cli;
  GetClientRect(h, &cli);
  int pad = 8, gap = 6, bh = 24, list_w = 180;
  int top = pad, bottom = cli.bottom - pad - bh - gap;
  if (bottom < top + 60) bottom = top + 60;
  MoveWindow(log_list, pad, top, list_w, bottom - top, TRUE);
  MoveWindow(log_edit, pad + list_w + gap, top,
             cli.right - 2 * pad - list_w - gap, bottom - top, TRUE);
  int by = cli.bottom - pad - bh;
  int bx = cli.right - pad - 80;
  MoveWindow(GetDlgItem(h, ID_LV_CLOSE),   bx, by, 80, bh, TRUE);
  bx -= 100 + gap;
  MoveWindow(GetDlgItem(h, ID_LV_OPENDIR), bx, by, 100, bh, TRUE);
  bx -= 80 + gap;
  MoveWindow(GetDlgItem(h, ID_LV_REFRESH), bx, by, 80, bh, TRUE);
}

static LRESULT CALLBACK logs_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      log_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          LBS_NOTIFY | LBS_HASSTRINGS,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_LIST, G.instance, NULL);
      log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
          ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_EDIT, G.instance, NULL);
      SendMessageA(log_edit, EM_LIMITTEXT, 0x80000, 0);
      log_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
          ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
      SendMessageA(log_edit, WM_SETFONT, (WPARAM) log_font, 0);
      SendMessageA(log_list, WM_SETFONT, (WPARAM) G.font, 0);
      HWND b1 = CreateWindowExA(0, "BUTTON", "&Refresh",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_REFRESH, G.instance, NULL);
      HWND b2 = CreateWindowExA(0, "BUTTON", "Open &Folder",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_OPENDIR, G.instance, NULL);
      HWND b3 = CreateWindowExA(0, "BUTTON", "&Close",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, 0, 0, 0, h, (HMENU) (UINT_PTR) ID_LV_CLOSE, G.instance, NULL);
      SendMessageA(b1, WM_SETFONT, (WPARAM) G.font, 0);
      SendMessageA(b2, WM_SETFONT, (WPARAM) G.font, 0);
      SendMessageA(b3, WM_SETFONT, (WPARAM) G.font, 0);
      logs_populate(log_list);
      if (SendMessageA(log_list, LB_GETCOUNT, 0, 0) > 0) {
        SendMessageA(log_list, LB_SETCURSEL, 0, 0);
        logs_load(log_list, log_edit);
      }
      return 0;
    }
    case WM_SIZE:
      logs_layout(h);
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(wp), code = HIWORD(wp);
      if (id == ID_LV_LIST && code == LBN_SELCHANGE) {
        logs_load(log_list, log_edit);
      } else if (id == ID_LV_REFRESH) {
        logs_populate(log_list);
        SendMessageA(log_list, LB_SETCURSEL, 0, 0);
        logs_load(log_list, log_edit);
      } else if (id == ID_LV_OPENDIR) {
        char cmd[MAX_PATH + 32];
        wsprintfA(cmd, "explorer.exe \"%s\"", G.log_dir);
        WinExec(cmd, SW_SHOWNORMAL);
      } else if (id == ID_LV_CLOSE || id == IDCANCEL) {
        DestroyWindow(h);
      }
      return 0;
    }
    case WM_SYSCHAR: {
      int id = 0;
      switch (wp | 0x20) {
        case 'r': id = ID_LV_REFRESH; break;
        case 'f': id = ID_LV_OPENDIR; break;
        case 'c': id = ID_LV_CLOSE;   break;
        default: break;
      }
      if (!id) break;
      SendMessageA(h, WM_COMMAND, (WPARAM) id, 0);
      return 1;
    }
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (log_font) { DeleteObject(log_font); log_font = NULL; }
      log_viewer = NULL;
      log_list = log_edit = NULL;
      return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

void ui_logs(HWND parent) {
  static int registered = 0;
  if (!registered) {
    WNDCLASSA wc;  memzero(&wc, sizeof wc);
    wc.lpfnWndProc   = logs_proc;
    wc.hInstance     = G.instance;
    wc.hIcon         = LoadIconA(G.instance, MAKEINTRESOURCEA(1));
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = "FdchkLogs";
    RegisterClassA(&wc);
    registered = 1;
  }
  if (log_viewer && IsWindow(log_viewer)) {
    SetForegroundWindow(log_viewer);
    return;
  }
  int w = 640, h = 420;
  RECT pr;
  GetWindowRect(parent, &pr);
  log_viewer = CreateWindowExA(0, "FdchkLogs", "fdchk - logs",
      WS_OVERLAPPEDWINDOW,
      pr.left + ((pr.right - pr.left) - w) / 2,
      pr.top  + ((pr.bottom - pr.top) - h) / 2,
      w, h, parent, NULL, G.instance, NULL);
  if (log_viewer) {
    ShowWindow(log_viewer, SW_SHOW);
    UpdateWindow(log_viewer);
  }
}

static int running_from_drive(int drive) {
  char exe[MAX_PATH];
  GetModuleFileNameA(NULL, exe, MAX_PATH);
  char c = exe[0];
  if (c >= 'a' && c <= 'z') c -= 32;
  return c == 'A' + drive;
}

#define ID_FMT_CAP    2101
#define ID_FMT_LABEL  2102
#define ID_FMT_QUICK  2103
#define ID_FMT_FULL   2104
#define ID_FMT_OK     2105
#define ID_FMT_CANCEL 2106
#define ID_FMT_DRV    2107
#define ID_FMT_FDC    2108

#define FMT_DLG_W 428
#define FMT_DLG_H 358

static HWND fmt_dlg = NULL;
static HWND fmt_cap = NULL, fmt_label = NULL;
static HWND fmt_quick = NULL, fmt_full = NULL;
static HWND fmt_driver = NULL, fmt_fdc = NULL;
static int  fmt_drive = 0;
static int  fmt_map[16];

static HWND fmt_static(HWND h, const char * s, int x, int y, int w, int hh) {
  HWND t = CreateWindowExA(0, "STATIC", s, WS_CHILD | WS_VISIBLE,
                           x, y, w, hh, h, NULL, G.instance, NULL);
  SendMessageA(t, WM_SETFONT, (WPARAM) G.font, 0);
  return t;
}

static void fmt_start_job(void);

/*  Read the disk geometry without changing the drive settings.  */
static const floppy_geom * fmt_detect_media(int drive) {
  disk_handle d;
  BYTE bpb[SECTOR_SIZE];
  const floppy_geom * g = NULL;
  if (!disk_open(&d, drive)) return NULL;
  if (disk_read(&d, 0, 1, bpb) == 0 && bpb[0x0B] == 0x00 && bpb[0x0C] == 0x02) {
    int total = bpb[0x13] | (bpb[0x14] << 8);
    int spt   = bpb[0x18] | (bpb[0x19] << 8);
    int heads = bpb[0x1A] | (bpb[0x1B] << 8);
    g = geom_for_bpb(total, spt, heads);
  }
  disk_close(&d);
  return g;
}

static LRESULT CALLBACK fmt_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      BYTE dt = G.drive_type[fmt_drive & 1];

      fmt_static(h, "Capacity:", 14, 18, 70, 16);
      fmt_cap = CreateWindowExA(0, "COMBOBOX", NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWNLIST, 90, 14, 322, 200, h,
          (HMENU) (UINT_PTR) ID_FMT_CAP, G.instance, NULL);
      SendMessageA(fmt_cap, WM_SETFONT, (WPARAM) G.font, 0);

      /*  List the formats this drive supports.  */
      const floppy_geom * want = fmt_detect_media(fmt_drive);
      if (!want) want = geom_for_drive_type(dt);
      int i, n = 0, sel = -1, last = 0;
      for (i = 0; i < geom_count() && n < 16; i++) {
        const floppy_geom * g = geom_at(i);
        if (!geom_fits_drive(g, dt)) continue;
        SendMessageA(fmt_cap, CB_ADDSTRING, 0, (LPARAM) g->name);
        if (g == want) sel = n;
        last = n;
        fmt_map[n++] = i;
      }
      SendMessageA(fmt_cap, CB_SETCURSEL, sel >= 0 ? sel : last, 0);

      fmt_static(h, "Label:", 14, 50, 70, 16);
      fmt_label = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "FDCHK",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE,
          90, 46, 140, 22, h, (HMENU) (UINT_PTR) ID_FMT_LABEL,
          G.instance, NULL);
      SendMessageA(fmt_label, WM_SETFONT, (WPARAM) G.font, 0);
      SendMessageA(fmt_label, EM_LIMITTEXT, 11, 0);

      HWND grp = CreateWindowExA(0, "BUTTON", "Format type",
          WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
          14, 80, 398, 118, h, NULL, G.instance, NULL);
      SendMessageA(grp, WM_SETFONT, (WPARAM) G.font, 0);

      fmt_quick = CreateWindowExA(0, "BUTTON", "&Quick",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP |
          BS_AUTORADIOBUTTON | WS_GROUP,
          26, 100, 300, 18, h, (HMENU) (UINT_PTR) ID_FMT_QUICK,
          G.instance, NULL);
      fmt_static(h, "Writes the boot sector, FATs and root directory.",
                 44, 118, 360, 14);

      fmt_full = CreateWindowExA(0, "BUTTON", "F&ull (track-level)",
          WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
          26, 138, 300, 18, h, (HMENU) (UINT_PTR) ID_FMT_FULL,
          G.instance, NULL);
      fmt_static(h, "Formats and checks every track, then writes the",
                 44, 156, 360, 14);
      fmt_static(h, "filesystem. Use this if fdchk cannot read or format "
                    "the disk.", 44, 170, 360, 14);
      SendMessageA(fmt_quick, WM_SETFONT, (WPARAM) G.font, 0);
      SendMessageA(fmt_full,  WM_SETFONT, (WPARAM) G.font, 0);
      SendMessageA(fmt_full, BM_SETCHECK, BST_CHECKED, 0);

      HWND grp2 = CreateWindowExA(0, "BUTTON", "Track formatter (full only)",
          WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
          14, 206, 398, 84, h, NULL, G.instance, NULL);
      SendMessageA(grp2, WM_SETFONT, (WPARAM) G.font, 0);

      fmt_driver = CreateWindowExA(0, "BUTTON",
          "&Block driver (Int 21h 440Dh)",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP |
          BS_AUTORADIOBUTTON | WS_GROUP,
          26, 226, 340, 18, h, (HMENU) (UINT_PTR) ID_FMT_DRV,
          G.instance, NULL);
      fmt_fdc = CreateWindowExA(0, "BUTTON",
          "&Raw FDC via fdchk.vxd (command 4Dh, non-DMA)",
          WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
          26, 246, 340, 18, h, (HMENU) (UINT_PTR) ID_FMT_FDC,
          G.instance, NULL);
      SendMessageA(fmt_driver, WM_SETFONT, (WPARAM) G.font, 0);
      SendMessageA(fmt_fdc, WM_SETFONT, (WPARAM) G.font, 0);
      SendMessageA(fmt_driver, BM_SETCHECK, BST_CHECKED, 0);
      if (!G.vxd) {
        EnableWindow(fmt_fdc, FALSE);
        fmt_static(h, "fdchk.vxd did not load. Raw FDC format is off.",
                   44, 266, 360, 14);
      } else {
        fmt_static(h, "Use raw FDC if the block driver cannot format tracks.",
                   44, 266, 360, 14);
      }

      HWND ok = CreateWindowExA(0, "BUTTON", "&Format",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP |
          BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
          FMT_DLG_W - 8 - 2 * 85 - 6, FMT_DLG_H - 8 - 24 - 4, 85, 24,
          h, (HMENU) (UINT_PTR) ID_FMT_OK, G.instance, NULL);
      HWND cancel = CreateWindowExA(0, "BUTTON", "&Cancel",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          FMT_DLG_W - 8 - 85, FMT_DLG_H - 8 - 24 - 4, 85, 24,
          h, (HMENU) (UINT_PTR) ID_FMT_CANCEL, G.instance, NULL);
      SendMessageA(ok,     WM_SETFONT, (WPARAM) G.font, 0);
      SendMessageA(cancel, WM_SETFONT, (WPARAM) G.font, 0);
      SetFocus(ok);
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (id == IDOK) id = ID_FMT_OK;
      if (id == ID_FMT_OK) {
        int cur = (int) SendMessageA(fmt_cap, CB_GETCURSEL, 0, 0);
        if (cur < 0) return 0;
        const floppy_geom * g = geom_at(fmt_map[cur]);
        int full = SendMessageA(fmt_full, BM_GETCHECK, 0, 0) == BST_CHECKED;

        char label[16];
        label[0] = 0;
        GetWindowTextA(fmt_label, label, sizeof label);

        int by_fdc = SendMessageA(fmt_fdc, BM_GETCHECK, 0, 0) == BST_CHECKED;

        char msg[480];
        wsprintfA(msg,
            "Format drive %c: as %s?\r\n\r\n%s\r\n\r\n"
            "Formatting will delete all data on the disk.",
            'A' + fmt_drive, g->name,
            full ? (by_fdc
                    ? "Raw FDC will format and check every track; this "
                      "takes a few minutes."
                    : "The block driver will format and check every track; "
                      "this takes a few minutes.")
                 : "Quick format writes the boot sector, FATs and root "
                   "directory.");
        if (MessageBoxA(h, msg, "Format floppy",
                MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
          return 0;

        G.fmt_geom   = g;
        G.fmt_style  = full ? FMT_FULL : FMT_QUICK;
        G.fmt_method = by_fdc ? FMT_BY_FDC : FMT_BY_DRIVER;
        lstrcpynA(G.fmt_label, label, sizeof G.fmt_label);
        DestroyWindow(h);
        fmt_start_job();
        return 0;
      }
      if (id == ID_FMT_CANCEL || id == IDCANCEL) {
        DestroyWindow(h);
        return 0;
      }
      return 0;
    }
    case WM_SYSCHAR: {
      HWND t = NULL;
      switch (wp | 0x20) {
        case 'q': t = fmt_quick; break;
        case 'u': t = fmt_full;  break;
        case 'b': t = fmt_driver;   break;
        case 'r': t = fmt_fdc;   break;
        case 'f': t = GetDlgItem(h, ID_FMT_OK);     break;
        case 'c': t = GetDlgItem(h, ID_FMT_CANCEL); break;
        default: break;
      }
      if (t && IsWindowEnabled(t)) {
        SetFocus(t);
        SendMessageA(t, BM_CLICK, 0, 0);
        return 1;
      }
      break;
    }

    case WM_CTLCOLORSTATIC:
      SetBkMode((HDC) wp, TRANSPARENT);
      SetTextColor((HDC) wp, GetSysColor(COLOR_BTNTEXT));
      return (LRESULT) G.face_brush;
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      fmt_dlg = NULL;
      fmt_cap = fmt_label = fmt_quick = fmt_full = NULL;
      fmt_driver = fmt_fdc = NULL;
      if (G.main) {
        EnableWindow(G.main, TRUE);
        SetForegroundWindow(G.main);
      }
      return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}

static void fmt_start_job(void) {
  HWND window = G.main;
  G.drive       = fmt_drive;
  G.mode        = MODE_FORMAT;
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

  EnableWindow(G.start_button,      FALSE);
  EnableWindow(G.stop_button,       TRUE);
  EnableWindow(G.recover_button,    FALSE);
  EnableWindow(G.format_button,     FALSE);
  EnableWindow(G.defrag_button,     FALSE);
  EnableWindow(G.standard,   FALSE);
  EnableWindow(G.thorough,   FALSE);
  EnableWindow(G.diagnostic, FALSE);
  EnableWindow(G.chkfs,      FALSE);

  trail_clear();
  SendMessageA(G.statusbar, SB_SETTEXTA, 0,
               (LPARAM) (G.drive ? "Drive: B:" : "Drive: A:"));
  SendMessageA(G.statusbar, SB_SETTEXTA, 2, (LPARAM) "Formatting...");

  SetTimer(window, 1, 100, NULL);
  SetTimer(window, 2, 150, NULL);
  G.thread = CreateThread(NULL, 0, format_thread, NULL, 0, &G.tid);
  if (!G.thread) {
    G.running = 0;
    lstrcpyA(G.fmt_msg, "Cannot start formatting.");
    G.fmt_rc = -1;
    ui_worker_done(window);
  }
}

void ui_format(HWND window) {
  if (G.running) return;
  int drive = (G.has_b && !G.has_a) ? 1
            : (SendMessageA(G.drive_b, BM_GETCHECK, 0, 0) == BST_CHECKED
               ? 1 : 0);
  if (running_from_drive(drive)) {
    char m[320];
    wsprintfA(m,
        "Cannot format drive %c: while fdchk runs from it.\r\n\r\n"
        "Run fdchk from a hard disk.",
        'A' + drive);
    MessageBoxA(window, m, "Format floppy", MB_OK | MB_ICONERROR);
    return;
  }

  static int registered = 0;
  if (!registered) {
    WNDCLASSA wc;
    memzero(&wc, sizeof wc);
    wc.lpfnWndProc   = fmt_proc;
    wc.hInstance     = G.instance;
    wc.hIcon         = LoadIconA(G.instance, MAKEINTRESOURCEA(1));
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    wc.lpszClassName = "FdchkFormat";
    RegisterClassA(&wc);
    registered = 1;
  }
  if (fmt_dlg && IsWindow(fmt_dlg)) {
    SetForegroundWindow(fmt_dlg);
    return;
  }

  fmt_drive = drive;
  char title[64];
  wsprintfA(title, "Format drive %c:", 'A' + drive);

  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  RECT pr, rc = { 0, 0, FMT_DLG_W, FMT_DLG_H };
  GetWindowRect(window, &pr);
  AdjustWindowRect(&rc, style, FALSE);
  fmt_dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, "FdchkFormat", title,
      style,
      pr.left + ((pr.right - pr.left) - FMT_DLG_W) / 2,
      pr.top  + ((pr.bottom - pr.top) - FMT_DLG_H) / 2,
      rc.right - rc.left, rc.bottom - rc.top, window, NULL, G.instance, NULL);
  if (!fmt_dlg) return;
  EnableWindow(window, FALSE);
  ShowWindow(fmt_dlg, SW_SHOW);
  UpdateWindow(fmt_dlg);
}

void ui_defrag(HWND window) {
  int i;
  if (G.running) return;
  int drive = (G.has_b && !G.has_a) ? 1
            : (SendMessageA(G.drive_b, BM_GETCHECK, 0, 0) == BST_CHECKED
               ? 1 : 0);
  if (running_from_drive(drive)) {
    char m[320];
    wsprintfA(m,
        "Cannot defragment drive %c: while fdchk runs from it.\r\n\r\n"
        "Run fdchk from a hard disk.",
        'A' + drive);
    MessageBoxA(window, m, "Defragment floppy", MB_OK | MB_ICONERROR);
    return;
  }
  char msg[400];
  wsprintfA(msg,
      "Defragment drive %c:?\r\n\r\n"
      "Files and directories will move to lower cluster numbers. fdchk will\r\n"
      "not move bad clusters. It will fill free clusters with zeros.\r\n\r\n"
      "An interruption may corrupt the disk.",
      'A' + drive);
  if (MessageBoxA(window, msg, "Defragment floppy",
      MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
    return;

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

  EnableWindow(G.start_button,      FALSE);
  EnableWindow(G.stop_button,       TRUE);
  EnableWindow(G.recover_button,    FALSE);
  EnableWindow(G.format_button,     FALSE);
  EnableWindow(G.defrag_button,     FALSE);
  EnableWindow(G.standard,   FALSE);
  EnableWindow(G.thorough,   FALSE);
  EnableWindow(G.diagnostic, FALSE);
  EnableWindow(G.chkfs,      FALSE);

  Fi(MAX_SECTORS, G.state[i] = ST_UNTESTED);
  mark_system_sectors();
  trail_clear();
  InvalidateRect(G.grid, NULL, FALSE);

  SendMessageA(G.statusbar, SB_SETTEXTA, 0,
               (LPARAM) (drive ? "Drive: B:" : "Drive: A:"));
  SendMessageA(G.statusbar, SB_SETTEXTA, 2, (LPARAM) "Defragmenting...");

  SetTimer(window, 1, 100, NULL);
  SetTimer(window, 2, 150, NULL);
  G.thread = CreateThread(NULL, 0, defrag_thread, NULL, 0, &G.tid);
  if (!G.thread) { G.running = 0; G.defrag_rc = 1; ui_worker_done(window); }
}

void ui_recover(HWND window) {
  if (G.bad_n == 0) {
    MessageBoxA(window, "No bad sectors to recover.", APP_NAME,
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  char folder[MAX_PATH];
  GetCurrentDirectoryA(MAX_PATH, folder);
  char buf[MAX_PATH + 200];
  wsprintfA(buf,
      "Copy readable files from drive %c: to:\r\n\r\n%s\r\n\r\n"
      "fdchk will write zeros in place of file data it cannot read. Details "
      "go to fdchk-recovery.log.\r\n\r\n"
      "Continue?",
      'A' + G.drive, folder);
  if (MessageBoxA(window, buf, "Recover files",
                  MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
    recover_files(window, folder);
}
