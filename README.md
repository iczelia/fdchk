# fdchk - Floppy Disk Checker for Windows 9x

A surface scanner, FAT12 consistency checker, file-recovery utility,
defragmenter and raw-FDC head diagnostic for Windows 9x (95/98/Me).
Licensed under the GNU GPL v3, see `LICENSE`. Report bugs to
<k@iczelia.net>.

## Test modes

1. Standard - read-only verification.  Reads every sector and reports
   anything that does not come back cleanly.
2. Thorough - destructive per-sector surface test: read the original,
   then write-and-verify the original, its inverse, a PRNG block and that
   block's inverse, and finally restore the original.
3. Diagnostic - probes the drive.  Uses BIOS `INT 13h` via VWIN32; with
   `fdchk.vxd` loaded it switches to raw FDC commands and surfaces the
   full `ST0/ST1/ST2/C/H/R/N` result phase.
4. Check filesystem - walks FAT12 read-only for cross-linked clusters,
   lost chains, size mismatches, chain cycles and bad FAT entries.

## Other operations

- Recover - after a surface scan, walks the directory tree, copies out
  every file with zero-filled holes where bad sectors fell, and writes
  `fdchk-recovery.log`.  With auto-fix on, new bad clusters are marked
  `0xFF7` in both FATs.
- Format - quick or full FAT12 format of a 1.44 MB disk.
- Defrag - repacks files and subdirectories into contiguous low
  clusters (depth-first, alphabetical at each level) and zeroes every
  free cluster.
- Batch mode - after each scan, prompts to insert the next disk.
- Logs - an in-app viewer for the log files in `<exe-dir>\logs`.

## Diagnostic colours

In Diagnostic mode the grid becomes a `cyl x head` map (160 cells):

| Colour | Meaning |
|---|---|
| Grey   | OK - `ST0 & 0xC0 == 0` and C matches |
| Yellow | currently testing |
| Orange | Wrong Cylinder - `ST2` bit 4, or C != cyl.  Head misalignment |
| Purple | No Address Mark - `ST1`/`ST2` bit 0.  Track unformatted/severe |
| Red    | other failure (CRC, timeout, controller fault) |

## Build

Requires `i686-w64-mingw32-gcc` and `nasm`; `mtools` is needed only for
`make floppy`.

```
make           fdchk.exe + fdchk.vxd
make floppy    fdchk.img - a 1.44 MB FAT12 image carrying the EXE
make debug     with-CRT build carrying symbols
make clean
```
