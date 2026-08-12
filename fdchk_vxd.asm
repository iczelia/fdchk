;  fdchk -- Copyright (C) 2026 Kamila Szewczyk
;  SPDX-License-Identifier: GPL-3.0-only
;
;  This program is free software; you can redistribute it and/or modify
;  it under the terms of version 3 of the GNU General Public License as
;  published by the Free Software Foundation.  Version 3 is the only
;  version of that license that applies to this program.
;
;  This program is distributed in the hope that it will be useful,
;  but WITHOUT ANY WARRANTY; without even the implied warranty of
;  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;  GNU General Public License for more details.
;
;  You should have received a copy of the GNU General Public License
;  along with this program. If not, see <http://www.gnu.org/licenses/>.
;
;  Routines for raw NEC uPD765 floppy-controller access for Windows 9x.
;  This VxD builds the LE container by hand, as seen below. One fixup
;  is necessary: the DDB.Control_Proc pointer at image offset 24. Fully
;  asynchronous.

bits 32
cpu  486
org  0

%define LE_OFF       0x40          ; LE header file offset (MZ e_lfanew)
%define IMAGE_OFF    0x1000        ; VxD image file offset
%define IMAGE_ENTRY  0x80          ; control_proc within the image
%define PAGE_SIZE    0x1000
%define PAGE_COUNT   4

;  MZ / DOS stub.
mz_start:
  dw      0x5A4D                   ; 'MZ'
  dw      0x40                     ; bytes on last page
  dw      1                        ; pages in file
  dw      0                        ; relocations
  dw      4                        ; header size in paragraphs
  dw      0                        ; min alloc
  dw      0xFFFF                   ; max alloc
  dw      0                        ; ss
  dw      0xB8                     ; sp
  dw      0                        ; checksum
  dw      0                        ; ip
  dw      0                        ; cs
  dw      0x40                     ; relocation table offset
  dw      0                        ; overlay number
  times   0x3C - ($ - mz_start) db 0
  dd      LE_OFF                   ; e_lfanew

;  LE header.  Four 4 KB objects and pages, one fixup record.
le_start:
  dw      0x454C                   ; 'LE'
  db      0                        ; byte order
  db      0                        ; word order
  dd      0                        ; format level
  dw      3                        ; cpu type = 80386
  dw      4                        ; os type = Windows VxD
  dd      0                        ; module version
  dd      0x00038000               ; module flags, stolen from LOGGER.VXD
  dd      PAGE_COUNT               ; page count
  dd      0                        ; EIP object number
  dd      0                        ; EIP
  dd      0                        ; ESP object number
  dd      0                        ; ESP
  dd      PAGE_SIZE                ; page size
  dd      PAGE_SIZE                ; bytes on last page
  dd      fixup_end - fixup_pages  ; fixup section size
  dd      0                        ; fixup section checksum
  dd      fixup_pages - obj_table  ; loader section size
  dd      0                        ; loader section checksum
  dd      obj_table   - le_start   ; object table offset
  dd      4                        ; object count
  dd      page_map    - le_start   ; object page map offset
  dd      0                        ; iterated-data map offset
  dd      0                        ; resource table offset
  dd      0                        ; resource count
  dd      res_names   - le_start   ; resident name table offset
  dd      entry_table - le_start   ; entry table offset
  dd      0                        ; module directives offset
  dd      0                        ; module directive count
  dd      fixup_pages - le_start   ; fixup page table offset
  dd      fixup_recs  - le_start   ; fixup record table offset
  dd      loader_end  - le_start   ; imported module table offset
  dd      0                        ; imported module count
  dd      loader_end  - le_start   ; imported procedure table offset
  dd      0                        ; per-page checksum offset
  dd      IMAGE_OFF                ; data pages file offset
  dd      1                        ; preload page count
  dd      0                        ; non-resident name table offset
  dd      0                        ; non-resident name table length
  dd      0                        ; non-resident name table checksum
  dd      0                        ; auto data-segment object
  dd      0                        ; debug info offset
  dd      0                        ; debug info length
  dd      0                        ; preload-only page count
  dd      0                        ; demand-load page count
  dd      0                        ; extra heap allocation
  times   0xB8 - ($ - le_start) db 0
  dd      0                        ; VxD version resource offset
  dd      0                        ; VxD version resource length
  dw      0                        ; VxD device id = UNDEFINED
  dw      0x0400                   ; VxD DDK version

