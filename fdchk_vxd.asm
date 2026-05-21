;  Copyright (C) 2026 Kamila Szewczyk
;
;  This program is free software; you can redistribute it and/or modify
;  it under the terms of the GNU General Public License as published by
;  the Free Software Foundation; either version 3 of the License, or
;  (at your option) any later version.
;
;  This program is distributed in the hope that it will be useful,
;  but WITHOUT ANY WARRANTY; without even the implied warranty of
;  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;  GNU General Public License for more details.
;
;  You should have received a copy of the GNU General Public License
;  along with this program. If not, see <http://www.gnu.org/licenses/>.
;
;  fdchk.vxd - raw NEC uPD765 floppy-controller access for Windows 9x.
;  Assembles straight to a loadable VxD with `nasm -f bin`: the Win9x LE
;  container is hand-built below, the position-independent image follows
;  at IMAGE_OFF.  The loader's only task is the single fixup of the
;  absolute DDB.Control_Proc pointer at image offset 24.

bits 32
cpu  486
org  0

;  ----- file layout -------------------------------------------------------
%define LE_OFF       0x40          ; LE header file offset (MZ e_lfanew)
%define IMAGE_OFF    0x1000        ; VxD image file offset
%define IMAGE_ENTRY  0x80          ; control_proc within the image
%define PAGE_SIZE    0x1000

;  =========================================================================
;  MZ / DOS stub.  Just enough header for the loader to find e_lfanew.
;  =========================================================================
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

;  =========================================================================
;  LE header.  One 4 KB code+data object, one page, one fixup record.
;  =========================================================================
le_start:
  dw      0x454C                   ; 'LE'
  db      0                        ; byte order
  db      0                        ; word order
  dd      0                        ; format level
  dw      3                        ; cpu type = 80386
  dw      4                        ; os type = Windows VxD
  dd      0                        ; module version
  dd      0x00008000               ; module flags = virtual device
  dd      1                        ; page count
  dd      1                        ; EIP object number
  dd      IMAGE_ENTRY              ; EIP
  dd      0                        ; ESP object number
  dd      0                        ; ESP
  dd      PAGE_SIZE                ; page size
  dd      PAGE_SIZE                ; bytes on last page
  dd      fixup_end - fixup_pages  ; fixup section size
  dd      0                        ; fixup section checksum
  dd      loader_end - obj_table   ; loader section size
  dd      0                        ; loader section checksum
  dd      obj_table   - le_start   ; object table offset
  dd      1                        ; object count
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
  dd      0                        ; preload page count
  dd      0                        ; non-resident name table offset
  dd      0                        ; non-resident name table length
  dd      0                        ; non-resident name table checksum
  dd      0                        ; auto data-segment object
  dd      0                        ; debug info offset
  dd      0                        ; debug info length
  dd      0                        ; preload-only page count
  dd      0                        ; demand-load page count
  dd      0                        ; extra heap allocation
  dd      0                        ; reserved
  times   0xC4 - ($ - le_start) db 0
  dd      0                        ; VxD version resource offset
  dd      0                        ; VxD version resource length
  dw      0                        ; VxD device id = UNDEFINED
  dw      0x030A                   ; VxD DDK version

;  =========================================================================
;  Loader section: object table, page map, names, entry + fixup tables.
;  =========================================================================
obj_table:
  dd      PAGE_SIZE                ; virtual size
  dd      0                        ; relocation base address
  dd      0x00002207               ; flags: read|write|exec|resident|big
  dd      1                        ; page table index
  dd      1                        ; page count
  dd      0                        ; reserved

page_map:
  db      0x00, 0x00, 0x01         ; page number 1 (big-endian 24-bit)
  db      0x00                     ; flag: page is valid

res_names:
  db      5, 'FDCHK'               ; module name
  dw      0                        ; ordinal
  db      0                        ; end of table

entry_table:
  db      0                        ; end-of-bundle marker

fixup_pages:
  dd      0                        ; page 1: first fixup record
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

;  =========================================================================
;  VxD image.  Loaded as object 1; control_proc lands at object offset
;  0x80.  org stays 0 - the image has no absolute self-references, so its
;  bytes are identical wherever it sits in the file.
;  =========================================================================

;  ----- VMM message codes (EAX on entry to the control proc) --------------
%define Sys_Dynamic_Device_Init    0x001B
%define Sys_Dynamic_Device_Exit    0x001C
%define W32_DEVICEIOCONTROL        0x0023

