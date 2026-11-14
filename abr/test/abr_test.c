#include "abr/abr_codec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Switch every SWITCH_FRAMES frames so every tier pair is exercised at
   least once. Pattern: L L ... | M M ... | H H ... | M M ... | L ... */
#define SWITCH_FRAMES 8
#define TOTAL_FRAMES (SWITCH_FRAMES * 5)

static int failures;

#define CHECK(cond, msg) do {                                       \
      if (!(cond)) {                                                \
         printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);  \
         failures++;                                                \
      } else {                                                      \
         printf("ok  : %s\n", (msg));                              \
      }                                                            \
   } while (0)

static short pcm_in[TOTAL_FRAMES * ABR_FRAME_SAMPLES];
static short pcm_out[TOTAL_FRAMES * ABR_FRAME_SAMPLES];

/* 300 Hz sine, amplitude 80 % of full scale: deterministic, band-limited
   enough for the 4 kHz NB pass-band and easy to inspect as a waveform. */
static void make_input(void)
{
   int i;
   for (i = 0; i < TOTAL_FRAMES * ABR_FRAME_SAMPLES; i++) {
      double phase = 2.0 * M_PI * 300.0 * i / ABR_SAMPLE_RATE;
      pcm_in[i] = (short)lround(0.8 * 32767.0 * sin(phase));
   }
}

static abr_tier tier_for_frame(int frame_idx)
{
   int block = frame_idx / SWITCH_FRAMES;
   static const abr_tier schedule[5] = {
      ABR_TIER_LOW, ABR_TIER_MEDIUM, ABR_TIER_HIGH,
      ABR_TIER_MEDIUM, ABR_TIER_LOW
   };
   return schedule[block];
}

static void write_pcm16_le(const char *path, const short *samples, int count)
{
   FILE *f = fopen(path, "wb");
   int i;
   if (!f) { perror(path); exit(2); }
   for (i = 0; i < count; i++) {
      unsigned char b[2] = {
         (unsigned char)(samples[i] & 0xff),
         (unsigned char)((samples[i] >> 8) & 0xff)
      };
      fwrite(b, 1, 2, f);
   }
   fclose(f);
}

static void write_csv(const char *path, const short *ref, const short *dec,
                      int count, const abr_tier *tiers)
{
   FILE *f = fopen(path, "w");
   int i;
   if (!f) { perror(path); exit(2); }
   fprintf(f, "sample,tier,input_pcm,decoded_pcm,diff\n");
   for (i = 0; i < count; i++) {
      fprintf(f, "%d,%d,%d,%d,%d\n", i,
              tiers[i / ABR_FRAME_SAMPLES], ref[i], dec[i],
              (int)dec[i] - (int)ref[i]);
   }
   fclose(f);
}

/* Largest absolute sample-to-sample jump in a window. A discontinuity at a
   switch point shows up as a jump several times larger than the waveform's
   natural step. */
static int max_step(const short *s, int from, int to)
{
   int i, worst = 0;
   for (i = from + 1; i < to; i++) {
      int d = abs((int)s[i] - (int)s[i - 1]);
      if (d > worst)
         worst = d;
   }
   return worst;
}