;  Loader section: object table, page map, names, entry + fixup tables.
obj_table:
  ;  1: LCOD - locked code, read|write|exec|preload|big
  dd      PAGE_SIZE, 0, 0x00002047, 1, 1
  db      'LCOD'
  ;  2: RARE - rarely used code
  dd      PAGE_SIZE, 0, 0x00002005, 2, 1
  db      'RARE'
  ;  3: W32  - Win32 service code
  dd      PAGE_SIZE, 0, 0x00002005, 3, 1
  db      'W32', 0
  ;  4: ICOD - init code, discardable
  dd      PAGE_SIZE, 0, 0x00002015, 4, 1
  db      'ICOD'

page_map:
  db      0x00, 0x00, 0x01, 0x00   ; page 1 (big-endian 24-bit), valid
  db      0x00, 0x00, 0x02, 0x00   ; page 2
  db      0x00, 0x00, 0x03, 0x00   ; page 3
  db      0x00, 0x00, 0x04, 0x00   ; page 4

res_names:
  db      5, 'FDCHK'               ; module name
  dw      0                        ; ordinal
  db      0                        ; end of table

;  Entry table.  Ordinal 1 exports the DDB, necessary for the VMM locating
;  it when loading the module.
entry_table:
  db      1                        ; one entry in this bundle
  db      3                        ; bundle type: 32-bit entry
  dw      1                        ; object number (1-based)
  db      0x03                     ; flags: exported, shared data
  dd      0                        ; offset of ddb_start within the object
  db      0                        ; end of entry table

;  One entry per page plus a terminator.  Page 1 owns the only record; the
;  empty pages start where it ends, so their ranges are zero-length.
fixup_pages:
  dd      0                        ; page 1: first record
  dd      fixup_end - fixup_recs   ; page 2: none
  dd      fixup_end - fixup_recs   ; page 3: none
  dd      fixup_end - fixup_recs   ; page 4: none
  dd      fixup_end - fixup_recs   ; end-of-records pointer
fixup_recs:
  db      0x07                     ; source type: 32-bit offset
  db      0x00                     ; flags: internal ref, 8-bit obj
  dw      24                       ; source offset (DDB.Control_Proc)
  db      1                        ; target object (1-based)
  dw      IMAGE_ENTRY              ; target offset (control_proc)
fixup_end:
loader_end:

  times   IMAGE_OFF - ($ - mz_start) db 0

;  VxD image starts here.

;  VMM message codes
%define Sys_Dynamic_Device_Init    0x001B
%define Sys_Dynamic_Device_Exit    0x001C
%define W32_DEVICEIOCONTROL        0x0023

;  DIOC_PARAMS layout (ESI on W32_DEVICEIOCONTROL)
%define DIOC_dwIoControlCode       0x0C
%define DIOC_lpvInBuffer           0x10
%define DIOC_cbInBuffer            0x14
%define DIOC_lpvOutBuffer          0x18
%define DIOC_cbOutBuffer           0x1C
%define DIOC_lpcbBytesReturned     0x20

;  Driver's IOCTL codes.
%define DIOC_GETVERSION            0      ; asked by the loader
%define IOCTL_FDC_RESET            0x0080
%define IOCTL_FDC_RECAL            0x0082
%define IOCTL_FDC_SEEK             0x0083
%define IOCTL_FDC_READID           0x0084
%define IOCTL_FDC_POLL             0x0085 ; command finished yet?
%define IOCTL_FDC_DIR              0x0086 ; sample the disk-change line
%define IOCTL_FDC_FORMAT           0x0087
%define IOCTL_FDC_CMOS             0x0088
%define IOCTL_FDC_RESULT           0x0089 ; collect the result phase
%define IOCTL_FDC_MOTOR            0x008A
%define IOCTL_FDC_SPECIFY          0x008B
%define IOCTL_FDC_END              0x008C ; hand the controller back
%define IOCTL_FDC_IDENT            0x008E

;  FDC ports
%define FDC_DOR                    0x3F2
%define FDC_MSR                    0x3F4
%define FDC_FIFO                   0x3F5
%define CMOS_INDEX                 0x70
%define CMOS_DATA                  0x71
%define FDC_DIR                    0x3F7   ; read:  bit 7 = disk change
%define FDC_CCR                    0x3F7   ; write: data rate select

;  FDC commands
%define FDC_CMD_SPECIFY            0x03
%define FDC_CMD_RECAL              0x07
%define FDC_CMD_SENSE_INT          0x08
%define FDC_CMD_SEEK               0x0F
%define FDC_CMD_READ_ID            0x4A   ; READ ID + MFM
%define FDC_CMD_FORMAT             0x4D   ; FORMAT TRACK + MFM