;  ----- DIOC_PARAMS layout (ESI on W32_DEVICEIOCONTROL) -------------------
%define DIOC_dwIoControlCode       0x0C
%define DIOC_lpvInBuffer           0x10
%define DIOC_cbInBuffer            0x14
%define DIOC_lpvOutBuffer          0x18
%define DIOC_cbOutBuffer           0x1C
%define DIOC_lpcbBytesReturned     0x20

;  ----- our IOCTL codes ---------------------------------------------------
%define DIOC_GETVERSION            0
%define IOCTL_FDC_RESET            0x0080
%define IOCTL_FDC_SENSEINT         0x0081
%define IOCTL_FDC_RECAL            0x0082
%define IOCTL_FDC_SEEK             0x0083
%define IOCTL_FDC_READID           0x0084
%define IOCTL_FDC_SPECIFY          0x0085
%define IOCTL_FDC_RAW              0x008F

;  ----- FDC ports ---------------------------------------------------------
%define FDC_DOR                    0x3F2
%define FDC_MSR                    0x3F4
%define FDC_FIFO                   0x3F5
%define FDC_CCR                    0x3F7

;  ----- FDC commands ------------------------------------------------------
%define FDC_CMD_SPECIFY            0x03
%define FDC_CMD_RECAL              0x07
%define FDC_CMD_SENSE_INT          0x08
%define FDC_CMD_SEEK               0x0F
%define FDC_CMD_READ_ID            0x4A   ; READ ID + MFM

;  ----- FdcIn  (user input buffer, EBP in dispatch) -----------------------
%define FdcIn_drive                0
%define FdcIn_head                 1
%define FdcIn_cyl                  2
%define FdcIn_sec                  3
%define FdcIn_size_code            4
%define FdcIn_motor                6
%define FdcIn_raw_count            7
%define FdcIn_raw                  8

;  ----- FdcOut (user output buffer, EDI in FDC routines) ------------------
%define FdcOut_status              0
%define FdcOut_st0                 1
%define FdcOut_st1                 2
%define FdcOut_st2                 3
%define FdcOut_c                   4
%define FdcOut_h                   5
%define FdcOut_r                   6
%define FdcOut_n                   7
%define FdcOut_cur_cyl             8
%define FdcOut_result_n            9
%define FdcOut_total_size          16

;  =========================================================================
;  Offset 0: the DDB.  Exactly 0x80 bytes including padding so control_proc
;  lands at the fixed image offset 0x80.
;  =========================================================================
ddb_start:
  dd      0                        ;  0 DDB_Next (set by VMM)
  dw      0x030A                   ;  4 DDB_SDK_Version
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
  times   0x80 - ($ - ddb_start) db 0

;  =========================================================================
;  Offset 0x80: control_proc.  The LE entry point.
;  =========================================================================
control_proc:
  cmp     eax, Sys_Dynamic_Device_Init
  je      .ok
  cmp     eax, Sys_Dynamic_Device_Exit
  je      .ok
  cmp     eax, W32_DEVICEIOCONTROL
  je      do_ioctl
  ; any other message: succeed and ignore
.ok:
  clc
  ret

;  -------------------------------------------------------------------------
;  do_ioctl - ESI = DIOC_PARAMS*, EBX = VM handle.  EAX = 0 on success.
;  -------------------------------------------------------------------------
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
  mov     ecx, FdcOut_total_size
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
  cmp     eax, IOCTL_FDC_RESET
  je      .ioc_reset
  cmp     eax, IOCTL_FDC_SENSEINT
  je      .ioc_senseint
  cmp     eax, IOCTL_FDC_RECAL
  je      .ioc_recal
  cmp     eax, IOCTL_FDC_SEEK
  je      .ioc_seek
  cmp     eax, IOCTL_FDC_READID
  je      .ioc_readid
  cmp     eax, IOCTL_FDC_SPECIFY
  je      .ioc_specify
  cmp     eax, IOCTL_FDC_RAW
  je      .ioc_raw

  mov     eax, 0x32                ; ERROR_NOT_SUPPORTED
  jmp     .out

;  --- DIOC_GETVERSION: write 0x0100 into the first word of the out-buffer
.ioc_version:
  test    edi, edi
  jz      .out_inval
  mov     word [edi], 0x0100
  jmp     .ok

;  --- IOCTL_FDC_RESET
.ioc_reset:
  test    edi, edi
  jz      .out_inval
  call    fdc_reset
  jmp     .ok

;  --- IOCTL_FDC_SENSEINT
.ioc_senseint:
  test    edi, edi
  jz      .out_inval
  call    fdc_sense_int
  jmp     .ok

;  --- IOCTL_FDC_RECAL (in: drive)
.ioc_recal:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  movzx   eax, byte [ebp + FdcIn_drive]
  call    fdc_recalibrate
  jmp     .ok

