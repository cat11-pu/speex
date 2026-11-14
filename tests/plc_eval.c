/*
 * Deterministic packet-loss evaluation harness for Speex.
 *
 * Reads 16-bit mono PCM, encodes every frame, then decodes the stream twice:
 * once without loss and once with a deterministic frame-loss pattern.  The
 * same encoded packets and loss decisions are used for every implementation.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speex/speex.h"

#define MAX_PACKET_BYTES 4096

typedef struct {
   unsigned char *data;
   int len;
} Packet;

static uint32_t rng_state;

static uint32_t next_rand(void)
{
   uint32_t x = rng_state;
   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;
   rng_state = x;
   return x;
}

static void usage(const char *name)
{
   fprintf(stderr,
           "usage: %s [-m nb|wb] [-q 0..10] [-l PERCENT] [-p uniform|periodic] [-s SEED]\n"
           "          input.raw clean.raw lossy.raw\n", name);
}

static int read_pcm(const char *path, int16_t **samples, size_t *count)
{
   FILE *f;
   long size;

   f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
      return -1;
   }
   if (fseek(f, 0, SEEK_END) != 0) {
      fclose(f);
      return -1;
   }
   size = ftell(f);
   if (size < 0 || (size & 1)) {
      fprintf(stderr, "input must be an even number of bytes\n");
      fclose(f);
      return -1;
   }
   rewind(f);
   *count = (size_t)size / sizeof(int16_t);
   *samples = (int16_t *)malloc(*count * sizeof(int16_t));
   if (!*samples) {
      fclose(f);
      return -1;
   }
   if (fread(*samples, sizeof(int16_t), *count, f) != *count) {
      fprintf(stderr, "short read from %s\n", path);
      free(*samples);
      fclose(f);
      return -1;
   }
   fclose(f);
   return 0;
}

static int write_pcm(const char *path, const int16_t *samples, size_t count)
{
   FILE *f = fopen(path, "wb");
   if (!f) {
      fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
      return -1;
   }
   if (fwrite(samples, sizeof(int16_t), count, f) != count) {
      fprintf(stderr, "short write to %s\n", path);
      fclose(f);
      return -1;
   }
   fclose(f);
   return 0;
}

static int write_loss_mask(const char *pcm_path, const unsigned char *lost, int nframes)
{
   char *path;
   FILE *f;
   size_t len = strlen(pcm_path);
   int ret;

   path = (char *)malloc(len + 6);
   if (!path)
      return -1;
   memcpy(path, pcm_path, len);
   memcpy(path + len, ".loss", 6);
   f = fopen(path, "wb");
   if (!f) {
      fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
      free(path);
      return -1;
   }
   ret = fwrite(lost, 1, (size_t)nframes, f) == (size_t)nframes ? 0 : -1;
   fclose(f);
   free(path);
   return ret;
}

static int decode_stream(const Packet *packets, int nframes, int frame_size,
                         const unsigned char *lost, int16_t *out,
                         const SpeexMode *mode)
{
   SpeexBits bits;
   void *dec;
   int frame;

   dec = speex_decoder_init(mode);
   if (!dec)
      return -1;
   speex_bits_init(&bits);

   for (frame = 0; frame < nframes; ++frame) {
      int ret;
      if (lost && lost[frame]) {
         ret = speex_decode_int(dec, NULL, out + frame * frame_size);
      } else {
         speex_bits_read_from(&bits, (const char *)packets[frame].data,
                              packets[frame].len);
         ret = speex_decode_int(dec, &bits, out + frame * frame_size);
      }
      if (ret != 0) {
         fprintf(stderr, "decode failed at frame %d: %d\n", frame, ret);
         speex_bits_destroy(&bits);
         speex_decoder_destroy(dec);
         return -1;
      }
   }

   speex_bits_destroy(&bits);
   speex_decoder_destroy(dec);
   return 0;
}

int main(int argc, char **argv)
{
   const SpeexMode *mode;
   void *enc;
   SpeexBits bits;
   int16_t *input = NULL;
   int16_t *clean = NULL;
   int16_t *lossy = NULL;
   Packet *packets = NULL;
   unsigned char *lost = NULL;
   int *loss_order = NULL;
   size_t nsamples = 0;
   int frame_size = 0;
   int quality = 8;
   int loss_percent = 20;
   int periodic_loss = 0;
   int nframes;
   int nlost;
   int i;
   int arg = 1;

   rng_state = 0x6d2b79f5u;
   mode = speex_lib_get_mode(SPEEX_MODEID_NB);

   while (arg < argc && argv[arg][0] == '-') {
      if (!strcmp(argv[arg], "-m") && arg + 1 < argc) {
         if (!strcmp(argv[arg + 1], "nb"))
            mode = speex_lib_get_mode(SPEEX_MODEID_NB);
         else if (!strcmp(argv[arg + 1], "wb"))
            mode = speex_lib_get_mode(SPEEX_MODEID_WB);
         else {
            usage(argv[0]);
            return 2;
         }
         arg += 2;
      } else if (!strcmp(argv[arg], "-q") && arg + 1 < argc) {
         quality = atoi(argv[arg + 1]);
         arg += 2;
      } else if (!strcmp(argv[arg], "-l") && arg + 1 < argc) {
         loss_percent = atoi(argv[arg + 1]);
         arg += 2;
      } else if (!strcmp(argv[arg], "-s") && arg + 1 < argc) {
         rng_state = (uint32_t)strtoul(argv[arg + 1], NULL, 0);
         if (rng_state == 0)
            rng_state = 1;
         arg += 2;
      } else if (!strcmp(argv[arg], "-p") && arg + 1 < argc) {
         if (!strcmp(argv[arg + 1], "uniform"))
            periodic_loss = 0;
         else if (!strcmp(argv[arg + 1], "periodic"))
            periodic_loss = 1;
         else {
            usage(argv[0]);
            return 2;
         }
         arg += 2;
      } else {
         usage(argv[0]);
         return 2;
      }
   }
   if (argc - arg != 3 || loss_percent < 0 || loss_percent > 100 ||
       quality < 0 || quality > 10) {
      usage(argv[0]);
      return 2;
   }

   if (read_pcm(argv[arg], &input, &nsamples) != 0)
      return 1;

   enc = speex_encoder_init(mode);
   if (!enc) {
      fprintf(stderr, "encoder init failed\n");
      return 1;
   }
   speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &quality);
   speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
   speex_bits_init(&bits);

   nframes = (int)(nsamples / frame_size);
   packets = (Packet *)calloc((size_t)nframes, sizeof(Packet));
   lost = (unsigned char *)calloc((size_t)nframes, 1);
   loss_order = (int *)malloc((size_t)nframes * sizeof(int));
   clean = (int16_t *)calloc((size_t)nframes * frame_size, sizeof(int16_t));
   lossy = (int16_t *)calloc((size_t)nframes * frame_size, sizeof(int16_t));
   if (!packets || !lost || !loss_order || !clean || !lossy) {
      fprintf(stderr, "out of memory\n");
      return 1;
   }

   for (i = 0; i < nframes; ++i) {
      int nbytes;
      if (speex_encode_int(enc, input + i * frame_size, &bits) < 0) {
         fprintf(stderr, "encode failed at frame %d\n", i);
         return 1;
      }
      nbytes = speex_bits_nbytes(&bits);
      if (nbytes <= 0 || nbytes > MAX_PACKET_BYTES) {
         fprintf(stderr, "invalid packet size %d at frame %d\n", nbytes, i);
         return 1;
      }
      packets[i].data = (unsigned char *)malloc((size_t)nbytes);
      if (!packets[i].data)
         return 1;
      packets[i].len = speex_bits_write(&bits, (char *)packets[i].data,
                                        MAX_PACKET_BYTES);
      speex_bits_reset(&bits);
      loss_order[i] = i;
   }
   speex_bits_destroy(&bits);
   speex_encoder_destroy(enc);

   nlost = (nframes * loss_percent + 50) / 100;
   if (periodic_loss) {
      for (i = 0; i < nlost; ++i)
         lost[(int)(((int64_t)(i + 1) * (nframes + 1)) / (nlost + 1))] = 1;
   } else {
      for (i = nframes - 1; i > 0; --i) {
         int j = (int)(next_rand() % (uint32_t)(i + 1));
         int tmp = loss_order[i];
         loss_order[i] = loss_order[j];
         loss_order[j] = tmp;
      }
      for (i = 0; i < nlost; ++i)
         lost[loss_order[i]] = 1;
   }

   if (decode_stream(packets, nframes, frame_size, NULL, clean, mode) != 0)
      return 1;
   if (decode_stream(packets, nframes, frame_size, lost, lossy, mode) != 0)
      return 1;

   if (write_pcm(argv[arg + 1], clean, (size_t)nframes * frame_size) != 0)
      return 1;
   if (write_pcm(argv[arg + 2], lossy, (size_t)nframes * frame_size) != 0)
      return 1;
   if (write_loss_mask(argv[arg + 2], lost, nframes) != 0)
      return 1;

   fprintf(stderr,
           "mode=%s quality=%d frames=%d frame_size=%d lost=%d (%.2f%%)\n",
           mode->modeName, quality, nframes, frame_size, nlost,
           100.0 * nlost / (nframes ? nframes : 1));

   for (i = 0; i < nframes; ++i)
      free(packets[i].data);
   free(packets);
   free(lost);
   free(loss_order);
   free(input);
   free(clean);
   free(lossy);
   return 0;
}