;  FdcIn  (user input buffer, EBP in dispatch)
%define FdcIn_drive                0
%define FdcIn_head                 1
%define FdcIn_cyl                  2
%define FdcIn_sec                  3      ; FORMAT: sectors per track
%define FdcIn_size_code            4      ; FORMAT: N (2 = 512 bytes)
%define FdcIn_gpl                  5      ; FORMAT: gap length
%define FdcIn_filler               6      ; FORMAT: data-field fill byte
%define FdcIn_rate                 7      ; CCR data rate
%define FdcIn_spec1                8      ; SPECIFY SRT/HUT
%define FdcIn_spec2                9      ; SPECIFY HLT/ND
%define FdcIn_flags                10     ; MOTOR: bit 0 spindle, bit 1 gate
                                          ; POLL:  bit 0 sense the interrupt
%define FdcIn_total_size           11

;  FdcOut (user output buffer, EDI in FDC routines)
%define FdcOut_status              0
%define FdcOut_st0                 1      ; ST0,ST1,ST2,C,H,R,N follow: 1..7,
%define FdcOut_cur_cyl             8
%define FdcOut_result_n            9
%define FdcOut_dir_before          10
%define FdcOut_dir_after           11
%define FdcOut_stage               12
%define FdcOut_msr                 13
%define FdcOut_cmos                14
%define FdcOut_total_size          16

;  FdcOut_status values that are not an error.
%define FDC_ST_OK                  0x00
%define FDC_ST_BUSY                0x01   ; ask again in a moment

;  Interface version, reported by IOCTL_FDC_IDENT.
%define FDC_VXD_VERSION            0x0200

;  The DDB
ddb_start:
  dd      0                        ;  0 DDB_Next (set by VMM)
  dw      0x0400                   ;  4 DDB_SDK_Version
  dw      0                        ;  6 DDB_Req_Device_Number = UNDEFINED
  db      1                        ;  8 DDB_Dev_Major_Version
  db      0                        ;  9 DDB_Dev_Minor_Version
  dw      0                        ; 10 DDB_Flags
  db      'FDCHK   '               ; 12 DDB_Name (8 bytes)
  dd      0x80000000               ; 20 DDB_Init_Order
  dd      0                        ; 24 DDB_Control_Proc (loader fixes up)
  dd      0                        ; 28 DDB_V86_API_Proc
  dd      0                        ; 32 DDB_PM_API_Proc
  dd      0                        ; 36 DDB_V86_API_CSIP
  dd      0                        ; 40 DDB_PM_API_CSIP
  dd      0                        ; 44 DDB_Reference_Data
  dd      0                        ; 48 DDB_Service_Table_Ptr
  dd      0                        ; 52 DDB_Service_Table_Size
  dd      0                        ; 56 DDB_Win32_Service_Table
  dd      0x50726576               ; 60 DDB_Prev      = 'Prev'
  dd      0x00000050               ; 64 DDB_Reserved0 = 'P'
  dd      0x52737631               ; 68 DDB_Reserved1 = 'Rsv1'
  dd      0x52737632               ; 72 DDB_Reserved2 = 'Rsv2'
  dd      0x52737633               ; 76 DDB_Reserved3 = 'Rsv3'
  times   0x80 - ($ - ddb_start) db 0

;  Entry point.
control_proc:
  cmp     eax, Sys_Dynamic_Device_Init
  je      .ok
  cmp     eax, Sys_Dynamic_Device_Exit
  je      .exit
  cmp     eax, W32_DEVICEIOCONTROL
  je      do_ioctl
  ; any other message: succeed and ignore
.ok:
  clc
  ret
.exit:
  ;  Unloaded, possibly because the owning process died mid-scan.  Leave
  ;  the controller the way the system's own driver expects it.
  call    fdc_release
  clc
  ret

;  do_ioctl: ESI = DIOC_PARAMS*, EBX = VM handle.  EAX = 0 on success.
do_ioctl:
  push    ebp
  push    ebx
  push    edi
  push    esi

  mov     edi, [esi + DIOC_lpvOutBuffer]
  ; zero the FdcOut struct if the user supplied an out-buffer
  test    edi, edi
  jz      .no_zero
  push    edi
  push    ecx
  push    eax
  mov     ecx, [esi + DIOC_cbOutBuffer]     ; never overrun
  cmp     ecx, FdcOut_total_size
  jbe     .zero
  mov     ecx, FdcOut_total_size
