/*
 * Differential-testing helper for Speex fixed-point and floating-point builds.
 *
 * This program is linked once against each build.  It deliberately uses the
 * integer API for both corpus generation and decoding so that the comparison
 * measures codec arithmetic, not float-to-int conversion differences.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speex/speex.h"
#include "speex/speex_bits.h"

#define MAX_FRAME_SAMPLES 2000
#define MAX_PACKET_BYTES 4096
#define MAX_CASES 20000

#define CORPUS_MAGIC "SPDIFF1\0"
#define RESULT_MAGIC "SPDRES1\0"
#define CORPUS_MAGIC_LEN 8
#define RESULT_MAGIC_LEN 8

typedef struct {
   int id;
   int mode;
   int quality;
   int complexity;
   int vbr;
   int vbr_quality;
   int highpass;
   int signal;
   int amplitude;
   int frequency;
   int switch_every;
   int switch_kind;
   int frames;
   int seed;
   int enh;
} CaseSpec;

typedef struct {
   uint32_t id;
   uint8_t mode;
   uint8_t enh;
   uint16_t frames;
} CorpusCaseHeader;

static uint32_t rng_next(uint32_t *state)
{
   *state = *state * 1664525u + 1013904223u;
   return *state;
}

static int16_t clamp_s16(double value)
{
   if (value > 32767.0)
      return 32767;
   if (value < -32768.0)
      return -32768;
   return (int16_t)floor(value + 0.5);
}

static int write_u16(FILE *f, uint16_t value)
{
   unsigned char buf[2];
   buf[0] = (unsigned char)(value & 0xff);
   buf[1] = (unsigned char)((value >> 8) & 0xff);
   return fwrite(buf, 1, sizeof(buf), f) == sizeof(buf) ? 0 : -1;
}

static int write_u32(FILE *f, uint32_t value)
{
   unsigned char buf[4];
   buf[0] = (unsigned char)(value & 0xff);
   buf[1] = (unsigned char)((value >> 8) & 0xff);
   buf[2] = (unsigned char)((value >> 16) & 0xff);
   buf[3] = (unsigned char)((value >> 24) & 0xff);
   return fwrite(buf, 1, sizeof(buf), f) == sizeof(buf) ? 0 : -1;
}

static int write_i32(FILE *f, int32_t value)
{
   return write_u32(f, (uint32_t)value);
}

static int read_u16(FILE *f, uint16_t *value)
{
   unsigned char buf[2];
   if (fread(buf, 1, sizeof(buf), f) != sizeof(buf))
      return -1;
   *value = (uint16_t)buf[0] | (uint16_t)((uint16_t)buf[1] << 8);
   return 0;
}

static int read_u32(FILE *f, uint32_t *value)
{
   unsigned char buf[4];
   if (fread(buf, 1, sizeof(buf), f) != sizeof(buf))
      return -1;
   *value = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
            ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
   return 0;
}

static int read_magic(FILE *f, const char *expected, size_t len)
{
   char buf[8];
   if (len > sizeof(buf) || fread(buf, 1, len, f) != len)
      return -1;
   return memcmp(buf, expected, len) ? -1 : 0;
}

static void write_magic(FILE *f, const char *magic, size_t len)
{
   fwrite(magic, 1, len, f);
}

static int apply_encoder_parameters(void *state, const CaseSpec *spec)
{
   spx_int32_t value;
   float vbr_quality;
   int rate;

   rate = 8000 << spec->mode;
   value = rate;
   if (speex_encoder_ctl(state, SPEEX_SET_SAMPLING_RATE, &value))
      return -1;
   value = spec->complexity;
   if (speex_encoder_ctl(state, SPEEX_SET_COMPLEXITY, &value))
      return -1;
   value = spec->vbr;
   if (speex_encoder_ctl(state, SPEEX_SET_VBR, &value))
      return -1;
   vbr_quality = (float)spec->vbr_quality;
   if (speex_encoder_ctl(state, SPEEX_SET_VBR_QUALITY, &vbr_quality))
      return -1;
   value = spec->highpass;
   if (speex_encoder_ctl(state, SPEEX_SET_HIGHPASS, &value))
      return -1;
   value = spec->quality;
   if (speex_encoder_ctl(state, SPEEX_SET_QUALITY, &value))
      return -1;
   return 0;
}

static int switch_encoder_parameters(void *state, const CaseSpec *spec, int switch_no,
                                     int *quality, int *complexity, int *vbr,
                                     int *highpass)
{
   spx_int32_t value;
   int next_quality;

   switch (spec->switch_kind)
   {
   case 0:
      next_quality = (*quality == spec->quality) ?
                     ((spec->quality + 5) % 11) : spec->quality;
      *quality = next_quality;
      value = next_quality;
      return speex_encoder_ctl(state, SPEEX_SET_QUALITY, &value);
   case 1:
      *vbr = !*vbr;
      value = *vbr;
      return speex_encoder_ctl(state, SPEEX_SET_VBR, &value);
   case 2:
      *complexity = (*complexity == spec->complexity) ?
                     ((spec->complexity + 5) % 11) : spec->complexity;
      value = *complexity;
      return speex_encoder_ctl(state, SPEEX_SET_COMPLEXITY, &value);
   case 3:
      *highpass = !*highpass;
      value = *highpass;
      return speex_encoder_ctl(state, SPEEX_SET_HIGHPASS, &value);
   case 4:
      next_quality = (*quality == spec->quality) ?
                     ((spec->quality + 5) % 11) : spec->quality;
      *quality = next_quality;
      value = next_quality;
      if (speex_encoder_ctl(state, SPEEX_SET_QUALITY, &value))
         return -1;
      *vbr = !*vbr;
      value = *vbr;
      return speex_encoder_ctl(state, SPEEX_SET_VBR, &value);
   case 5:
      *quality = (switch_no & 1) ? 10 : 0;
      value = *quality;
      return speex_encoder_ctl(state, SPEEX_SET_QUALITY, &value);
   default:
      return -1;
   }
}

static void fill_frame(const CaseSpec *spec, int frame_index, int frame_size,
                       int16_t *samples, uint32_t *noise_state, double *phase,
                       int64_t *sample_index)
{
   int i;
   double rate = (double)(8000 << spec->mode);
   double amplitude = (double)spec->amplitude;
   double frequency = (double)spec->frequency;
   int silent = 0;

   if (spec->signal == 0)
      silent = 1;
   else if (spec->signal == 4)
      silent = (frame_index % 5 == 0);

   for (i = 0; i < frame_size; i++)
   {
      double value = 0.0;
      if (silent)
      {
         samples[i] = 0;
         continue;
      }

      switch (spec->signal)
      {
      case 1:
      case 4:
         value = amplitude * sin(*phase);
         *phase += 2.0 * 3.14159265358979323846 * frequency / rate;
         if (*phase > 2.0 * 3.14159265358979323846)
            *phase -= 2.0 * 3.14159265358979323846;
         if (spec->signal == 4)
         {
            uint32_t r = rng_next(noise_state);
            value += amplitude * 0.18 * (((double)(r & 0xffff) / 32767.5) - 1.0);
         }
         break;
      case 2:
      {
         uint32_t r = rng_next(noise_state);
         value = amplitude * (((double)(r & 0xffff) / 32767.5) - 1.0);
         break;
      }
      case 3:
         value = amplitude * sin(*phase);
         *phase += 2.0 * 3.14159265358979323846 *
                   (400.0 + 80.0 * (double)(*sample_index) / rate) / rate;
         if (*phase > 2.0 * 3.14159265358979323846)
            *phase -= 2.0 * 3.14159265358979323846;
         break;
      default:
         silent = 1;
         break;
      }

      if (silent)
         samples[i] = 0;
      else
         samples[i] = clamp_s16(value);
      (*sample_index)++;
   }
}

static int parse_case_line(char *line, CaseSpec *spec)
{
   int fields;
   int values[15];

   if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
      return 0;
   fields = sscanf(line, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                   &values[0], &values[1], &values[2], &values[3],
                   &values[4], &values[5], &values[6], &values[7],
                   &values[8], &values[9], &values[10], &values[11],
                   &values[12], &values[13], &values[14]);
   if (fields != 15)
      return -1;

   spec->id = values[0];
   spec->mode = values[1];
   spec->quality = values[2];
   spec->complexity = values[3];
   spec->vbr = values[4];
   spec->vbr_quality = values[5];
   spec->highpass = values[6];
   spec->signal = values[7];
   spec->amplitude = values[8];
   spec->frequency = values[9];
   spec->switch_every = values[10];
   spec->switch_kind = values[11];
   spec->frames = values[12];
   spec->seed = values[13];
   spec->enh = values[14];

   if (spec->id < 0 ||
       spec->mode < 0 || spec->mode >= SPEEX_NB_MODES ||
       spec->quality < 0 || spec->quality > 10 ||
       spec->complexity < 0 || spec->complexity > 10 ||
       (spec->vbr != 0 && spec->vbr != 1) ||
       spec->vbr_quality < 0 || spec->vbr_quality > 10 ||
       (spec->highpass != 0 && spec->highpass != 1) ||
       spec->signal < 0 || spec->signal > 4 ||
       spec->amplitude < 0 || spec->amplitude > 32767 ||
       spec->frequency < 0 ||
       spec->switch_every < 0 ||
       spec->switch_kind < 0 || spec->switch_kind > 5 ||
       spec->frames < 1 || spec->frames > 255 ||
       (spec->enh != 0 && spec->enh != 1))
      return -1;
   return 1;
}

static int cmd_info(void)
{
   const char *version = NULL;
   speex_lib_ctl(SPEEX_LIB_GET_VERSION_STRING, (void *)&version);
#ifdef FIXED_POINT
   printf("{\"arithmetic\":\"fixed\",\"version\":\"%s\"}\n", version ? version : "?");
#else
   printf("{\"arithmetic\":\"float\",\"version\":\"%s\"}\n", version ? version : "?");
#endif
   return 0;
}

static int load_specs(const char *path, CaseSpec **specs_out, int *count_out)
{
   FILE *f;
   char line[512];
   CaseSpec *specs = NULL;
   int count = 0;
   int capacity = 0;

   f = fopen(path, "r");
   if (!f)
   {
      fprintf(stderr, "cannot open cases file %s: %s\n", path, strerror(errno));
      return -1;
   }
   while (fgets(line, sizeof(line), f))
   {
      int parsed;
      CaseSpec spec;
      parsed = parse_case_line(line, &spec);
      if (parsed < 0)
      {
         const char *end = line + strlen(line);
         while (end > line && (end[-1] == '\n' || end[-1] == '\r'))
            end--;
         fprintf(stderr, "invalid case line: %.*s\n", (int)(end - line), line);
         free(specs);
         fclose(f);
         return -1;
      }
      if (parsed == 0)
         continue;
      if (count == capacity)
      {
         int next = capacity ? capacity * 2 : 256;
         CaseSpec *tmp;
         if (next > MAX_CASES)
            next = MAX_CASES;
         if (next <= capacity)
         {
            fprintf(stderr, "too many cases (limit %d)\n", MAX_CASES);
            free(specs);
            fclose(f);
            return -1;
         }
         tmp = (CaseSpec *)realloc(specs, (size_t)next * sizeof(*tmp));
         if (!tmp)
         {
            fprintf(stderr, "out of memory\n");
            free(specs);
            fclose(f);
            return -1;
         }
         specs = tmp;
         capacity = next;
      }
      specs[count++] = spec;
   }
   fclose(f);
   *specs_out = specs;
   *count_out = count;
   return 0;
}

static int cmd_generate(const char *cases_path, const char *out_path)
{
   CaseSpec *specs = NULL;
   int count = 0;
   int c;
   FILE *out = NULL;
   int ret = -1;

   if (load_specs(cases_path, &specs, &count))
      return -1;
   out = fopen(out_path, "wb");
   if (!out)
   {
      fprintf(stderr, "cannot open corpus %s: %s\n", out_path, strerror(errno));
      free(specs);
      return -1;
   }
   write_magic(out, CORPUS_MAGIC, CORPUS_MAGIC_LEN);
   if (write_u32(out, (uint32_t)count))
      goto done;

   for (c = 0; c < count; c++)
   {
      const CaseSpec *spec = &specs[c];
      void *enc;
      SpeexBits bits;
      spx_int16_t input[MAX_FRAME_SAMPLES];
      char packet[MAX_PACKET_BYTES];
      spx_int32_t frame_size = 0;
      uint32_t noise_state;
      double phase = 0.0;
      int64_t sample_index = 0;
      int quality;
      int complexity;
      int vbr;
      int highpass;
      int frame;

      enc = speex_encoder_init(speex_lib_get_mode(spec->mode));
      if (!enc)
      {
         fprintf(stderr, "encoder init failed for case %d\n", spec->id);
         goto done;
      }
      if (apply_encoder_parameters(enc, spec) ||
          speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size))
      {
         fprintf(stderr, "encoder setup failed for case %d\n", spec->id);
         speex_encoder_destroy(enc);
         goto done;
      }
      if (frame_size <= 0 || frame_size > MAX_FRAME_SAMPLES)
      {
         fprintf(stderr, "invalid frame size %d for case %d\n", frame_size, spec->id);
         speex_encoder_destroy(enc);
         goto done;
      }

      speex_bits_init(&bits);
      noise_state = (uint32_t)spec->seed ^ 0x9e3779b9u;
      quality = spec->quality;
      complexity = spec->complexity;
      vbr = spec->vbr;
      highpass = spec->highpass;

      if (write_u32(out, (uint32_t)spec->id) ||
          fputc(spec->mode, out) == EOF ||
          fputc(spec->enh, out) == EOF ||
          write_u16(out, (uint16_t)spec->frames))
      {
         speex_bits_destroy(&bits);
         speex_encoder_destroy(enc);
         goto done;
      }

      for (frame = 0; frame < spec->frames; frame++)
      {
         int packet_bytes;
         if (frame > 0 && spec->switch_every > 0 &&
             (frame % spec->switch_every) == 0)
         {
            if (switch_encoder_parameters(enc, spec, frame / spec->switch_every,
                                          &quality, &complexity, &vbr, &highpass))
            {
               fprintf(stderr, "parameter switch failed for case %d frame %d\n",
                       spec->id, frame);
               speex_bits_destroy(&bits);
               speex_encoder_destroy(enc);
               goto done;
            }
         }
         fill_frame(spec, frame, frame_size, input, &noise_state, &phase,
                    &sample_index);
         speex_bits_reset(&bits);
         speex_encode_int(enc, input, &bits);
         packet_bytes = speex_bits_write(&bits, packet, MAX_PACKET_BYTES);
         if (packet_bytes <= 0 || packet_bytes > MAX_PACKET_BYTES)
         {
            fprintf(stderr, "invalid packet size %d for case %d frame %d\n",
                    packet_bytes, spec->id, frame);
            speex_bits_destroy(&bits);
            speex_encoder_destroy(enc);
            goto done;
         }
         if (write_u32(out, (uint32_t)packet_bytes) ||
             fwrite(packet, 1, (size_t)packet_bytes, out) != (size_t)packet_bytes)
         {
            speex_bits_destroy(&bits);
            speex_encoder_destroy(enc);
            goto done;
         }
      }
      speex_bits_destroy(&bits);
      speex_encoder_destroy(enc);
   }
   ret = 0;
done:
   if (out)
      fclose(out);
   free(specs);
   return ret;
}

static int cmd_decode(const char *corpus_path, const char *out_path)
{
   FILE *in = NULL;
   FILE *out = NULL;
   uint32_t count;
   uint32_t c;
   int ret = -1;

   in = fopen(corpus_path, "rb");
   if (!in)
   {
      fprintf(stderr, "cannot open corpus %s: %s\n", corpus_path, strerror(errno));
      return -1;
   }
   if (read_magic(in, CORPUS_MAGIC, CORPUS_MAGIC_LEN) ||
       read_u32(in, &count))
   {
      fprintf(stderr, "invalid corpus header in %s\n", corpus_path);
      fclose(in);
      return -1;
   }
   out = fopen(out_path, "wb");
   if (!out)
   {
      fprintf(stderr, "cannot open result %s: %s\n", out_path, strerror(errno));
      fclose(in);
      return -1;
   }
   write_magic(out, RESULT_MAGIC, RESULT_MAGIC_LEN);
   if (write_u32(out, count))
      goto done;

   for (c = 0; c < count; c++)
   {
      CorpusCaseHeader header;
      void *dec = NULL;
      SpeexBits bits;
      spx_int32_t frame_size = 0;
      int enh;
      int frame;

      if (read_u32(in, &header.id) ||
          fread(&header.mode, 1, 1, in) != 1 ||
          fread(&header.enh, 1, 1, in) != 1 ||
          read_u16(in, &header.frames))
      {
         fprintf(stderr, "truncated corpus header for case %u\n", c);
         goto done;
      }
      if (header.mode >= SPEEX_NB_MODES)
      {
         fprintf(stderr, "invalid mode %u for case %u\n", header.mode, header.id);
         goto done;
      }

      dec = speex_decoder_init(speex_lib_get_mode(header.mode));
      if (!dec)
      {
         fprintf(stderr, "decoder init failed for case %u\n", header.id);
         goto done;
      }
      enh = header.enh != 0;
      speex_decoder_ctl(dec, SPEEX_SET_ENH, &enh);
      if (speex_decoder_ctl(dec, SPEEX_GET_FRAME_SIZE, &frame_size) ||
          frame_size <= 0 || frame_size > MAX_FRAME_SAMPLES)
      {
         fprintf(stderr, "invalid decoder frame size for case %u\n", header.id);
         speex_decoder_destroy(dec);
         goto done;
      }
      speex_bits_init(&bits);

      if (write_u32(out, header.id) ||
          fputc(header.mode, out) == EOF ||
          fputc(header.enh, out) == EOF ||
          write_u16(out, header.frames))
      {
         speex_bits_destroy(&bits);
         speex_decoder_destroy(dec);
         goto done;
      }

      for (frame = 0; frame < header.frames; frame++)
      {
         uint32_t packet_bytes;
         char packet[MAX_PACKET_BYTES];
         spx_int16_t output[MAX_FRAME_SAMPLES];
         int decode_ret;
         int overflow;
         uint32_t written_samples = 0;
         uint16_t packet_u16;

         if (read_u32(in, &packet_bytes) ||
             packet_bytes > MAX_PACKET_BYTES ||
             fread(packet, 1, packet_bytes, in) != packet_bytes)
         {
            fprintf(stderr, "truncated packet for case %u frame %d\n",
                    header.id, frame);
            speex_bits_destroy(&bits);
            speex_decoder_destroy(dec);
            goto done;
         }
         memset(output, 0, sizeof(output));
         speex_bits_read_from(&bits, packet, (int)packet_bytes);
         decode_ret = speex_decode_int(dec, &bits, output);
         overflow = bits.overflow != 0;
         if (decode_ret == 0)
            written_samples = (uint32_t)frame_size;

         packet_u16 = (uint16_t)frame;
         if (write_u32(out, header.id) ||
             write_u16(out, packet_u16) ||
             write_i32(out, decode_ret) ||
             fputc(overflow, out) == EOF ||
             fputc(0, out) == EOF ||
             write_u16(out, 0) ||
             write_u32(out, written_samples))
         {
            speex_bits_destroy(&bits);
            speex_decoder_destroy(dec);
            goto done;
         }
         if (written_samples)
         {
            int s;
            for (s = 0; s < (int)written_samples; s++)
            {
               if (write_u16(out, (uint16_t)output[s]))
               {
                  speex_bits_destroy(&bits);
                  speex_decoder_destroy(dec);
                  goto done;
               }
            }
         }
      }
      speex_bits_destroy(&bits);
      speex_decoder_destroy(dec);
   }
   ret = 0;
done:
   if (out)
      fclose(out);
   fclose(in);
   return ret;
}

static void usage(const char *argv0)
{
   fprintf(stderr,
           "usage:\n"
           "  %s info\n"
           "  %s gen CASES.txt CORPUS.bin\n"
           "  %s decode CORPUS.bin RESULTS.bin\n",
           argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "info") == 0)
      return cmd_info();
   if (argc == 4 && strcmp(argv[1], "gen") == 0)
      return cmd_generate(argv[2], argv[3]) ? 1 : 0;
   if (argc == 4 && strcmp(argv[1], "decode") == 0)
      return cmd_decode(argv[2], argv[3]) ? 1 : 0;
   usage(argv[0]);
   return 2;
}
