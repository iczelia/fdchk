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

/*  Surface test for a single sector..  */
static int test_one_sector(DiskHandle * d, DWORD lba,
                           BYTE * orig, BYTE * pat, BYTE * chk) {
  int r;
  ui_set_state((int) lba, ST_SCANNING);
  r = disk_read(d, lba, 1, orig);
  if (r != 0) return r;
  if (G.mode == MODE_STANDARD) return 0;
  /*  Each pattern step: fill pat, write, read back, compare.  */
  #define W_AND_V()                                       \
    do {                                                  \
      ui_set_state((int) lba, ST_WRITING);                \
      r = disk_write(d, lba, 1, pat);  if (r) goto restore; \
      ui_set_state((int) lba, ST_VERIFY);                 \
      r = disk_read(d, lba, 1, chk);   if (r) goto restore; \
      if (memcmp(pat, chk, SECTOR_SIZE)) {                \
        r = -DOS_ERR_DATA; goto restore;                  \
      }                                                   \
    } while (0)
  /*  (a) write the original back  */
  memcpy(pat, orig, SECTOR_SIZE);
  W_AND_V();
  /*  (b) write ~original  */
  for (int i = 0; i < SECTOR_SIZE; ++i) pat[i] = (BYTE) ~orig[i];
  W_AND_V();
  /*  (c) write a fresh random block  */
  for (int i = 0; i < SECTOR_SIZE; i += 4) {
    DWORD x = rng_next();
    pat[i]     = (BYTE) x;
    pat[i + 1] = (BYTE) (x >> 8);
    pat[i + 2] = (BYTE) (x >> 16);
    pat[i + 3] = (BYTE) (x >> 24);
  }
  W_AND_V();
  /*  (d) write ~random  */
  for (int i = 0; i < SECTOR_SIZE; ++i) pat[i] = (BYTE) ~pat[i];
  W_AND_V();
  r = 0;
restore:
  /*  Always try to put orig back, even after a failed step.  */
  memcpy(pat, orig, SECTOR_SIZE);
  {
    int rr = disk_write(d, lba, 1, pat);
    if (r == 0 && rr != 0) r = rr;
    if (rr == 0 && r == 0 && disk_read(d, lba, 1, chk) == 0
        && memcmp(orig, chk, SECTOR_SIZE) != 0)
      r = -DOS_ERR_DATA;
  }
  #undef W_AND_V
  return r;
}