.zero:
  xor     al, al
  cld
  rep     stosb
  pop     eax
  pop     ecx
  pop     edi
.no_zero:
  mov     eax, [esi + DIOC_dwIoControlCode]

  cmp     eax, DIOC_GETVERSION
  je      .ioc_version
  cmp     dword [esi + DIOC_cbOutBuffer], FdcOut_total_size
  jb      .out_inval
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .len_ok
  cmp     dword [esi + DIOC_cbInBuffer], FdcIn_total_size
  jb      .out_inval
.len_ok:
  cmp     eax, IOCTL_FDC_CMOS
  je      .ioc_cmos
  cmp     eax, IOCTL_FDC_IDENT
  je      .ioc_ident
  cmp     eax, IOCTL_FDC_RESET
  je      .ioc_reset
  cmp     eax, IOCTL_FDC_END
  je      .ioc_end
  cmp     eax, IOCTL_FDC_MOTOR
  je      .ioc_motor
  cmp     eax, IOCTL_FDC_SPECIFY
  je      .ioc_specify
  cmp     eax, IOCTL_FDC_RECAL
  je      .ioc_recal
  cmp     eax, IOCTL_FDC_SEEK
  je      .ioc_seek
  cmp     eax, IOCTL_FDC_READID
  je      .ioc_readid
  cmp     eax, IOCTL_FDC_POLL
  je      .ioc_poll
  cmp     eax, IOCTL_FDC_RESULT
  je      .ioc_result
  cmp     eax, IOCTL_FDC_DIR
  je      .ioc_dir
  cmp     eax, IOCTL_FDC_FORMAT
  je      .ioc_format

  mov     eax, 0x32                ; ERROR_NOT_SUPPORTED
  jmp     .out

.ioc_version:
  test    edi, edi
  jz      .ok
  mov     word [edi], FDC_VXD_VERSION
  jmp     .ok

;  IOCTL_FDC_RESET
.ioc_reset:
  test    edi, edi
  jz      .out_inval
  call    fdc_reset
  jmp     .ok

;  IOCTL_FDC_END: motors off, interrupt and DMA gate open again.
.ioc_end:
  test    edi, edi
  jz      .out_inval
  call    fdc_release
  jmp     .ok

;  IOCTL_FDC_MOTOR (in: drive, flags)
.ioc_motor:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  movzx   eax, byte [ebp + FdcIn_drive]
  movzx   ebx, byte [ebp + FdcIn_flags]
  call    fdc_set_dor
  jmp     .ok

;  IOCTL_FDC_SPECIFY (in: rate, spec1, spec2)
.ioc_specify:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  call    fdc_specify
  jmp     .ok

;  IOCTL_FDC_RECAL (in: drive)
.ioc_recal:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  movzx   eax, byte [ebp + FdcIn_drive]
  call    fdc_issue_recal
  jmp     .ok

;  IOCTL_FDC_SEEK (in: drive, head, cyl)
.ioc_seek:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  movzx   eax, byte [ebp + FdcIn_drive]
  movzx   ebx, byte [ebp + FdcIn_head]
  movzx   ecx, byte [ebp + FdcIn_cyl]
  call    fdc_issue_seek
  jmp     .ok

;  IOCTL_FDC_READID (in: drive, head)
.ioc_readid:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  movzx   eax, byte [ebp + FdcIn_drive]
  movzx   ebx, byte [ebp + FdcIn_head]
  call    fdc_issue_read_id
  jmp     .ok

;  IOCTL_FDC_POLL (in: drive, flags bit 0 = sense the interrupt when idle)
.ioc_poll:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  call    fdc_poll
  jmp     .ok

;  IOCTL_FDC_RESULT: take the result phase if the controller is offering it.
.ioc_result:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  call    fdc_result
  jmp     .ok

;  IOCTL_FDC_DIR: sample the disk-change line.
.ioc_dir:
  test    edi, edi
  jz      .out_inval
  mov     dx, FDC_DIR
  in      al, dx
  mov     [edi + FdcOut_dir_before], al
  mov     [edi + FdcOut_dir_after], al
  jmp     .ok

;  IOCTL_FDC_FORMAT (in: drive, head, cyl, sec, size_code, gpl, filler,
;  rate, spec1, spec2 - the whole FdcIn).  The caller has already seeked.
.ioc_format:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  call    fdc_format_track
  jmp     .ok

