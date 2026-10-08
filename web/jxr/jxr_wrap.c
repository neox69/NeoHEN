/* NeoWEB - a JPEG XR picture, from memory, to plain 8-bit RGBA.
 *
 * The console keeps HDR screenshots (and their thumbnails) as JPEG XR, which
 * no browser shows. This is the one function the page needs from Microsoft's
 * jxrlib (BSD 2-Clause), built to WebAssembly by build.sh: it decodes a file
 * held in memory and hands back its pixels, 8 bits per channel, so a canvas
 * can draw them. HDR values are brought down by jxrlib's own conversion;
 * very bright highlights clip.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include <JXRGlue.h>

/* Decodes `len` bytes at `data`. On success returns the pixels (width *
 * height * 4 bytes, to be given back with free()) and sets *w and *h; on
 * failure returns NULL. `max_side` > 0 refuses anything wider or taller. */
unsigned char *
jxr_decode(const unsigned char *data, int len, int max_side, int *w, int *h) {
  struct WMPStream *stream = NULL;
  PKImageDecode *dec = NULL;
  PKFormatConverter *conv = NULL;
  unsigned char *out = NULL;
  I32 width = 0, height = 0;
  PKRect rect = {0, 0, 0, 0};
  int ok = 0;

  *w = *h = 0;
  if(len < 16 || Failed(CreateWS_Memory(&stream, (void *)data, (size_t)len))) {
    return NULL;
  }
  if(Failed(PKImageDecode_Create_WMP(&dec)) || Failed(dec->Initialize(dec, stream))) {
    goto done;
  }
  dec->fStreamOwner = 0; /* the stream is closed here, not by the decoder */
  if(Failed(dec->GetSize(dec, &width, &height)) || width <= 0 || height <= 0 ||
     (max_side > 0 && (width > max_side || height > max_side)) || width > 16384 || height > 16384) {
    goto done;
  }
  if(Failed(PKCodecFactory_CreateFormatConverter(&conv)) ||
     Failed(conv->Initialize(conv, dec, NULL, GUID_PKPixelFormat32bppRGBA))) {
    goto done;
  }
  /* the conversion works in place, so the row must fit the source's pixels
   * too: up to 16 bytes each (128-bit float) */
  if(!(out = malloc((size_t)width * (size_t)height * 16))) {
    goto done;
  }
  rect.Width = width;
  rect.Height = height;
  if(Failed(conv->Copy(conv, &rect, out, (U32)width * 16))) {
    goto done;
  }
  /* the rows came 16 bytes a pixel apart: pack them to 4 */
  for(I32 y = 0; y < height; y++) {
    memmove(out + (size_t)y * width * 4, out + (size_t)y * width * 16, (size_t)width * 4);
  }
  for(size_t i = 3, n = (size_t)width * height * 4; i < n; i += 4) {
    out[i] = 255; /* a screenshot has no see-through parts */
  }
  *w = (int)width;
  *h = (int)height;
  ok = 1;

done:
  if(conv) {
    conv->Release(&conv);
  }
  if(dec) {
    dec->Release(&dec);
  }
  if(stream) {
    stream->Close(&stream);
  }
  if(!ok) {
    free(out);
    out = NULL;
  }
  return out;
}

#ifdef JXR_WRAP_MAIN
/* Native check: jxr_wrap in.jxr out.ppm */
#include <stdio.h>
int
main(int argc, char **argv) {
  unsigned char *in, *px;
  long n;
  int w, h;
  FILE *f;

  if(argc != 3 || !(f = fopen(argv[1], "rb"))) {
    return 2;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  rewind(f);
  in = malloc((size_t)n);
  if(fread(in, 1, (size_t)n, f) != (size_t)n) {
    return 2;
  }
  fclose(f);
  if(!(px = jxr_decode(in, (int)n, 0, &w, &h))) {
    fprintf(stderr, "could not decode\n");
    return 1;
  }
  f = fopen(argv[2], "wb");
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for(long i = 0; i < (long)w * h; i++) {
    fwrite(px + i * 4, 1, 3, f);
  }
  fclose(f);
  printf("%d x %d\n", w, h);
  return 0;
}
#endif
