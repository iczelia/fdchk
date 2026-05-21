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

/*  Extract the embedded VxD blob to a temp file so VxDLDR/VMM can find it
    by path.  The path is stashed in G.vxd_tmp_path.  1 on success.  */
static int vxd_extract(void) {
  HRSRC res = FindResourceA(G.hInst, "FDCHKVXD", MAKEINTRESOURCEA(10));
  if (!res) return 0;
  HGLOBAL hg = LoadResource(G.hInst, res);
  if (!hg) return 0;
  void * data = LockResource(hg);
  DWORD size  = SizeofResource(G.hInst, res);
  if (!data || !size) return 0;

  char tmp[MAX_PATH];
  if (GetTempPathA(MAX_PATH, tmp) == 0) lstrcpyA(tmp, ".\\");
  wsprintfA(G.vxd_tmp_path, "%sfdchk.vxd", tmp);

  HANDLE f = CreateFileA(G.vxd_tmp_path, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return 0;
  DWORD cb;
  BOOL ok = WriteFile(f, data, size, &cb, NULL) && cb == size;
  CloseHandle(f);
  return ok ? 1 : 0;
}

/*  Open the VxD.  Returns 1 on success, 0 if it is unavailable.  */
int vxd_open(void) {
  G.vxd_tmp_path[0] = 0;
  if (!vxd_extract()) return 0;

  /*  CreateFile on the dropped path triggers a Win9x dynamic VxD load.  */
  G.hVxd = CreateFileA(G.vxd_tmp_path, 0, 0, NULL, OPEN_EXISTING,
                       FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if (G.hVxd == INVALID_HANDLE_VALUE) {
    /*  Fall back to the VxD namespace in case the loader is picky.  */
    G.hVxd = CreateFileA("\\\\.\\fdchk.vxd", 0, 0, NULL, OPEN_EXISTING,
                         FILE_FLAG_DELETE_ON_CLOSE, NULL);
  }
  if (G.hVxd == INVALID_HANDLE_VALUE) {
    G.hVxd = NULL;
    if (G.vxd_tmp_path[0]) DeleteFileA(G.vxd_tmp_path);
    return 0;
  }
  return 1;
}

void vxd_close(void) {
  if (G.hVxd && G.hVxd != INVALID_HANDLE_VALUE) {
    CloseHandle(G.hVxd);
    G.hVxd = NULL;
  }
  if (G.vxd_tmp_path[0]) {
    DeleteFileA(G.vxd_tmp_path);
    G.vxd_tmp_path[0] = 0;
  }
}

/*  Send an IOCTL to the VxD.  1 on success, 0 on failure.  */
int vxd_call(DWORD ioctl, const FdcIn * in_buf, FdcOut * out_buf) {
  DWORD cb = 0;
  if (!G.hVxd || G.hVxd == INVALID_HANDLE_VALUE) return 0;
  return DeviceIoControl(G.hVxd, ioctl,
      (LPVOID) in_buf, in_buf ? sizeof(FdcIn) : 0,
      out_buf, out_buf ? sizeof(FdcOut) : 0, &cb, NULL) ? 1 : 0;
}

/*  Decode an FDC ST byte triplet into a short summary string.  */
const char * st_summary(BYTE st0, BYTE st1, BYTE st2) {
  if (st2 & 0x10)            return "WRONG CYLINDER (head misaligned)";
  if (st2 & 0x20)            return "CRC error in data field";
  if (st1 & 0x20)            return "CRC error in ID field";
  if (st1 & 0x01)            return "missing ID address mark";
  if (st2 & 0x01)            return "missing data address mark";
  if (st1 & 0x04)            return "no data (track unformatted?)";
  if (st1 & 0x02)            return "not writable";
  if (st1 & 0x10)            return "DMA over-run";
  if (st1 & 0x80)            return "end of cylinder";
  if (st0 & 0x10)            return "equipment check / drive fault";
  if (st0 & 0x08)            return "not ready";
  if ((st0 & 0xC0) == 0x40)  return "abnormal termination";
  if ((st0 & 0xC0) == 0x80)  return "invalid command";
  if ((st0 & 0xC0) == 0xC0)  return "polling-state change";
  return "OK";
}