;  IOCTL_FDC_CMOS: report the BIOS's own record of what the drives are
.ioc_cmos:
  test    edi, edi
  jz      .out_inval
  call    fdc_read_cmos_floppy
  jmp     .ok

;  IOCTL_FDC_IDENT: prove the driver is live, and report what it was
;  handed.
.ioc_ident:
  test    edi, edi
  jz      .out_inval
  mov     word [edi], FDC_VXD_VERSION   ; version
  mov     eax, [esi + DIOC_dwIoControlCode]
  mov     [edi + 4], eax                ; the code we saw
  mov     eax, [esi + DIOC_lpvOutBuffer]
  mov     [edi + 8], eax                ; the out buffer we were given
  mov     eax, [esi + DIOC_cbOutBuffer]
  mov     [edi + 12], eax               ; and its size
  jmp     .ok

.out_inval:
  mov     eax, 0x57                ; ERROR_INVALID_PARAMETER
  jmp     .out
.ok:
  mov     ebp, [esi + DIOC_lpcbBytesReturned]
  test    ebp, ebp
  jz      .skip_cb
  mov     ecx, [esi + DIOC_cbOutBuffer]
  cmp     ecx, FdcOut_total_size
  jbe     .have_cb
  mov     ecx, FdcOut_total_size
.have_cb:
  mov     [ebp], ecx
.skip_cb:
  xor     eax, eax
.out:
  pop     esi
  pop     edi
  pop     ebx
  pop     ebp
  clc
  ret

;  FDC primitives.  Convention: EDI = FdcOut*, results via [EDI + FdcOut_*].
;  Other args in EAX/EBX/ECX as documented.  EDI/ESI/EBP preserved.

%define FDC_SPIN     20000

;  fdc_wait_rqm_out: wait for MSR.RQM=1, DIO=0 (ready for a byte).
;  CF set on timeout.
fdc_wait_rqm_out:
  mov     ecx, FDC_SPIN
.spin:
  mov     dx, FDC_MSR
  in      al, dx
  and     al, 0xC0
  cmp     al, 0x80
  je      .ready
  dec     ecx
  jnz     .spin
  mov     dx, FDC_MSR  ; Timed out.
  in      al, dx
  mov     [edi + FdcOut_msr], al
  stc
  ret
.ready:
  clc
  ret

;  fdc_wait_rqm_in: wait for MSR.RQM=1, DIO=1 (FDC has a byte).
;  CF set on timeout.
fdc_wait_rqm_in:
  mov     ecx, FDC_SPIN
.spin:
  mov     dx, FDC_MSR
  in      al, dx
  and     al, 0xC0
  cmp     al, 0xC0
  je      .ready
  dec     ecx
  jnz     .spin
  stc
  ret
.ready:
  clc
  ret

;  fdc_send_byte: AL -> FIFO after wait.  CF set on timeout.
fdc_send_byte:
  push    eax
  call    fdc_wait_rqm_out
  pop     eax
  jc      .err
  mov     dx, FDC_FIFO
  out     dx, al
  clc
  ret
.err:
  stc
  ret

;  fdc_read_byte: FIFO -> AL.  CF set on timeout.
fdc_read_byte:
  call    fdc_wait_rqm_in
  jc      .err
  mov     dx, FDC_FIFO
  in      al, dx
  clc
  ret
.err:
  xor     al, al
  stc
  ret

;  fdc_read_cmos_floppy: read CMOS 10h to determine drive types.
fdc_read_cmos_floppy:
  pushfd
  cli
  mov     al, 0x10                 ; index, NMI left enabled as we found it
  mov     dx, CMOS_INDEX
  out     dx, al
  mov     ecx, 8                   ; brief settle before reading the data port
.d:
  in      al, dx
  loop    .d
  mov     dx, CMOS_DATA
  in      al, dx
  mov     [edi + FdcOut_cmos], al
  popfd
  ret

;  fdc_flush: wait until the controller will accept a command phase,
;  discarding anything it is still offering.  CF set if it never does.
fdc_flush:
  push    eax
  push    ecx
  push    edx
  mov     ecx, FDC_SPIN
.next:
  mov     dx, FDC_MSR
  in      al, dx
  and     al, 0xC0
  cmp     al, 0x80                 ; RQM=1, DIO=0 -> ready for a command
  je      .ok
  cmp     al, 0xC0                 ; RQM=1, DIO=1 -> take the byte, re-check
  jne     .wait
  mov     dx, FDC_FIFO
  in      al, dx
