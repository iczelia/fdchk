##  Copyright (C) 2026 Kamila Szewczyk
##
##  This program is free software; you can redistribute it and/or modify
##  it under the terms of the GNU General Public License as published by
##  the Free Software Foundation, version 3.
##
##  This program is distributed in the hope that it will be useful,
##  but WITHOUT ANY WARRANTY; without even the implied warranty of
##  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##  GNU General Public License for more details.
##
##  You should have received a copy of the GNU General Public License
##  along with this program. If not, see <http://www.gnu.org/licenses/>.

CC      = i686-w64-mingw32-gcc
WINDRES = i686-w64-mingw32-windres
NASM    = nasm
MFORMAT = mformat
MCOPY   = mcopy

OBJS = fdchk.o disk.o vxd.o fat.o scan.o defrag.o ui.o

CFLAGS = -m32 -march=i486 -mtune=i486 -std=c11 -Os \
         -Wall -Wextra -Wno-unused-parameter -fno-ident \
         -fno-asynchronous-unwind-tables -fno-unwind-tables \
         -ffunction-sections -fdata-sections \
         -fno-stack-protector -fno-stack-check -fomit-frame-pointer \
         -mno-stack-arg-probe -fno-tree-loop-distribute-patterns \
         -mwindows -DUNICODE=0 -D_UNICODE=0 -DWIN32_LEAN_AND_MEAN

LDFLAGS = -nostdlib -nostartfiles -nodefaultlibs \
          -Wl,-e_WinMainCRTStartup -Wl,-uWinMainCRTStartup \
          -Wl,--gc-sections,-s,--build-id=none,--no-seh \
          -Wl,--file-alignment=512,--section-alignment=4096 \
          -Wl,--major-subsystem-version,4,--minor-subsystem-version,0 \
          -Wl,--subsystem,windows

LIBS = -lkernel32 -luser32 -lgdi32 -lcomctl32

.PHONY: all floppy debug clean

all: fdchk.exe

fdchk.exe: $(OBJS) fdchk.res
	$(CC) $(CFLAGS) -o $@ $(OBJS) fdchk.res $(LDFLAGS) $(LIBS)

$(OBJS): fdchk.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

fdchk.res: fdchk.rc fdchk.ico fdchk.vxd
	$(WINDRES) -O coff -i fdchk.rc -o $@

fdchk.vxd: fdchk_vxd.asm
	$(NASM) -f bin -o $@ $<

floppy: fdchk.exe
	dd if=/dev/zero of=fdchk.img bs=512 count=2880 status=none
	$(MFORMAT) -i fdchk.img -f 1440 -v FDCHK ::
	$(MCOPY) -i fdchk.img fdchk.exe ::FDCHK.EXE

debug: fdchk.vxd fdchk.res
	$(CC) -m32 -std=c11 -g -O0 -DDEBUG -Wall -Wextra \
	      -Wno-unused-parameter -mwindows -DUNICODE=0 -D_UNICODE=0 \
	      -DWIN32_LEAN_AND_MEAN -o fdchk.exe $(OBJS:.o=.c) fdchk.res \
	      $(LIBS)

clean:
	rm -f fdchk.exe $(OBJS) fdchk.res fdchk.vxd fdchk.img