;  --- IOCTL_FDC_SEEK (in: drive, head, cyl)
.ioc_seek:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  movzx   eax, byte [ebp + FdcIn_drive]
  movzx   ebx, byte [ebp + FdcIn_head]
  movzx   ecx, byte [ebp + FdcIn_cyl]
  call    fdc_seek
  jmp     .ok

;  --- IOCTL_FDC_READID (in: drive, head)
.ioc_readid:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  movzx   eax, byte [ebp + FdcIn_drive]
  movzx   ebx, byte [ebp + FdcIn_head]
  call    fdc_read_id
  jmp     .ok

;  --- IOCTL_FDC_SPECIFY (in: raw[0]=SRT/HUT, raw[1]=HLT/ND)
.ioc_specify:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  movzx   eax, byte [ebp + FdcIn_raw + 0]
  movzx   ebx, byte [ebp + FdcIn_raw + 1]
  call    fdc_specify
  jmp     .ok

;  --- IOCTL_FDC_RAW (in: raw_count, raw[])
.ioc_raw:
  mov     ebp, [esi + DIOC_lpvInBuffer]
  test    ebp, ebp
  jz      .out_inval
  test    edi, edi
  jz      .out_inval
  call    fdc_raw_command
  jmp     .ok

.out_inval:
  mov     eax, 0x57                ; ERROR_INVALID_PARAMETER
  jmp     .out
.ok:
  ; report 16 bytes returned
  mov     ebp, [esi + DIOC_lpcbBytesReturned]
  test    ebp, ebp
  jz      .skip_cb
  mov     dword [ebp], FdcOut_total_size
.skip_cb:
  xor     eax, eax
.out:
  pop     esi
  pop     edi
  pop     ebx
  pop     ebp
  clc
  ret

;  =========================================================================
;  FDC primitives.  Convention: EDI = FdcOut*, results via [EDI + FdcOut_*].
;  Other args in EAX/EBX/ECX as documented.  EDI/ESI/EBP preserved.
;  =========================================================================

;  --- fdc_wait_rqm_out: wait for MSR.RQM=1, DIO=0 (ready for a byte).
;      CF set on timeout.
fdc_wait_rqm_out:
  mov     ecx, 0x100000
.spin:
  mov     dx, FDC_MSR
  in      al, dx
  and     al, 0xC0
  cmp     al, 0x80
  je      .ready
  dec     ecx
  jnz     .spin
  stc
  ret
.ready:
  clc
  ret

;  --- fdc_wait_rqm_in: wait for MSR.RQM=1, DIO=1 (FDC has a byte).
;      CF set on timeout.
fdc_wait_rqm_in:
  mov     ecx, 0x100000
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

;  --- fdc_send_byte: AL -> FIFO after wait.  CF set on timeout.
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

;  --- fdc_read_byte: FIFO -> AL.  CF set on timeout.
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

;  --- motor_on: spin up the drive in AL (low 2 bits) via the DOR.
motor_on:
  push    eax
  push    edx
  and     al, 3
  mov     ah, 0x10                 ; motor-enable mask (bit 4 for drive 0)
  mov     cl, al
  shl     ah, cl                   ; shift to the actual drive
  or      al, 0x0C                 ; FDC enable + DMA gate
  or      al, ah                   ; OR in the motor bit
  mov     dx, FDC_DOR
  out     dx, al
  mov     ecx, 100                 ; short post-DOR settle delay
.d:
  in      al, dx
  loop    .d
  pop     edx
  pop     eax
  ret

;  --- drain_result_phase: read up to 7 bytes into [edi+FdcOut_st0+i] and
;      record the count in [edi+FdcOut_result_n].
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

;  --- fdc_reset: DOR-pulse the FDC, then 4x SENSE INT to clear pending IRQs.
fdc_reset:
  pushfd
  cli
  mov     dx, FDC_DOR
  xor     al, al
  out     dx, al
  mov     ecx, 4000
.d:
  in      al, dx
  loop    .d
  mov     al, 0x0C
  out     dx, al
  popfd
  push    ecx
  mov     ecx, 4
.sense:
  push    ecx
  call    fdc_sense_int_raw
  pop     ecx
  loop    .sense
  pop     ecx
  mov     byte [edi + FdcOut_status], 0
  ret

;  --- fdc_sense_int_raw: SENSE INT, store the 2 result bytes into
;      [edi+FdcOut_st0] and [edi+FdcOut_cur_cyl].
fdc_sense_int_raw:
  mov     al, FDC_CMD_SENSE_INT
  call    fdc_send_byte
  jc      .err
  call    fdc_read_byte
  jc      .err
  mov     [edi + FdcOut_st0], al
  call    fdc_read_byte
  jc      .err
  mov     [edi + FdcOut_cur_cyl], al
  mov     byte [edi + FdcOut_result_n], 2
  ret