.wait:
  dec     ecx
  jnz     .next
  pop     edx
  pop     ecx
  pop     eax
  stc
  ret
.ok:
  pop     edx
  pop     ecx
  pop     eax
  clc
  ret

;  fdc_release: motors off, gate open.
fdc_release:
  pushfd
  cli
  push    eax
  push    edx
  mov     dx, FDC_DOR
  mov     al, 0x0C                 ; FDC enabled, gate open, motors off
  out     dx, al
  pop     edx
  pop     eax
  popfd
  ret

;  fdc_set_dor (EAX = drive, EBX = flags: bit 0 spindle, bit 1 gate).
;  Bit 3 of the DOR gates the controller's interrupt and DMA lines onto the
;  bus.
fdc_set_dor:
  push    eax
  push    ebx
  push    ecx
  push    edx
  and     al, 3
  mov     cl, al
  xor     ah, ah                   ; motor-enable mask
  test    bl, 1
  jz      .no_motor
  mov     ah, 0x10                 ; bit 4 is drive 0's motor
  shl     ah, cl                   ; shift to the actual drive
.no_motor:
  or      al, 0x04                 ; FDC enable (not-reset)
  test    bl, 2
  jz      .no_dma
  or      al, 0x08                 ; DMA/IRQ gate
.no_dma:
  or      al, ah
  mov     dx, FDC_DOR
  out     dx, al
  mov     ecx, 100                 ; short post-DOR settle delay
.d:
  in      al, dx
  loop    .d
  pop     edx
  pop     ecx
  pop     ebx
  pop     eax
  ret

;  fdc_gate_shut / fdc_gate_open (EAX = drive).
fdc_gate_shut:
  push    ebx
  mov     ebx, 1                   ; motor on, gate shut
  call    fdc_set_dor
  pop     ebx
  ret
fdc_gate_open:
  push    ebx
  mov     ebx, 3                   ; motor on, gate open
  call    fdc_set_dor
  pop     ebx
  ret

;  drain_result_phase: read up to 7 bytes into [edi+FdcOut_st0+i] and
;  record the count in [edi+FdcOut_result_n].
drain_result_phase:
  push    ebx
  push    eax
  xor     ebx, ebx
.next:
  call    fdc_wait_rqm_in
  jc      .done
  mov     dx, FDC_FIFO
  in      al, dx
  mov     [edi + FdcOut_st0 + ebx], al
  inc     ebx
  cmp     ebx, 7
  jae     .done
  mov     dx, FDC_MSR              ; if MSR.CB still set, expect more
  in      al, dx
  test    al, 0x10
  jnz     .next
.done:
  mov     [edi + FdcOut_result_n], bl
  pop     eax
  pop     ebx
  ret

;  fdc_reset: DOR-pulse the FDC, then SENSE INT until it stops reporting
;  one.
fdc_reset:
  pushfd
  cli
  mov     dx, FDC_DOR
  xor     al, al
  out     dx, al
  mov     ecx, 500                 ; hold the reset line low
.d:
  in      al, dx
  loop    .d
  mov     al, 0x0C
  out     dx, al
  mov     ecx, 500                 ; and let the chip come back up
.d2:
  in      al, dx
  loop    .d2
  mov     ecx, 4
.sense:
  push    ecx
  call    fdc_sense_int_raw
  pop     ecx
  jc      .done
  mov     al, [edi + FdcOut_st0]
  and     al, 0xC0
  cmp     al, 0x80
  je      .done
  loop    .sense
.done:
  mov     byte [edi + FdcOut_status], 0
  popfd
  ret

;  fdc_sense_int_raw: SENSE INT, store the result into [edi+FdcOut_st0] and
;  [edi+FdcOut_cur_cyl].
fdc_sense_int_raw:
  mov     al, FDC_CMD_SENSE_INT
  call    fdc_send_byte
  jc      .err
  call    fdc_read_byte
  jc      .err
  mov     [edi + FdcOut_st0], al
  mov     byte [edi + FdcOut_result_n], 1
  and     al, 0xC0
  cmp     al, 0x80                 ; invalid command: no PCN byte follows
  je      .done
  call    fdc_read_byte
  jc      .err
  mov     [edi + FdcOut_cur_cyl], al
  mov     byte [edi + FdcOut_result_n], 2
.done:
  clc
  ret
.err:
  mov     byte [edi + FdcOut_status], 0xE0
  stc
  ret