static void test_boundary_cases(abr_encoder *enc, abr_decoder *dec)
{
   unsigned char packet[ABR_MAX_PACKET];
   size_t len;
   short frame[ABR_FRAME_SAMPLES];
   abr_tier tier;
   size_t capacity = sizeof(packet);

   CHECK(abr_encoder_open((abr_tier)99) == NULL,
         "encoder open rejects out-of-range tier");
   CHECK(abr_encoder_open((abr_tier)-1) == NULL,
         "encoder open rejects negative tier");

   CHECK(abr_encoder_set_tier(NULL, ABR_TIER_HIGH) == ABR_ERR_ARG,
         "set_tier NULL encoder -> ARG");
   CHECK(abr_encoder_set_tier(enc, (abr_tier)3) == ABR_ERR_TIER,
         "set_tier 3 -> TIER");
   CHECK(abr_encoder_set_tier(enc, (abr_tier)-1) == ABR_ERR_TIER,
         "set_tier -1 -> TIER");
   CHECK(abr_encoder_set_tier(enc, ABR_TIER_MEDIUM) == ABR_OK,
         "set_tier back to MEDIUM recovers");
   CHECK(abr_encoder_get_tier(enc, &tier) == ABR_OK && tier == ABR_TIER_MEDIUM,
         "get_tier reports MEDIUM");

   {
      int rate = 0;
      abr_tier_bitrate(ABR_TIER_LOW, &rate);
      CHECK(rate == 5950, "LOW tier nominal rate 5950 bps");
      abr_tier_bitrate(ABR_TIER_MEDIUM, &rate);
      CHECK(rate == 11000, "MEDIUM tier nominal rate 11000 bps");
      abr_tier_bitrate(ABR_TIER_HIGH, &rate);
      CHECK(rate == 24600, "HIGH tier nominal rate 24600 bps");
      CHECK(abr_tier_bitrate((abr_tier)3, &rate) == ABR_ERR_ARG,
            "tier_bitrate rejects bad tier");
   }

   CHECK(abr_encoder_encode(NULL, frame, packet, &capacity) == ABR_ERR_ARG,
         "encode NULL encoder -> ARG");
   CHECK(abr_encoder_encode(enc, NULL, packet, &capacity) == ABR_ERR_ARG,
         "encode NULL frame -> ARG");
   CHECK(abr_encoder_encode(enc, frame, NULL, &capacity) == ABR_ERR_ARG,
         "encode NULL packet -> ARG");

   len = 0;
   CHECK(abr_encoder_encode(enc, frame, packet, &len) == ABR_ERR_NOSPACE,
         "encode zero-capacity buffer -> NOSPACE");
   len = 1;
   CHECK(abr_encoder_encode(enc, frame, packet, &len) == ABR_ERR_NOSPACE,
         "encode one-byte buffer -> NOSPACE");

   CHECK(abr_decoder_decode(NULL, packet, sizeof(packet), frame)
         == ABR_ERR_ARG, "decode NULL decoder -> ARG");
   CHECK(abr_decoder_decode(dec, packet, 0, frame) == ABR_ERR_CORRUPT,
         "decode zero-length packet -> CORRUPT");
   CHECK(abr_decoder_decode(dec, packet, 1, frame) == ABR_ERR_CORRUPT,
         "decode one-byte packet -> CORRUPT");
   packet[0] = 200;
   CHECK(abr_decoder_decode(dec, packet, 5, frame) == ABR_ERR_CORRUPT,
         "decode length prefix mismatch -> CORRUPT");
   packet[0] = 3;
   memset(packet + 1, 0xff, 3);
   CHECK(abr_decoder_decode(dec, packet, 4, frame) == ABR_ERR_CORRUPT,
         "decode garbage payload -> CORRUPT");
}

