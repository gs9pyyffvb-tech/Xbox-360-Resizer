# Project status

Implemented:
- configurable directory
- 900x600 defaults
- recursive subdirectory discovery by default
- PNG/JPEG/BMP recognition
- dimensions check before decode/resize
- exact-dimension resize
- same-format output
- JPEG quality setting
- input-size cap
- dry-run mode
- per-run log
- temp + original-byte backup before overwrite
- failure continuation
- GitHub Actions cloud-build workflow
- OpenXeChain bootstrap script

Not verified in this environment:
- actual OpenXeChain compilation (toolchain is not installed locally)
- launch/runtime behavior on physical RGH/JTAG Xbox 360 hardware
- exact behavior for non-ASCII filenames