;  fdc_specify (EBP = FdcIn*): data rate and drive timings.
fdc_specify:
  pushfd
  cli
  movzx   eax, byte [ebp + FdcIn_rate]
  and     al, 3
  mov     dx, FDC_CCR
  out     dx, al
  call    fdc_flush
  jc      .err
  mov     al, FDC_CMD_SPECIFY
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_spec1]
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_spec2]
  call    fdc_send_byte
  jc      .err
  popfd
  ret
.err:
  mov     byte [edi + FdcOut_status], 0xE4
  popfd
  ret

;  fdc_issue_recal (EAX = drive): start a seek to track 0 and come straight
;  back.
fdc_issue_recal:
  push    eax
  pushfd
  cli
  call    fdc_gate_shut
  call    fdc_flush
  jc      .err
  mov     al, FDC_CMD_RECAL
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 4]           ; drive
  and     al, 3
  call    fdc_send_byte
  jc      .err
  popfd
  add     esp, 4
  ret
.err:
  call    fdc_reset                ; never hand back a half-commanded FDC
  mov     byte [edi + FdcOut_status], 0xE1   ; after the reset, which clears it
  popfd
  add     esp, 4
  ret

;  fdc_issue_seek (EAX = drive, EBX = head, ECX = cyl): start the step.
fdc_issue_seek:
  push    eax
  push    ebx
  push    ecx
  pushfd
  cli
  call    fdc_gate_shut
  call    fdc_flush
  jc      .err
  mov     al, FDC_CMD_SEEK
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 12]          ; drive
  mov     ebx, [esp + 8]           ; head
  and     al, 3
  and     bl, 1
  shl     ebx, 2
  or      al, bl
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 4]           ; cyl
  call    fdc_send_byte
  jc      .err
  popfd
  add     esp, 12
  ret
.err:
  call    fdc_reset                ; never hand back a half-commanded FDC
  mov     byte [edi + FdcOut_status], 0xE2   ; after the reset, which clears it
  popfd
  add     esp, 12
  ret

;  fdc_issue_read_id (EAX = drive, EBX = head): start the search for a
;  sector header.
fdc_issue_read_id:
  push    eax
  push    ebx
  pushfd
  cli
  call    fdc_gate_shut
  call    fdc_flush
  jc      .err
  mov     al, FDC_CMD_READ_ID
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 8]           ; drive
  mov     ebx, [esp + 4]           ; head
  and     al, 3
  and     bl, 1
  shl     ebx, 2
  or      al, bl
  call    fdc_send_byte
  jc      .err
  popfd
  add     esp, 8
  ret
.err:
  call    fdc_reset                ; never hand back a half-commanded FDC
  mov     byte [edi + FdcOut_status], 0xE3   ; after the reset, which clears it
  popfd
  add     esp, 8
  ret

;  fdc_poll (EBP = FdcIn*): has the command finished?
fdc_poll:
  pushfd
  cli
  mov     dx, FDC_MSR
  in      al, dx
  mov     [edi + FdcOut_msr], al
  test    al, 0x10                 ; CB: a command is still running
  jnz     .busy
  test    byte [ebp + FdcIn_flags], 1
  jz      .idle
  ;  A seek reports completion only through its interrupt, and while the
  ;  head is still moving SENSE INTERRUPT answers "invalid command".
  call    fdc_sense_int_raw
  jc      .hard                    ; status already carries the error
  mov     al, [edi + FdcOut_st0]
  and     al, 0xC0
  cmp     al, 0x80
  je      .busy
.idle:
  mov     byte [edi + FdcOut_status], FDC_ST_OK
.done:
  movzx   eax, byte [ebp + FdcIn_drive]     ; command over: gate back on
  call    fdc_gate_open
  popfd
  ret
.busy:
  mov     byte [edi + FdcOut_status], FDC_ST_BUSY
  popfd
  ret
.hard:
  jmp     .done                             ; status already carries the error

;  fdc_result: collect the result phase, if the controller is offering one.
fdc_result:
  pushfd
  cli
  mov     dx, FDC_MSR
  in      al, dx
  mov     [edi + FdcOut_msr], al
  and     al, 0xC0
  cmp     al, 0xC0                 ; RQM + DIO: the bytes are waiting
  je      .take
  mov     al, [edi + FdcOut_msr]
  test    al, 0x10
  jz      .lost
  mov     byte [edi + FdcOut_status], FDC_ST_BUSY
  popfd
  ret