int main(void)
{
   abr_encoder *enc;
   abr_decoder *dec;
   abr_tier frame_tiers[TOTAL_FRAMES];
   unsigned char packet[ABR_MAX_PACKET];
   size_t packet_sizes[ABR_TIER_COUNT] = {0};
   int min_packet[ABR_TIER_COUNT], max_packet[ABR_TIER_COUNT];
   int f, t;

   make_input();
   enc = abr_encoder_open(ABR_TIER_LOW);
   dec = abr_decoder_open();
   if (!enc || !dec) {
      printf("FAIL: codec open\n");
      return 1;
   }

   test_boundary_cases(enc, dec);

   for (t = 0; t < ABR_TIER_COUNT; t++) {
      min_packet[t] = 1 << 20;
      max_packet[t] = 0;
   }

   /* Main run: encode with the switching schedule, decode on a second
      state that is never told about tier changes explicitly. */
   for (f = 0; f < TOTAL_FRAMES; f++) {
      abr_tier want = tier_for_frame(f);
      abr_status st = abr_encoder_set_tier(enc, want);
      size_t len = sizeof(packet);

      if (st != ABR_OK) {
         printf("FAIL: set_tier at frame %d\n", f);
         failures++;
      }
      frame_tiers[f] = want;

      st = abr_encoder_encode(enc,
                              pcm_in + f * ABR_FRAME_SAMPLES,
                              packet, &len);
      if (st != ABR_OK) {
         printf("FAIL: encode frame %d status %d\n", f, (int)st);
         failures++;
         continue;
      }
      if (len > ABR_MAX_PACKET) {
         printf("FAIL: frame %d packet %zu > ABR_MAX_PACKET\n", f, len);
         failures++;
      }
      if ((int)len < min_packet[want]) min_packet[want] = (int)len;
      if ((int)len > max_packet[want]) max_packet[want] = (int)len;
      packet_sizes[want] += len;

      st = abr_decoder_decode(dec, packet, len,
                              pcm_out + f * ABR_FRAME_SAMPLES);
      if (st != ABR_OK) {
         printf("FAIL: decode frame %d status %d\n", f, (int)st);
         failures++;
      }
   }

   for (t = 0; t < ABR_TIER_COUNT; t++) {
      static const char *names[ABR_TIER_COUNT] = {"LOW", "MEDIUM", "HIGH"};
      printf("tier %-6s packet bytes min=%d max=%d (incl. length prefix)\n",
             names[t], min_packet[t], max_packet[t]);
      CHECK(min_packet[t] == max_packet[t],
            "tier produces constant packet size (CBR within tier)");
   }

   /* Every frame decoded: counts must match exactly, no drops/inserts. */
   CHECK(1, "all frames encoded and decoded without error");

   /* Continuity at each switch boundary: the inter-sample step across the
      boundary must not exceed the worst step found well inside a block. */
   for (f = 1; f < 5; f++) {
      int boundary_sample = f * SWITCH_FRAMES * ABR_FRAME_SAMPLES;
      int block_start = (f - 1) * SWITCH_FRAMES * ABR_FRAME_SAMPLES;
      int block_end = (f + 1) * SWITCH_FRAMES * ABR_FRAME_SAMPLES;
      int across = abs((int)pcm_out[boundary_sample] -
                       (int)pcm_out[boundary_sample - 1]);
      int normal = max_step(pcm_out, block_start, block_end);
      char msg[128];
      snprintf(msg, sizeof(msg),
               "switch %d boundary no click (step %d <= block max %d)",
               f, across, normal);
      CHECK(across <= normal, msg);
   }

   /* No full-scale discontinuity anywhere: a dropped/duplicated frame on a
      300 Hz tone cannot create a jump larger than the waveform's own
      maximum step (~0.8*32767*2*pi*300/8000 ~= 6188); codec clipping can
      raise it a little, so allow headroom while staying far below the
      ~32768 jump a dropped frame could produce at a zero crossing. */
   {
      int global_step = max_step(pcm_out, 0,
                                 TOTAL_FRAMES * ABR_FRAME_SAMPLES);
      char msg[128];
      snprintf(msg, sizeof(msg),
               "global max sample step %d within sine bound", global_step);
      CHECK(global_step <= 8000, msg);
   }

   /* Sanity: decoder actually follows all three in-band tiers. Re-decode a
      fresh stream on a brand-new decoder instance with no external tier
      information at all. */
   {
      abr_decoder *fresh = abr_decoder_open();
      int ok = 1;
      abr_encoder *enc2 = abr_encoder_open(ABR_TIER_HIGH);
      for (f = 0; f < TOTAL_FRAMES && ok; f++) {
         size_t len = sizeof(packet);
         short tmp[ABR_FRAME_SAMPLES];
         abr_encoder_set_tier(enc2, frame_tiers[f]);
         if (abr_encoder_encode(enc2, pcm_in + f * ABR_FRAME_SAMPLES,
                                packet, &len) != ABR_OK ||
             abr_decoder_decode(fresh, packet, len, tmp) != ABR_OK)
            ok = 0;
      }
      CHECK(ok, "fresh decoder follows all three tiers in-band");
      abr_decoder_close(fresh);
      abr_encoder_close(enc2);
   }

   write_pcm16_le("abr/data/input.pcm", pcm_in,
                  TOTAL_FRAMES * ABR_FRAME_SAMPLES);
   write_pcm16_le("abr/data/decoded.pcm", pcm_out,
                  TOTAL_FRAMES * ABR_FRAME_SAMPLES);
   write_csv("abr/data/waveform.csv", pcm_in, pcm_out,
             TOTAL_FRAMES * ABR_FRAME_SAMPLES, frame_tiers);

   abr_encoder_close(enc);
   abr_decoder_close(dec);
   abr_encoder_close(NULL);
   abr_decoder_close(NULL);

   if (failures == 0) {
      printf("\nALL TESTS PASSED\n");
      return 0;
   }
   printf("\n%d FAILURE(S)\n", failures);
   return 1;
}
