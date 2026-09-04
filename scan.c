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

/*  Surface scan and drive diagnostics.  */

static int test_sector(disk_handle * d, DWORD lba,
                       BYTE * orig, BYTE * pat, BYTE * chk) {
  int i, r;
  ui_set_state((int) lba, ST_SCANNING);
  r = disk_read(d, lba, 1, orig);
  if (r != 0) return r;
  if (G.mode == MODE_STANDARD) return 0;
#define W_AND_V()                                         \
    do {                                                  \
      ui_set_state((int) lba, ST_WRITING);                \
      r = disk_write(d, lba, 1, pat);  if (r) goto restore; \
      ui_set_state((int) lba, ST_VERIFY);                 \
      r = disk_read(d, lba, 1, chk);   if (r) goto restore; \
      if (memcmp(pat, chk, SECTOR_SIZE)) {                \
        r = -DOS_ERR_DATA; goto restore;                  \
      }                                                   \
    } while (0)
  /*  Original, inverse, random, and inverse random.  */
  memcpy(pat, orig, SECTOR_SIZE);
  W_AND_V();
  Fi(SECTOR_SIZE, pat[i] = (BYTE) ~orig[i]);
  W_AND_V();
  for (i = 0; i < SECTOR_SIZE; i += 4) {
    DWORD x = rng_next();
    pat[i]     = (BYTE) x;
    pat[i + 1] = (BYTE) (x >> 8);
    pat[i + 2] = (BYTE) (x >> 16);
    pat[i + 3] = (BYTE) (x >> 24);
  }
  W_AND_V();
  Fi(SECTOR_SIZE, pat[i] = (BYTE) ~pat[i]);
  W_AND_V();
  r = 0;
restore:
  /*  Restore after success or failure.  */
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

/*  Mark the boot, FAT, and root sectors.  */
void mark_system_sectors(void) {
  int root_sec = (G.root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
  int last = G.reserved_sec + G.num_fats * G.fat_size + root_sec;
  int i;
  Fi(MIN(last, MAX_SECTORS),
    if (G.state[i] == ST_UNTESTED) G.state[i] = ST_SYSTEM);
}

/*  Load existing bad clusters from the FAT.  */
static void mark_existing_bad(disk_handle * d) {
  BYTE * fat = (BYTE *) LocalAlloc(LPTR, G.fat_size * SECTOR_SIZE);
  int i, n;
  if (!fat) return;
  if (disk_read(d, (DWORD) G.reserved_sec, (WORD) G.fat_size, fat) == 0) {
    for (n = 2; n < G.total_clusters + 2; n++) {
      if (fat12_get(fat, n) != 0xFF7) continue;
      DWORD lba = cluster_to_lba(n);
      Fi(G.sec_per_cluster, ui_set_state((int) lba + i, ST_BAD_OLD));
    }
  }
  LocalFree(fat);
}

static int diag_read_id(disk_handle * dh, int drive, int cyl, int head,
                        BYTE * st0, BYTE * st1, BYTE * st2,
                        BYTE * c_out, BYTE * h_out,
                        BYTE * r_out, BYTE * n_out) {
  fdc_out out;
  (void) dh;
  vxd_seek(drive, cyl, head, &out);
  if (vxd_read_id(drive, head, &out) == 0 && out.result_n >= 7) {
    *st0 = out.st0;  *st1 = out.st1;  *st2 = out.st2;
    *c_out = out.c;  *h_out = out.h;
    *r_out = out.r;  *n_out = out.n;
    return (out.st0 & 0xC0) == 0 ? 0 : 1;
  }
  *st0 = 0x80;   /*  Invalid command: no controller response.  */
  *st1 = *st2 = 0;
  *c_out = *h_out = *r_out = *n_out = 0;
  return 1;
}

static DWORD run_diagnostic(disk_handle * dh) {
  if (!G.vxd) {
    ui_prompt("The drive test needs fdchk.vxd, but the driver did not load.\n\n"
              "fdchk.vxd runs only on Windows 95, 98 and Me. The other tests "
              "do not need it.",
              APP_NAME, MB_OK | MB_ICONWARNING);
    ui_status("Cannot run the drive test: fdchk.vxd did not load.");
    return 0;
  }


  HANDLE log = log_create("fdchk-diag.log");
  int i, cyl, head;
  int wrong_cyl = 0, no_am = 0, ok_reads = 0, stress_fail = 0, stress_n = 0;
  DWORD seek_total = 0, seek_avg10 = 0, rnd_total = 0, rnd_avg10 = 0;
  const DWORD rnd_n = 50;

  /*  Use the physical drive geometry.  */
  BYTE dev_type = disk_probe_drive_type(dh);
  const floppy_geom * dg = geom_for_drive_type(dev_type);
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
      "fdchk drive diagnostic\r\n"
      "drive: %c:   type: %s   method: raw FDC\r\n"
      "layout: %d cylinders x %d heads\r\n"
      "date: %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
      'A' + G.drive, drive_type_str(dev_type), n_cyl, n_head,
      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

  /*  Keep the motor on during the test.  */
  int held = vxd_begin(G.drive);
  if (!held)
    log_write(log, "note: could not keep the motor on; sending one "
                   "command at a time\r\n\r\n");

  {
    fdc_out ms;
    int m = vxd_sense_media(G.drive, &ms);
    LOG_FMT(log,
        "media: %s\r\n"
        "  DIR before=%02x after=%02x (bit 7 = disk change)\r\n"
        "  READ ID  ST=%02x %02x %02x  C=%d H=%d R=%d N=%d  n=%d  status=%02x\r\n\r\n",
        m == MEDIA_PRESENT    ? "disk present, sector IDs readable" :
        m == MEDIA_NONE       ? "no disk in drive" :
        m == MEDIA_UNREADABLE ? "disk present, no readable sector IDs"
                              : "no result",
        ms.dir_before, ms.dir_after,
        ms.st0, ms.st1, ms.st2, ms.c, ms.h, ms.r, ms.n,
        ms.result_n, ms.status);
  }

  /*  One cell per cylinder and head.  */
  G.total_sec = n_cell;
  Fi(n_cell, G.state[i] = ST_UNTESTED);
  SendMessageA(G.main, WM_APP_REPAINT, 0, 0);

  DWORD t_start = now_ms();
  G.t_start = t_start;

  ui_status("[1/5] Resetting controller...");
  DWORD t0 = now_ms();
  {
    fdc_out o;
    vxd_reset(&o);
    LOG_FMT(log, "Reset:        %lums  ST0=%02x  status=%02x\r\n",
            (DWORD) (now_ms() - t0), o.st0, o.status);
  }
  if (G.abort_req) goto done;

  ui_status("[2/5] Recalibrating to track 0...");
  t0 = now_ms();
  {
    fdc_out o;
    vxd_recalibrate(G.drive, &o);
    LOG_FMT(log,
        "Recalibrate:  %lums  ST0=%02x  cur_cyl=%d  status=%02x\r\n",
        (DWORD) (now_ms() - t0), o.st0, o.cur_cyl, o.status);
  }
  if (G.abort_req) goto done;

  /*  Time all reads together because GetTickCount is coarse.  */
  ui_status("[3/5] Reading every track ID...");
  log_write(log,
      "\r\nREAD ID results for every track "
      "(track/head ST0 ST1 ST2 C H R N):\r\n");
  DWORD read_start = now_ms();
  for (cyl = 0; cyl < n_cyl && !G.abort_req; cyl++) {
    for (head = 0; head < n_head && !G.abort_req; head++) {
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
        PostMessageA(G.main, WM_APP_PROGRESS,
                     (WPARAM) G.scanned, (LPARAM) G.total_sec);
      LOG_FMT(log,
          "  %2d/%d  ST=%02x %02x %02x  C=%d H=%d R=%d N=%d  %s\r\n",
          cyl, head, st0, st1, st2, c, h, r, n, st_summary(st0, st1, st2));
    }
  }
  seek_total = now_ms() - read_start;
  seek_avg10 = G.scanned ? (seek_total * 10 / G.scanned) : 0;
  LOG_FMT(log,
      "\r\nsummary: %d good, %d wrong cylinder, %d no address mark, "
      "%lu read errors\r\n"
      "time: %lu ms total, %lu.%lu ms/read\r\n\r\n",
      ok_reads, wrong_cyl, no_am,
      (DWORD) (G.scanned - ok_reads - wrong_cyl - no_am),
      seek_total, seek_avg10 / 10, seek_avg10 % 10);
  if (G.abort_req) goto done;

  /*  Read the innermost track 50 times.  */
  {
    char m[80];
    wsprintfA(m, "[4/5] Reading inner track %d 50 times...", inner);
    ui_status(m);
    LOG_FMT(log, "inner track (50 READ ID passes at track %d):\r\n",
            inner);
  }
  for (i = 0; i < 50 && !G.abort_req; i++) {
    for (head = 0; head < n_head && !G.abort_req; head++) {
      BYTE st0 = 0, st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
      int rc = diag_read_id(dh, G.drive, inner, head,
                            &st0, &st1, &st2, &c, &h, &r, &n);
      stress_n++;
      if (rc || (st0 & 0xC0) || c != inner) stress_fail++;
    }
  }
  LOG_FMT(log, "  %d reads, %d failures (%d%% succeeded)\r\n\r\n",
          stress_n, stress_fail,
          stress_n ? (100 * (stress_n - stress_fail)) / stress_n : 0);

  ui_status("[5/5] Timing 50 random seeks...");
  log_write(log, "random seek timing:\r\n");
  DWORD batch_start = now_ms();
  for (i = 0; i < (int) rnd_n && !G.abort_req; i++) {
    int cyl = (int) (rng_next() % (DWORD) n_cyl);
    fdc_out o;
    vxd_seek(G.drive, cyl, 0, &o);
  }
  rnd_total = now_ms() - batch_start;
  rnd_avg10 = rnd_n ? (rnd_total * 10 / rnd_n) : 0;
  LOG_FMT(log, "  %lu seeks in %lu ms; %lu.%lu ms/seek\r\n\r\n",
          rnd_n, rnd_total, rnd_avg10 / 10, rnd_avg10 % 10);

done:
  if (held) vxd_end();
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);

  /*  Show the results.  */
  {
    DWORD el = now_ms() - t_start;
    char els[16];
    fmt_hms(el, els);
    int read_errors = (int) G.scanned - ok_reads - wrong_cyl - no_am;
    if (read_errors < 0) read_errors = 0;

    char ln[128];
    wsprintfA(ln,
        "Drive test done: %d good, %d wrong cylinder, %d no address mark, "
        "%d read errors (%s)",
        ok_reads, wrong_cyl, no_am, read_errors, els);
    ui_status(ln);

    const char * verdict;
    UINT vicon;
    /*  Set limits from the number of tracks.  */
    int ok_thresh     = n_cell - n_cell / 16;
    int stress_ok     = stress_n / 20;
    int stress_severe = stress_n / 5;
    if (ok_reads >= ok_thresh && wrong_cyl == 0 && no_am == 0 &&
        stress_fail <= stress_ok) {
      verdict = "The drive passed the tests.";
      vicon = MB_ICONINFORMATION;
    } else if (wrong_cyl > 0) {
      verdict = "The head read IDs from the wrong track. The drive may need "
                "alignment or replacement.";
      vicon = MB_ICONWARNING;
    } else if (no_am > 5) {
      verdict = "Many tracks have no address mark. The disk is unformatted "
                "or the drive cannot read it.";
      vicon = MB_ICONWARNING;
    } else if (stress_fail > stress_severe) {
      verdict = "The inner-track test failed often. The head may be failing.";
      vicon = MB_ICONWARNING;
    } else {
      verdict = "Some tracks failed without a clear pattern. Try a disk that "
                "works in another drive.";
      vicon = MB_ICONWARNING;
    }

    char summary[1024];
    wsprintfA(summary,
        "Drive %c: test results\r\n\r\n"
        "Elapsed:        %s\r\n"
        "Track IDs:      %d of %d good\r\n"
        "Wrong cylinder: %d\r\n"
        "No address mark: %d\r\n"
        "READ ID time:   %lu.%lu ms average\r\n"
        "Inner track:    %d of %d reads failed at track %d\r\n"
        "Random seeks:   %lu.%lu ms average\r\n\r\n"
        "%s\r\n\r\nFull log: fdchk-diag.log",
        'A' + G.drive, els, ok_reads, n_cell, wrong_cyl, no_am,
        seek_avg10 / 10, seek_avg10 % 10, stress_fail, stress_n, inner,
        rnd_avg10 / 10, rnd_avg10 % 10, verdict);
    MessageBoxA(G.main, summary, "Drive diagnostic", MB_OK | vicon);
  }
  return 0;
}

DWORD WINAPI scan_thread(LPVOID arg) {
  (void) arg;
  disk_handle dh;
  int i, rc, resynced = 0;
  BYTE * orig = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  BYTE * pat  = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  BYTE * chk  = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  BYTE * bpb  = (BYTE *) LocalAlloc(LPTR, SECTOR_SIZE);
  DWORD lba;
  if (!orig || !pat || !chk || !bpb) goto cleanup;
  if (!disk_open(&dh, G.drive)) {
    ui_status("Cannot open the drive.");
    ui_prompt("Cannot open the floppy drive. Insert a disk and check that "
              "fdchk is running on Windows 95, 98 or Me.",
              APP_NAME, MB_OK | MB_ICONERROR);
    goto cleanup;
  }
  rc = disk_lock(&dh, 1);
  if (rc < 0) {
    char msg[160];
    wsprintfA(msg,
        "Cannot lock drive %c: %s.\n\n"
        "Close programs using the disk and try again.",
        'A' + G.drive, dos_err_str(rc));
    ui_prompt(msg, APP_NAME, MB_OK | MB_ICONWARNING);
    ui_status("Lock failed.");
    goto close_disk;
  }
  G.drive_type[G.drive & 1] = disk_probe_drive_type(&dh);
  ui_drive_labels();
  /*  Check for a disk before starting the test.  */
retry_probe: {
  int present = probe_disk_present(&dh);
  if (present == MEDIA_NONE) {
    if (ui_prompt("No disk in the drive.\n\nInsert one and click Retry.",
                  APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY)
      goto retry_probe;
    goto unlock;
  }
  if (present == MEDIA_UNREADABLE) {
    if (!resynced && disk_resync_media(&dh)) {
      resynced = 1;
      goto retry_probe;
    }
    if (ui_prompt("fdchk could not read any test sector. The disk may be "
                  "blank, formatted for another density, or damaged.\n\n"
                  "A full format can write new sector address marks. "
                  "Continue the scan?",
                  APP_NAME, MB_OKCANCEL | MB_ICONWARNING) != IDOK)
      goto unlock;
  }
}

  if (G.mode == MODE_DIAGNOSTIC) { run_diagnostic(&dh);  goto unlock; }
  if (G.mode == MODE_CHKFS)      { check_filesystem(&dh); goto unlock; }
  ui_status("Reading boot sector...");
  rc = disk_read(&dh, 0, 1, bpb);
  if (rc != 0 && !resynced && disk_resync_media(&dh)) {
    resynced = 1;
    rc = disk_read(&dh, 0, 1, bpb);
  }
  if (rc != 0 || !parse_bpb(bpb)) {
    /*  Use the drive geometry and skip FAT access.  */
    const floppy_geom * g =
        geom_for_drive_type(G.drive_type[G.drive & 1]);
    geom_apply(g ? g : geom_for_size(2880));
    lstrcpyA(G.fs_type, "RAW");
    G.vol_label[0] = 0;
    G.has_fat = 0;
    SendMessageA(G.statusbar, SB_SETTEXTA, 1, (LPARAM) "RAW");
  } else {
    G.has_fat = 1;
    SendMessageA(G.statusbar, SB_SETTEXTA, 1, (LPARAM) G.fs_type);
  }

  SendMessageA(G.main, WM_APP_REPAINT, 0, 0);

  Fi(MAX_SECTORS, G.state[i] = ST_UNTESTED);
  if (G.has_fat) {
    mark_system_sectors();
    mark_existing_bad(&dh);
  }
  G.t_start = now_ms();
  rng_seed(G.t_start ^ 0xCAFEBABEu);

  for (lba = 0; lba < (DWORD) G.total_sec; lba++) {
    if (G.abort_req) goto unlock;
    if (G.state[lba] == ST_BAD_OLD) continue;
    G.current_sec = (int) lba;

    int attempts = 0, last_err = 0;
  retry_sector:
    attempts++;
    last_err = test_sector(&dh, lba, orig, pat, chk);
    if (last_err == 0) {
      ui_set_state((int) lba, ST_GOOD);
      G.good_count++;
    } else {
      int e = -last_err;
      /*  Ask for the original media after a disk change.  */
      if (e == DOS_ERR_CHANGE_LINE) {
        if (attempts < 3) { Sleep(200); goto retry_sector; }
        if (ui_prompt("The disk changed. Reinsert the original and click "
                      "Retry.",
                      APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY) {
          attempts = 0;
          goto retry_sector;
        }
        goto unlock;
      }
      if (e == DOS_ERR_WRITE_PROTECT) {
        if (ui_prompt("The disk is write-protected. Remove write protection "
                      "and click Retry.",
                      APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY) {
          attempts = 0;
          goto retry_sector;
        }
        goto unlock;
      }
      if (e == DOS_ERR_TIMEOUT) {
        if (ui_prompt("The drive is not ready. Insert a disk and click Retry.",
                      APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY) {
          attempts = 0;
          goto retry_sector;
        }
        goto unlock;
      }
      /*  Retry transient controller errors.  */
      if ((e == DOS_ERR_DMA_OVERRUN || e == DOS_ERR_CTRL_FAIL ||
           e == DOS_ERR_SEEK_FAIL) && attempts < 3) {
        Sleep(100);
        goto retry_sector;
      }
      /*  Mark every other failure as a bad sector.  */
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
      PostMessageA(G.main, WM_APP_PROGRESS,
                   (WPARAM) G.scanned, (LPARAM) G.total_sec);
  }

  PostMessageA(G.main, WM_APP_PROGRESS,
               (WPARAM) G.total_sec, (LPARAM) G.total_sec);

  /*  Write new bad clusters to every FAT.  */
  if (G.auto_fix && G.bad_n > 0 && G.has_fat) {
    ui_status("Marking bad clusters in FAT...");
    BYTE * fat = (BYTE *) LocalAlloc(LPTR, G.fat_size * SECTOR_SIZE);
    if (fat) {
      if (disk_read(&dh, (DWORD) G.reserved_sec,
                    (WORD) G.fat_size, fat) == 0) {
        Fi(G.bad_n,
          int cl = lba_to_cluster(G.bad_lba[i]);
          if (cl >= 2 && cl < G.total_clusters + 2) {
            WORD cur = fat12_get(fat, cl);
            if (cur == 0x000 || cur == 0xFF7)
              fat12_set(fat, cl, 0xFF7);
          }
        );
        Fi(G.num_fats,
          disk_write(&dh, (DWORD) (G.reserved_sec + i * G.fat_size),
                     (WORD) G.fat_size, fat);
        );
      }
      LocalFree(fat);
    }
  }

  {
    char el_s[16];
    fmt_hms(now_ms() - G.t_start, el_s);
    char buf[160];
    wsprintfA(buf,
        "Done: %lu sectors checked, %lu bad, %s elapsed",
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
  PostMessageA(G.main, WM_APP_DONE, 0, 0);
  return 0;
}
