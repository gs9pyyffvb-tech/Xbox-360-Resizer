The build fetches these public-domain/MIT stb headers at build time:

- stb_image.h
- deprecated/stb_image_resize.h
- stb_image_write.h

Run `scripts/fetch-stb.sh` before a local build if they are not already present.