.lost:
  mov     byte [edi + FdcOut_status], 0xE5
  jmp     .done
.take:
  call    drain_result_phase
  mov     byte [edi + FdcOut_status], FDC_ST_OK
.done:
  movzx   eax, byte [ebp + FdcIn_drive]     ; command over: gate back on
  call    fdc_gate_open
  popfd
  ret

;  fdc_format_track (EBP = FdcIn*, EDI = FdcOut*)
;  Lays down one whole track: fresh ID address marks, data address marks,
;  gaps and CRCs, with every data field set to the filler byte. Runs the FDC
;  in non-DMA mode.
fdc_format_track:
  mov     byte [edi + FdcOut_stage], 1
  push    esi
  pushfd
  cli
  call    fdc_flush
  jc      .err
  movzx   eax, byte [ebp + FdcIn_rate]
  and     al, 3
  mov     dx, FDC_CCR
  out     dx, al

  ;  DMA gate off: this transfer is programmed I/O.
  mov     byte [edi + FdcOut_stage], 2
  movzx   eax, byte [ebp + FdcIn_drive]
  mov     ebx, 1                        ; motor on, gate off
  call    fdc_set_dor

  mov     byte [edi + FdcOut_stage], 3

  mov     al, FDC_CMD_SPECIFY
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_spec1]
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_spec2]
  or      al, 1                    ; ND = 1, non-DMA
  call    fdc_send_byte
  jc      .err

  mov     byte [edi + FdcOut_stage], 5

  ;  Command phase: 4Dh, (head<<2)|drive, N, SC, GPL, filler.
  mov     al, FDC_CMD_FORMAT
  call    fdc_send_byte
  jc      .err
  movzx   eax, byte [ebp + FdcIn_drive]
  and     al, 3
  movzx   ebx, byte [ebp + FdcIn_head]
  and     bl, 1
  shl     ebx, 2
  or      al, bl
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_size_code]
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_sec]         ; SC, sectors per track
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_gpl]
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_filler]
  call    fdc_send_byte
  jc      .err

  ;  Execution phase: one C,H,R,N per sector, R counting 1..SC.
  mov     byte [edi + FdcOut_stage], 6
  movzx   esi, byte [ebp + FdcIn_sec]
  or      esi, esi
  jz      .err
  mov     bl, 1                         ; R
.sector:
  ;  If the controller gives DIO=1 it has abandoned the command.
  mov     dx, FDC_MSR
  in      al, dx
  and     al, 0xC0
  cmp     al, 0xC0
  je      .early_result
  mov     al, [ebp + FdcIn_cyl]         ; C
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_head]        ; H
  call    fdc_send_byte
  jc      .err
  mov     al, bl                        ; R
  call    fdc_send_byte
  jc      .err
  mov     al, [ebp + FdcIn_size_code]   ; N
  call    fdc_send_byte
  jc      .err
  inc     bl
  dec     esi
  jnz     .sector

  mov     byte [edi + FdcOut_stage], 7
  call    drain_result_phase
  call    .restore_dma
  popfd
  pop     esi
  ret
.early_result:
  call    drain_result_phase
  call    .restore_dma
  popfd
  pop     esi
  ret
.err:
  ;  Reset the controller.
  mov     byte [edi + FdcOut_status], 0xE6
  push    edi
  call    fdc_reset
  pop     edi
  mov     byte [edi + FdcOut_status], 0xE6   ; fdc_reset clears
  call    .restore_dma
  popfd
  pop     esi
  ret

;  Put the controller back the way Windows' driver expects to find it.
.restore_dma:
  call    fdc_flush
  mov     al, FDC_CMD_SPECIFY
  call    fdc_send_byte
  jc      .gate
  mov     al, [ebp + FdcIn_spec1]
  call    fdc_send_byte
  jc      .gate
  mov     al, [ebp + FdcIn_spec2]
  and     al, 0xFE                 ; ND = 0
  call    fdc_send_byte
.gate:
  movzx   eax, byte [ebp + FdcIn_drive]
  mov     ebx, 3                   ; same drive, motor on, DMA gate restored
  call    fdc_set_dor
  ret

;  Pad the image to a 4 KB page boundary for clean LE wrapping.
code_end:
%if (code_end - ddb_start) > PAGE_SIZE
  %error "LCOD exceeds its 4 KB page - give it more pages in the object table"
%endif
  align   PAGE_SIZE, db 0
  times   (PAGE_COUNT - 1) * PAGE_SIZE db 0