.err:
  mov     byte [edi + FdcOut_status], 0xE0
  ret

;  --- fdc_sense_int: interrupt-locked variant of the above.
fdc_sense_int:
  pushfd
  cli
  call    fdc_sense_int_raw
  popfd
  ret

;  --- fdc_recalibrate (EAX = drive): seek to track 0.
fdc_recalibrate:
  push    eax
  pushfd
  cli
  pop     edx                      ; stash EFLAGS in EDX
  push    edx
  pop     edx
  call    motor_on
  mov     al, FDC_CMD_RECAL
  call    fdc_send_byte
  jc      .err
  pop     eax
  push    eax
  and     al, 3
  call    fdc_send_byte
  jc      .err
  mov     ecx, 0x400000            ; poll MSR.CB until the seek completes
.wait:
  mov     dx, FDC_MSR
  in      al, dx
  test    al, 0x10
  jz      .ready
  dec     ecx
  jnz     .wait
.ready:
  call    fdc_sense_int_raw
  pop     eax
  push    edx
  popfd
  ret
.err:
  mov     byte [edi + FdcOut_status], 0xE1
  add     esp, 4                   ; drop saved EAX
  push    edx
  popfd
  ret

;  --- fdc_seek (EAX = drive, EBX = head, ECX = cyl).
fdc_seek:
  push    eax
  push    ebx
  push    ecx
  pushfd
  cli
  mov     eax, [esp + 12]          ; drive
  call    motor_on
  mov     al, FDC_CMD_SEEK
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 12]          ; drive
  mov     ebx, [esp + 8]           ; head
  and     al, 3
  shl     ebx, 2
  or      al, bl
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 4]           ; cyl
  call    fdc_send_byte
  jc      .err
  mov     ecx, 0x400000
.wait:
  mov     dx, FDC_MSR
  in      al, dx
  test    al, 0x10
  jz      .ready
  dec     ecx
  jnz     .wait
.ready:
  call    fdc_sense_int_raw
  popfd
  add     esp, 12
  ret
.err:
  mov     byte [edi + FdcOut_status], 0xE2
  popfd
  add     esp, 12
  ret

;  --- fdc_read_id (EAX = drive, EBX = head): report the first sector
;      header found under the head; result phase carries all 7 bytes.
fdc_read_id:
  push    eax
  push    ebx
  pushfd
  cli
  mov     eax, [esp + 8]           ; drive
  call    motor_on
  mov     al, FDC_CMD_READ_ID
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 8]           ; drive
  mov     ebx, [esp + 4]           ; head
  and     al, 3
  shl     ebx, 2
  or      al, bl
  call    fdc_send_byte
  jc      .err
  call    drain_result_phase
  popfd
  add     esp, 8
  ret
.err:
  mov     byte [edi + FdcOut_status], 0xE3
  popfd
  add     esp, 8
  ret

;  --- fdc_specify (EAX = SRT<<4|HUT, EBX = HLT<<1|ND).
fdc_specify:
  push    eax
  push    ebx
  pushfd
  cli
  mov     al, FDC_CMD_SPECIFY
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 8]
  call    fdc_send_byte
  jc      .err
  mov     eax, [esp + 4]
  call    fdc_send_byte
  jc      .err
  popfd
  add     esp, 8
  ret
.err:
  mov     byte [edi + FdcOut_status], 0xE4
  popfd
  add     esp, 8
  ret

;  --- fdc_raw_command (EBP = FdcIn*, EDI = FdcOut*): send raw[0..raw_count)
;      then drain the result phase.  Issues any command we lack a wrapper for.
fdc_raw_command:
  pushfd
  cli
  movzx   ecx, byte [ebp + FdcIn_raw_count]
  cmp     ecx, 15
  ja      .err
  or      ecx, ecx
  jz      .err
  push    esi
  lea     esi, [ebp + FdcIn_raw]
.send:
  lodsb
  push    ecx
  call    fdc_send_byte
  pop     ecx
  jc      .erro
  loop    .send
  pop     esi
  call    drain_result_phase
  popfd
  ret
.erro:
  pop     esi
.err:
  mov     byte [edi + FdcOut_status], 0xE5
  popfd
  ret

;  =========================================================================
;  Pad the image to a 4 KB page boundary for clean LE wrapping.
;  =========================================================================
  align   PAGE_SIZE, db 0
end_of_image:
