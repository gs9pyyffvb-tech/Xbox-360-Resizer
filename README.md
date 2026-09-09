# Aurora Image Resizer — OpenXeChain

Xbox 360 utility intended to be launched as `default.xex` from Aurora on an RGH/JTAG console.

It walks the configured directory and, by default, **recursively searches every subdirectory**. PNG, JPG/JPEG and BMP files are inspected. Images already matching the configured dimensions are left untouched; mismatches are resized to the exact target dimensions.

## Default configuration

```ini
[ImageResizer]
Path=Hdd1:\Images\
Width=900
Height=600
Recursive=true
JpegQuality=90
MaxInputMB=128
DryRun=false
```

`Recursive=true` is the default. Directory traversal uses an expandable work stack rather than function recursion, so there is no intentionally fixed subdirectory depth; the practical limit is memory and the 1024-character path limit in this build.

## Supported image formats

- PNG
- JPG / JPEG
- BMP

The original file format is retained. JPEG output uses `JpegQuality` (1–100).

## Replacement safety

Before overwriting a mismatched image, the program:

1. Reads and validates the source.
2. Decodes and resizes it completely in memory.
3. Encodes the complete replacement in memory.
4. Writes `<original>.xrs.tmp` containing the resized result.
5. Writes `<original>.xrs.bak` containing the original bytes.
6. Overwrites the original only after both safety files succeeded.
7. Deletes the `.xrs.tmp` and `.xrs.bak` files after a successful replacement.

If the final overwrite fails, both recovery files are deliberately retained. A power loss during the final write may likewise leave the `.xrs.bak` recovery file available.

## Log

A run writes `game:\resize.log` (falling back to `resize.log`). It records directories, matching images, resized images and failures, followed by totals.

## Dry run

Set:

```ini
DryRun=true
```

to recursively enumerate and report what would be resized without altering any image.

## Build with GitHub Actions

Push this project to a GitHub repository, then open **Actions → Build Xbox 360 XEX → Run workflow**. The workflow builds/caches OpenXeChain, fetches stb, compiles this program and uploads `default.xex` plus an `AuroraImageResizer.zip` artifact.

The current OpenXeChain buildscript uses GitHub SSH URLs for its public submodules; `scripts/build-toolchain.sh` rewrites those URLs to HTTPS so an unattended Actions runner can clone them.

## Local build

On a supported Linux machine:

```bash
scripts/build-toolchain.sh
scripts/fetch-stb.sh
OPENXECHAIN="$PWD/.openxechain/sysroot" ./build.sh
```

Output:

```text
build/default.xex
build/config.ini
```

## Install on Xbox 360

Copy at least:

```text
AuroraImageResizer/
  default.xex
  config.ini
```

onto the console, edit `config.ini`, add/scan the folder in Aurora if desired, and launch `default.xex`.

For a first hardware test, `DryRun=true` is recommended. Check `resize.log`, then set `DryRun=false` when the directory traversal and target path are confirmed.

## Technical notes / current risk

This project targets the open-source OpenXeChain stack and does not require the proprietary Microsoft Xbox 360 XDK. The program uses `xecorelib` XAM file calls for normal file I/O and `NtCreateFile`/`NtQueryDirectoryFile` for directory enumeration.

OpenXeChain and SynthXEX are still early-development projects. This source has been structured against their published headers and build flow, but this package has **not been executed on Xbox 360 hardware from this environment**. The first GitHub Actions run and first console dry run are therefore validation steps, not formal proof of runtime compatibility.

Filename conversion for directory enumeration is currently conservative: characters outside the single-byte range are replaced with `?`. Ordinary ASCII/Latin filenames are the intended first target.
