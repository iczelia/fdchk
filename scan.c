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

/*  Surface test for a single sector.
      orig - holds the original contents on entry/exit
      pat  - scratch: the write-pattern source
      chk  - scratch: the read-back buffer
    Returns 0 on success, -DOS_ERR_* on error (orig restored best-effort).  */

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

/*  Diagnostic mode - probes the FDC, head and seek mechanism.  Uses the
    VxD for true ST0/ST1/ST2 when present, else BIOS Int 13h (AH-decoded).
    The state map is repurposed as 160 cells: 80 cylinders x 2 heads,
    indexed cyl*2 + head.  */

/*  Issue READ ID at (cyl, head); fill the ST triplet and C/H/R/N.  */
static int diag_read_id(DiskHandle * dh, int drive, int cyl, int head,
                        BYTE * st0, BYTE * st1, BYTE * st2,
                        BYTE * c_out, BYTE * h_out,
                        BYTE * r_out, BYTE * n_out) {
  if (G.hVxd) {
    FdcIn in_buf;
    FdcOut out;
    memzero(&in_buf, sizeof in_buf);
    in_buf.drive = (BYTE) drive;
    in_buf.head  = (BYTE) head;
    in_buf.motor = 1;
    in_buf.cyl   = (BYTE) cyl;
    vxd_call(IOCTL_FDC_SEEK, &in_buf, &out);
    if (vxd_call(IOCTL_FDC_READID, &in_buf, &out) && out.result_n >= 7) {
      *st0 = out.st0; *st1 = out.st1; *st2 = out.st2;
      *c_out = out.c; *h_out = out.h;
      *r_out = out.r; *n_out = out.n;
      return ((out.st0 & 0xC0) == 0) ? 0 : 1;
    }
    /*  VxD call failed - fall through to BIOS.  */
  }
  /*  BIOS Int 13h AH=02: read 1 sector; AH digests the result phase.  */
  BYTE buf[SECTOR_SIZE], ah = 0;
  int rc = bios_int13(dh, 0x02, 1, cyl, head, 1, buf, &ah);
  bios_ah_to_st(ah, st0, st1, st2);
  *c_out = (BYTE) cyl;  *h_out = (BYTE) head;
  *r_out = 1;           *n_out = 2;
  return rc ? 1 : 0;
}