/*  Mark boot + FAT + root-directory sectors so the grid shows them teal.  */
void mark_system_sectors(void) {
  int root_sec = (G.root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
  int last = G.reserved_sec + G.num_fats * G.fat_size + root_sec;
  for (int i = 0; i < last && i < MAX_SECTORS; ++i)
    if (G.state[i] == ST_UNTESTED) G.state[i] = ST_SYSTEM;
}

/*  Preload clusters already marked bad (0xFF7) in the FAT.  */
static void mark_existing_bad(DiskHandle * d) {
  BYTE * fat = (BYTE *) LocalAlloc(LPTR, G.fat_size * SECTOR_SIZE);
  if (!fat) return;
  if (disk_read(d, (DWORD) G.reserved_sec, (WORD) G.fat_size, fat) == 0) {
    for (int n = 2; n < G.total_clusters + 2; ++n) {
      if (fat12_get(fat, n) != 0xFF7) continue;
      DWORD lba = cluster_to_lba(n);
      for (int s = 0; s < G.sec_per_cluster; ++s)
        ui_set_state((int) lba + s, ST_BAD_OLD);
    }
  }
  LocalFree(fat);
}

/*  Diagnostic mode.  */
static int diag_read_id(DiskHandle * dh, int drive, int cyl, int head,
                        BYTE * st0, BYTE * st1, BYTE * st2,
                        BYTE * c_out, BYTE * h_out,
                        BYTE * r_out, BYTE * n_out) {
  FdcIn in_buf;  FdcOut out;
  (void) dh;
  memzero(&in_buf, sizeof in_buf);
  in_buf.drive = (BYTE) drive;
  in_buf.head  = (BYTE) head;
  in_buf.cyl   = (BYTE) cyl;
  vxd_call(IOCTL_FDC_SEEK, &in_buf, &out);
  if (vxd_call(IOCTL_FDC_READID, &in_buf, &out) && out.result_n >= 7) {
    *st0 = out.st0; *st1 = out.st1; *st2 = out.st2;
    *c_out = out.c; *h_out = out.h;
    *r_out = out.r; *n_out = out.n;
    return ((out.st0 & 0xC0) == 0) ? 0 : 1;
  }
  *st0 = 0x80;   /*  invalid command: the controller never answered  */
  *st1 = *st2 = 0;
  *c_out = *h_out = *r_out = *n_out = 0;
  return 1;
}

static DWORD diag_worker(DiskHandle * dh) {
  if (!G.hVxd) {
    ui_prompt("Diagnostic mode needs the raw-FDC driver, which did not "
              "load.\n\n"
              "fdchk.vxd is embedded in the executable and loads itself on "
              "start; if it failed, this is not Windows 95/98/Me or the VxD "
              "loader refused it.\n\n"
              "The other test modes do not need it.",
              APP_NAME, MB_OK | MB_ICONWARNING);
    ui_status("Diagnostic mode unavailable: fdchk.vxd did not load.");
    return 0;
  }


  HANDLE log = log_create("fdchk-diag.log");
  int wrong_cyl = 0, no_am = 0, ok_reads = 0, stress_fail = 0, stress_n = 0;
  DWORD seek_total = 0, seek_avg10 = 0, rnd_total = 0, rnd_avg10 = 0;
  const DWORD rnd_n = 50;

  /*  Physical layout of the drive under test.  */
  BYTE dev_type = disk_probe_drive_type(dh);
  const FloppyGeom * dg = geom_for_drive_type(dev_type);
  int n_cyl  = dg ? dg->cyls  : MAX_CYLS;
  int n_head = dg ? dg->heads : MAX_HEADS;
  int n_cell = n_cyl * n_head;
  int inner  = n_cyl - 1;
  G.drive_type[G.drive & 1] = dev_type;
  ui_drive_labels();
  G.cyls  = n_cyl;
  G.heads = n_head;

  SYSTEMTIME t;
  GetLocalTime(&t);
  LOG_FMT(log,
      "fdchk Drive Diagnostic Log\r\n"
      "Drive: %c:   Type: %s   (raw FDC via fdchk.vxd)\r\n"
      "Layout: %d cylinders x %d head(s)\r\n"
      "Date:  %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
      'A' + G.drive, drive_type_str(dev_type), n_cyl, n_head,
      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

  {
    FdcOut ms;
    int m = vxd_sense_media(G.drive, &ms);
    LOG_FMT(log,
        "Media sense:  %s\r\n"
        "  DIR before=%02x after=%02x (bit 7 = disk change)\r\n"
        "  READ ID  ST=%02x %02x %02x  C=%d H=%d R=%d N=%d  n=%d  status=%02x\r\n\r\n",
        m == MEDIA_PRESENT    ? "disk present, sector IDs readable" :
        m == MEDIA_NONE       ? "drive reports empty" :
        m == MEDIA_UNREADABLE ? "disk present, no readable sector IDs"
                              : "unavailable",
        ms.dir_before, ms.dir_after,
        ms.st0, ms.st1, ms.st2, ms.c, ms.h, ms.r, ms.n,
        ms.result_n, ms.status);
  }

  /*  Repurpose the state map as one cell per (cylinder, head).  */
  G.total_sec = n_cell;
  for (int i = 0; i < n_cell; ++i) G.state[i] = ST_UNTESTED;
  SendMessageA(G.hMain, WM_APP_REPAINT, 0, 0);

  DWORD t_start = now_ms();
  G.t_start = t_start;

  /*  Test 1: reset.  */
  ui_status("[1/5] Reset controller...");
  DWORD t0 = now_ms();
  {
    FdcOut o;
    vxd_call(IOCTL_FDC_RESET, NULL, &o);
    LOG_FMT(log, "Reset:        %lums  ST0=%02x  status=%02x\r\n",
            (DWORD) (now_ms() - t0), o.st0, o.status);
  }
  if (G.abort_req) goto done;

  /*  Test 2: recalibrate.  */
  ui_status("[2/5] Recalibrate (seek to track 0)...");
  t0 = now_ms();
  {
    FdcIn in_buf;
    FdcOut o;
    memzero(&in_buf, sizeof in_buf);
    in_buf.drive = (BYTE) G.drive;
    vxd_call(IOCTL_FDC_RECAL, &in_buf, &o);
    LOG_FMT(log,
        "Recalibrate:  %lums  ST0=%02x  cur_cyl=%d  status=%02x\r\n",
        (DWORD) (now_ms() - t0), o.st0, o.cur_cyl, o.status);
  }
  if (G.abort_req) goto done;

  /*  Tests 3 & 4: READ ID at every (cyl, head).  GetTickCount is too
      coarse to time a single seek, so time the whole sweep.  */
  ui_status("[3/5] Reading sector IDs from every track on both heads...");
  log_write(log,
      "\r\nREAD ID sweep (track, head, ST0, ST1, ST2, C, H, R, N):\r\n");
  DWORD sweep_start = now_ms();
  for (int cyl = 0; cyl < n_cyl && !G.abort_req; ++cyl) {
    for (int head = 0; head < n_head && !G.abort_req; ++head) {
      int idx = cyl * n_head + head;
      G.current_sec = idx;
      G.state[idx] = ST_SCANNING;
      grid_invalidate_cell(idx);

      BYTE st0 = 0, st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
      int rc = diag_read_id(dh, G.drive, cyl, head,
                            &st0, &st1, &st2, &c, &h, &r, &n);
      BYTE new_st;
      if (rc == 0 && (st0 & 0xC0) == 0) {
        if (c == cyl) { new_st = ST_GOOD; ok_reads++; }
        else          { new_st = ST_WRONG_CYL; wrong_cyl++; }
      } else if (st2 & 0x10) {
        new_st = ST_WRONG_CYL; wrong_cyl++;
      } else if ((st1 & 0x01) || (st2 & 0x01)) {
        new_st = ST_NO_AM; no_am++;
      } else {
        new_st = ST_BAD_NEW;
      }
      ui_set_state(idx, new_st);
      G.scanned++;
      if ((idx & 7) == 0)
        PostMessageA(G.hMain, WM_APP_PROGRESS,
                     (WPARAM) G.scanned, (LPARAM) G.total_sec);
      LOG_FMT(log,
          "  %2d/%d  ST=%02x %02x %02x  C=%d H=%d R=%d N=%d  %s\r\n",
          cyl, head, st0, st1, st2, c, h, r, n, st_summary(st0, st1, st2));
    }
  }
  seek_total = now_ms() - sweep_start;
  seek_avg10 = G.scanned ? (seek_total * 10 / G.scanned) : 0;
  LOG_FMT(log,
      "\r\nREAD ID summary: %d OK, %d wrong-cyl, %d no-AM, %lu other\r\n"
      "Sweep total: %lums  avg %lu.%lu ms/read\r\n\r\n",
      ok_reads, wrong_cyl, no_am,
      (DWORD) (G.scanned - ok_reads - wrong_cyl - no_am),
      seek_total, seek_avg10 / 10, seek_avg10 % 10);
  if (G.abort_req) goto done;

  /*  Test 5: innermost-track stress.  */
  {
    char m[80];
    wsprintfA(m, "[4/5] Innermost-track READ ID stress (track %d)...", inner);
    ui_status(m);
    LOG_FMT(log, "Innermost-track stress (50x READ ID at track %d):\r\n",
            inner);
  }
  for (int i = 0; i < 50 && !G.abort_req; ++i) {
    for (int head = 0; head < n_head && !G.abort_req; ++head) {
      BYTE st0 = 0, st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
      int rc = diag_read_id(dh, G.drive, inner, head,
                            &st0, &st1, &st2, &c, &h, &r, &n);
      stress_n++;
      if (rc || (st0 & 0xC0) || c != inner) stress_fail++;
    }
  }
  LOG_FMT(log, "  %d reads, %d failures (%d%% reliability)\r\n\r\n",
          stress_n, stress_fail,
          stress_n ? (100 * (stress_n - stress_fail)) / stress_n : 0);

  /*  Test 6: random seek timing.  Batch-timed for the same reason.  */
  ui_status("[5/5] Random seek timing (50 jumps)...");
  log_write(log, "Random seek timing:\r\n");
  DWORD batch_start = now_ms();
  for (DWORD i = 0; i < rnd_n && !G.abort_req; ++i) {
    int cyl = (int) (rng_next() % (DWORD) n_cyl);
    FdcIn in_buf;
    FdcOut o;
    memzero(&in_buf, sizeof in_buf);
    in_buf.drive = (BYTE) G.drive;
    in_buf.cyl   = (BYTE) cyl;
    vxd_call(IOCTL_FDC_SEEK, &in_buf, &o);
  }
  rnd_total = now_ms() - batch_start;
  rnd_avg10 = rnd_n ? (rnd_total * 10 / rnd_n) : 0;
  LOG_FMT(log, "  %lu seeks in %lums total -> avg %lu.%lu ms per seek\r\n\r\n",
          rnd_n, rnd_total, rnd_avg10 / 10, rnd_avg10 % 10);

done:
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);

  /*  Status line + a verdict popup that explains the numbers.  */
  {
    DWORD el = now_ms() - t_start;
    char els[16];
    fmt_hms(el, els);
    int bad_other = (int) G.scanned - ok_reads - wrong_cyl - no_am;
    if (bad_other < 0) bad_other = 0;

    char ln[128];
    wsprintfA(ln,
        "Diag complete: OK=%d  Wrong-Cyl=%d  No-AM=%d  Other=%d  (%s)",
        ok_reads, wrong_cyl, no_am, bad_other, els);
    ui_status(ln);

    const char * verdict;
    UINT vicon;
    /*  Thresholds scale with the drive.  */
    int ok_thresh     = n_cell - n_cell / 16;
    int stress_ok     = stress_n / 20;
    int stress_severe = stress_n / 5;
    if (ok_reads >= ok_thresh && wrong_cyl == 0 && no_am == 0 &&
        stress_fail <= stress_ok) {
      verdict = "Drive looks healthy.";
      vicon = MB_ICONINFORMATION;
    } else if (wrong_cyl > 0) {
      verdict = "Wrong-Cylinder failures detected - the head is reading "
                "IDs from the wrong track.  The drive may need re-alignment "
                "or replacement.";
      vicon = MB_ICONWARNING;
    } else if (no_am > 5) {
      verdict = "Many tracks return no address mark.  The disk may be "
                "unformatted, or the drive is failing to read those "
                "tracks at all.";
      vicon = MB_ICONWARNING;
    } else if (stress_fail > stress_severe) {
      verdict = "Innermost-track stress failed often.  The head is "
                "degraded.  Innermost tracks fail first as the magnetic "
                "field weakens.";
      vicon = MB_ICONWARNING;
    } else {
      verdict = "Some tracks failed but with no clear pattern.  Try a "
                "known-good disk.";
      vicon = MB_ICONWARNING;
    }

    char summary[1024];
    wsprintfA(summary,
        "Drive Diagnostic Results (drive %c:)\r\n\r\n"
        "Phase 1 - Reset:        ran in elapsed %s\r\n"
        "Phase 2 - Recalibrate:  issued\r\n"
        "Phase 3 - Track sweep:  %d of %d cells read cleanly\r\n"
        "                        %d wrong-cylinder failures\r\n"
        "                        %d no-address-mark failures\r\n"
        "                        %lu.%lu ms avg per READ ID\r\n"
        "Phase 4 - Inner stress: %d of %d reads failed at track %d\r\n"
        "Phase 5 - Random seeks: %lu.%lu ms avg per seek\r\n\r\n"
        "%s\r\n\r\nFull log: fdchk-diag.log",
        'A' + G.drive, els, ok_reads, n_cell, wrong_cyl, no_am,
        seek_avg10 / 10, seek_avg10 % 10, stress_fail, stress_n, inner,
        rnd_avg10 / 10, rnd_avg10 % 10, verdict);
    MessageBoxA(G.hMain, summary, "Drive Diagnostic", MB_OK | vicon);
  }
  return 0;
}

/*  Surface-scan worker thread.  */

DWORD WINAPI worker_proc(LPVOID arg) {
  (void) arg;
  DiskHandle dh;
  int rc, resynced = 0;
  BYTE * orig = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  BYTE * pat  = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  BYTE * chk  = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  BYTE * bpb  = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  if (!orig || !pat || !chk || !bpb) goto cleanup;
  if (!disk_open(&dh, G.drive)) {
    ui_status("ERROR: Cannot open drive (no driver / not Win9x).");
    ui_prompt("Could not open the floppy drive.\n"
              "Make sure a disk is inserted and that you are running "
              "Windows 95/98/Me.",
              APP_NAME, MB_OK | MB_ICONERROR);
    goto cleanup;
  }
  rc = disk_lock(&dh, 1);
  if (rc < 0) {
    char msg[160];
    wsprintfA(msg,
        "Could not lock drive %c: (%s).\n\n"
        "Close any other programs using the disk and try again.",
        'A' + G.drive, dos_err_str(rc));
    ui_prompt(msg, APP_NAME, MB_OK | MB_ICONWARNING);
    ui_status("Lock failed.");
    goto close_disk;
  }
  G.drive_type[G.drive & 1] = disk_probe_drive_type(&dh);
  ui_drive_labels();
  /*  Probe before dispatch so every mode gets the same media detection.  */
retry_probe: {
  int present = probe_disk_present(&dh);
  if (present == MEDIA_NONE) {
    if (ui_prompt("No disk in the drive.\n\n"
                  "Insert a floppy and click Retry, or Cancel to abort.",
                  APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY)
      goto retry_probe;
    goto unlock;
  }
  if (present == MEDIA_UNREADABLE) {
    if (!resynced && disk_resync_media(&dh)) {
      resynced = 1;
      goto retry_probe;
    }
    if (ui_prompt("Disk is unreadable at every position probed.\n\n"
                  "Either the tracks carry no valid sector IDs (unformatted,\n"
                  "written at another density, or the address marks have\n"
                  "decayed) or the disk is physically damaged.\n\n"
                  "A Full format re-lays every track and often revives such\n"
                  "a disk.  Continue scanning anyway?",
                  APP_NAME, MB_OKCANCEL | MB_ICONWARNING) != IDOK)
      goto unlock;
  }
}

  if (G.mode == MODE_DIAGNOSTIC) { diag_worker(&dh);  goto unlock; }
  if (G.mode == MODE_CHKFS)      { chkfs_worker(&dh); goto unlock; }
  /*  Read the BPB.  */
  ui_status("Reading boot sector / BPB...");
  rc = disk_read(&dh, 0, 1, bpb);
  if (rc != 0 && !resynced && disk_resync_media(&dh)) {
    resynced = 1;
    rc = disk_read(&dh, 0, 1, bpb);
  }
  if (rc != 0 || !parse_bpb(bpb)) {
    /*  No usable BPB.  */
    const FloppyGeom * g =
        geom_for_drive_type(G.drive_type[G.drive & 1]);
    geom_apply(g ? g : geom_for_size(2880));
    lstrcpyA(G.fs_type, "RAW");
    G.vol_label[0] = 0;
    G.has_fat = 0;
    SendMessageA(G.hStatus, SB_SETTEXTA, 1, (LPARAM) "RAW");
  } else {
    G.has_fat = 1;
    SendMessageA(G.hStatus, SB_SETTEXTA, 1, (LPARAM) G.fs_type);
  }

  SendMessageA(G.hMain, WM_APP_REPAINT, 0, 0);

  for (int i = 0; i < MAX_SECTORS; ++i) G.state[i] = ST_UNTESTED;
  if (G.has_fat) {
    mark_system_sectors();
    mark_existing_bad(&dh);
  }
  /*  Counters were zeroed by start_scan, so a re-scan starts clean.  */
  G.t_start = now_ms();
  rng_seed(G.t_start ^ 0xCAFEBABEu);

  for (DWORD lba = 0; lba < (DWORD) G.total_sec; ++lba) {
    if (G.abort_req) goto unlock;
    if (G.state[lba] == ST_BAD_OLD) continue;   /*  skip known-bad  */
    G.current_sec = (int) lba;

    int attempts = 0, last_err = 0;
  retry_sector:
    attempts++;
    last_err = test_one_sector(&dh, lba, orig, pat, chk);
    if (last_err == 0) {
      ui_set_state((int) lba, ST_GOOD);
      G.good_count++;
    } else {
      int e = -last_err;
      /*  media changed: re-prompt for the original disk  */
      if (e == DOS_ERR_CHANGE_LINE) {
        if (attempts < 3) { Sleep(200); goto retry_sector; }
        if (ui_prompt("The disk appears to have been changed.\n\n"
                      "Reinsert the original disk and click Retry.",
                      APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY) {
          attempts = 0;
          goto retry_sector;
        }
        goto unlock;
      }
      /*  write-protect: prompt to slide the tab  */
      if (e == DOS_ERR_WRITE_PROTECT) {
        if (ui_prompt("Disk is write-protected.\n\n"
                      "Slide the write-protect tab and click Retry,\n"
                      "or click Cancel to abort the scan.",
                      APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY) {
          attempts = 0;
          goto retry_sector;
        }
        goto unlock;
      }
      /*  drive not ready  */
      if (e == DOS_ERR_TIMEOUT) {
        if (ui_prompt("Drive is not ready.\n\n"
                      "Insert a disk into the drive and click Retry.",
                      APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY) {
          attempts = 0;
          goto retry_sector;
        }
        goto unlock;
      }
      /*  transient errors: retry a few times  */
      if ((e == DOS_ERR_DMA_OVERRUN || e == DOS_ERR_CTRL_FAIL ||
           e == DOS_ERR_SEEK_FAIL) && attempts < 3) {
        Sleep(100);
        goto retry_sector;
      }
      /*  otherwise: mark the sector bad  */
      ui_set_state((int) lba, ST_BAD_NEW);
      if (G.bad_n < MAX_BAD) G.bad_lba[G.bad_n++] = lba;
      G.bad_count++;
      char buf[120];
      wsprintfA(buf, "Bad sector %lu (%s)",
                (unsigned long) lba, dos_err_str(last_err));
      ui_status(buf);
    }
    G.scanned++;
    if ((G.scanned & 7) == 0)
      PostMessageA(G.hMain, WM_APP_PROGRESS,
                   (WPARAM) G.scanned, (LPARAM) G.total_sec);
  }

  PostMessageA(G.hMain, WM_APP_PROGRESS,
               (WPARAM) G.total_sec, (LPARAM) G.total_sec);

  /*  Auto-fix: mark every new bad sector as 0xFF7 in both FATs.  */
  if (G.autofix && G.bad_n > 0 && G.has_fat) {
    ui_status("Marking bad clusters in FAT...");
    BYTE * fat = (BYTE *) LocalAlloc(LPTR, G.fat_size * SECTOR_SIZE);
    if (fat) {
      if (disk_read(&dh, (DWORD) G.reserved_sec,
                    (WORD) G.fat_size, fat) == 0) {
        for (int i = 0; i < G.bad_n; ++i) {
          int cl = lba_to_cluster(G.bad_lba[i]);
          if (cl >= 2 && cl < G.total_clusters + 2) {
            WORD cur = fat12_get(fat, cl);
            if (cur == 0x000 || cur == 0xFF7)   /*  only free / bad  */
              fat12_set(fat, cl, 0xFF7);
          }
        }
        for (int f = 0; f < G.num_fats; ++f)
          disk_write(&dh, (DWORD) (G.reserved_sec + f * G.fat_size),
                     (WORD) G.fat_size, fat);
      }
      LocalFree(fat);
    }
  }

  {
    char el_s[16];
    fmt_hms(now_ms() - G.t_start, el_s);
    char buf[160];
    wsprintfA(buf,
        "Done.  %lu sectors verified, %lu bad sector(s).  Elapsed %s",
        (unsigned long) G.good_count, (unsigned long) G.bad_count, el_s);
    ui_status(buf);
  }

unlock:     disk_unlock(&dh);
close_disk: disk_close(&dh);
cleanup:
  if (orig) LocalFree(orig);
  if (pat)  LocalFree(pat);
  if (chk)  LocalFree(chk);
  if (bpb)  LocalFree(bpb);
  InterlockedExchange(&G.running, 0);
  PostMessageA(G.hMain, WM_APP_DONE, 0, 0);
  return 0;
}