static DWORD diag_worker(DiskHandle * dh) {
  char log_full[MAX_PATH];
  log_path(log_full, "fdchk-diag.log");
  HANDLE log = CreateFileA(log_full, GENERIC_WRITE, 0, NULL,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

  /*  Counters at function scope so a goto-skip can't leave them unset.  */
  int wrong_cyl = 0, no_am = 0, ok_reads = 0, stress_fail = 0;
  DWORD seek_total = 0, seek_avg10 = 0, rnd_total = 0, rnd_avg10 = 0;
  const DWORD rnd_n = 50;

  SYSTEMTIME t;
  GetLocalTime(&t);
  LOG_FMT(log,
      "fdchk Drive Diagnostic Log\r\n"
      "Drive: %c:   VxD: %s\r\n"
      "Date:  %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
      'A' + G.drive,
      G.hVxd ? "loaded (raw FDC, true ST0/ST1/ST2)"
             : "NOT loaded (BIOS Int 13h, AH-decoded)",
      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

  /*  Repurpose the state map as 160 cells.  */
  G.total_sec = 160;
  for (int i = 0; i < 160; ++i) G.state[i] = ST_UNTESTED;
  SendMessageA(G.hMain, WM_APP_REPAINT, 0, 0);

  DWORD t_start = now_ms();
  G.t_start = t_start;

  /*  Test 1: reset.  */
  ui_status("[1/5] Reset controller...");
  DWORD t0 = now_ms();
  if (G.hVxd) {
    FdcOut o;
    vxd_call(IOCTL_FDC_RESET, NULL, &o);
    LOG_FMT(log, "Reset:        %lums  ST0=%02x  status=%02x\r\n",
            (DWORD) (now_ms() - t0), o.st0, o.status);
  } else {
    BYTE ah;
    bios_int13(dh, 0x00, 0, 0, 0, 0, NULL, &ah);
    LOG_FMT(log, "Reset:        %lums  BIOS AH=%02x  %s\r\n",
            (DWORD) (now_ms() - t0), ah, bios_ah_str(ah));
  }
  if (G.abort_req) goto done;

  /*  Test 2: recalibrate.  */
  ui_status("[2/5] Recalibrate (seek to track 0)...");
  t0 = now_ms();
  if (G.hVxd) {
    FdcIn in_buf;
    FdcOut o;
    memzero(&in_buf, sizeof in_buf);
    in_buf.drive = (BYTE) G.drive;
    vxd_call(IOCTL_FDC_RECAL, &in_buf, &o);
    LOG_FMT(log,
        "Recalibrate:  %lums  ST0=%02x  cur_cyl=%d  status=%02x\r\n",
        (DWORD) (now_ms() - t0), o.st0, o.cur_cyl, o.status);
  } else {
    BYTE ah;
    bios_int13(dh, 0x11, 0, 0, 0, 0, NULL, &ah);
    LOG_FMT(log, "Recalibrate:  %lums  BIOS AH=%02x  %s\r\n",
            (DWORD) (now_ms() - t0), ah, bios_ah_str(ah));
  }
  if (G.abort_req) goto done;

  /*  Tests 3 & 4: READ ID at every (cyl, head).  GetTickCount is too
      coarse to time a single seek, so time the whole sweep.  */
  ui_status("[3/5] Reading sector IDs from every track on both heads...");
  log_write(log,
      "\r\nREAD ID sweep (track, head, ST0, ST1, ST2, C, H, R, N):\r\n");
  DWORD sweep_start = now_ms();
  for (int cyl = 0; cyl < 80 && !G.abort_req; ++cyl) {
    for (int head = 0; head < 2 && !G.abort_req; ++head) {
      int idx = cyl * 2 + head;
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
  ui_status("[4/5] Innermost-track READ ID stress (track 79)...");
  log_write(log, "Innermost-track stress (50x READ ID at track 79):\r\n");
  for (int i = 0; i < 50 && !G.abort_req; ++i) {
    for (int head = 0; head < 2 && !G.abort_req; ++head) {
      BYTE st0 = 0, st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
      int rc = diag_read_id(dh, G.drive, 79, head,
                            &st0, &st1, &st2, &c, &h, &r, &n);
      if (rc || (st0 & 0xC0) || c != 79) stress_fail++;
    }
  }
  LOG_FMT(log, "  100 reads, %d failures (%d%% reliability)\r\n\r\n",
          stress_fail, (100 * (100 - stress_fail)) / 100);

  /*  Test 6: random seek timing.  Batch-timed for the same reason.  */
  ui_status("[5/5] Random seek timing (50 jumps)...");
  log_write(log, "Random seek timing:\r\n");
  DWORD batch_start = now_ms();
  for (DWORD i = 0; i < rnd_n && !G.abort_req; ++i) {
    int cyl = (int) (rng_next() % 80);
    if (G.hVxd) {
      FdcIn in_buf;
      FdcOut o;
      memzero(&in_buf, sizeof in_buf);
      in_buf.drive = (BYTE) G.drive;
      in_buf.cyl   = (BYTE) cyl;
      vxd_call(IOCTL_FDC_SEEK, &in_buf, &o);
    } else {
      BYTE ah;
      bios_int13(dh, 0x0C, 0, cyl, 0, 0, NULL, &ah);
    }
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
    if (ok_reads >= 150 && wrong_cyl == 0 && no_am == 0 && stress_fail < 5) {
      verdict = "Drive looks healthy.";
      vicon = MB_ICONINFORMATION;
    } else if (wrong_cyl > 0) {
      verdict = "Wrong-Cylinder failures detected - the head is reading "
                "IDs from the wrong track.  The classic head-alignment "
                "symptom; the drive may need re-alignment or replacement.";
      vicon = MB_ICONWARNING;
    } else if (no_am > 5) {
      verdict = "Many tracks return no address mark.  The disk may be "
                "unformatted, or the drive is failing to read those "
                "tracks at all.";
      vicon = MB_ICONWARNING;
    } else if (stress_fail > 20) {
      verdict = "Innermost-track stress failed often.  The head is "
                "degraded - innermost tracks fail first as the magnetic "
                "field weakens.";
      vicon = MB_ICONWARNING;
    } else {
      verdict = "Some tracks failed but with no clear pattern.  Try a "
                "known-good disk to rule out media-only damage.";
      vicon = MB_ICONWARNING;
    }

    char summary[1024];
    wsprintfA(summary,
        "Drive Diagnostic Results (drive %c:)\r\n\r\n"
        "Phase 1 - Reset:        ran in elapsed %s\r\n"
        "Phase 2 - Recalibrate:  issued\r\n"
        "Phase 3 - Track sweep:  %d tracks reachable on both heads\r\n"
        "                        %d wrong-cylinder failures\r\n"
        "                        %d no-address-mark failures\r\n"
        "                        %lu.%lu ms avg per READ ID\r\n"
        "Phase 4 - Inner stress: %d of 100 reads failed at track 79\r\n"
        "Phase 5 - Random seeks: %lu.%lu ms avg per seek\r\n\r\n"
        "%s\r\n\r\nFull log: fdchk-diag.log",
        'A' + G.drive, els, ok_reads, wrong_cyl, no_am,
        seek_avg10 / 10, seek_avg10 % 10, stress_fail,
        rnd_avg10 / 10, rnd_avg10 % 10, verdict);
    MessageBoxA(G.hMain, summary, "Drive Diagnostic", MB_OK | vicon);
  }
  return 0;
}

/*  Surface-scan worker thread.  */

DWORD WINAPI worker_proc(LPVOID arg) {
  (void) arg;
  DiskHandle dh;
  int rc;

  /*  Heap-allocate the per-sector buffers so the worker's stack frame
      stays well under 4 KB.  */
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

  /*  Probe before dispatch so every mode gets the same media detection.  */
retry_probe: {
  int present = probe_disk_present(&dh);
  if (present == 0) {
    if (ui_prompt("No disk in the drive.\n\n"
                  "Insert a floppy and click Retry, or Cancel to abort.",
                  APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY)
      goto retry_probe;
    goto unlock;
  }
  if (present < 0) {
    if (ui_prompt("Disk is unreadable at every position probed.\n\n"
                  "It is most likely physically damaged or fully\n"
                  "demagnetised.  Continue scanning anyway?",
                  APP_NAME, MB_OKCANCEL | MB_ICONWARNING) != IDOK)
      goto unlock;
  }
}

  if (G.mode == MODE_DIAGNOSTIC) { diag_worker(&dh);  goto unlock; }
  if (G.mode == MODE_CHKFS)      { chkfs_worker(&dh); goto unlock; }

  /*  Read the BPB.  Media is confirmed present; a failure here means the
      boot sector alone is unreadable.  */
  ui_status("Reading boot sector / BPB...");
  rc = disk_read(&dh, 0, 1, bpb);
  if (rc != 0 || !parse_bpb(bpb)) {
    /*  Fall back to 1.44 MB geometry, filesystem unknown.  */
    G.bytes_per_sec = SECTOR_SIZE;  G.sec_per_cluster = 1;
    G.reserved_sec = 1;             G.num_fats = 2;
    G.fat_size = 9;                 G.root_entries = 224;
    G.total_sec = 2880;             G.data_start_sec = 33;
    G.total_clusters = 2847;        G.media_byte = 0xF0;
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

unlock:
  disk_unlock(&dh);
close_disk:
  disk_close(&dh);
cleanup:
  if (orig) LocalFree(orig);
  if (pat)  LocalFree(pat);
  if (chk)  LocalFree(chk);
  if (bpb)  LocalFree(bpb);
  InterlockedExchange(&G.running, 0);
  PostMessageA(G.hMain, WM_APP_DONE, 0, 0);
  return 0;
}
