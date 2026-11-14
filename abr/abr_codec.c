#include "abr/abr_codec.h"

#include <speex/speex.h>
#include <speex/speex_bits.h>

#include <stdlib.h>
#include <string.h>

/* Target bit rates in bps, indexed by abr_tier. SPEEX_SET_BITRATE maps each
   target onto the closest sub-mode whose rate does not exceed it, which gives
   5950 / 11000 / 24600 bps in the narrowband mode. */
static const int kTierTargetBps[ABR_TIER_COUNT] = {6000, 11000, 25000};

/* Nominal rates reported back to callers. */
static const int kTierNominalBps[ABR_TIER_COUNT] = {5950, 11000, 24600};

struct abr_encoder {
   void *state;
   SpeexBits bits;
   abr_tier tier;
};

struct abr_decoder {
   void *state;
   SpeexBits bits;
};

static int tier_valid(abr_tier tier)
{
   return (int)tier >= 0 && (int)tier < ABR_TIER_COUNT;
}

abr_encoder *abr_encoder_open(abr_tier initial_tier)
{
   abr_encoder *enc;
   spx_int32_t vbr_off = 0;

   if (!tier_valid(initial_tier))
      return NULL;

   enc = (abr_encoder *)calloc(1, sizeof(*enc));
   if (!enc)
      return NULL;

   enc->state = speex_encoder_init(speex_lib_get_mode(SPEEX_MODEID_NB));
   if (!enc->state) {
      free(enc);
      return NULL;
   }

   /* Constant bit rate per tier: tier switches, not VBR, drive the rate. */
   speex_encoder_ctl(enc->state, SPEEX_SET_VBR, &vbr_off);
   speex_bits_init(&enc->bits);
   enc->tier = initial_tier;

   if (abr_encoder_set_tier(enc, initial_tier) != ABR_OK) {
      speex_bits_destroy(&enc->bits);
      speex_encoder_destroy(enc->state);
      free(enc);
      return NULL;
   }
   return enc;
}

void abr_encoder_close(abr_encoder *enc)
{
   if (!enc)
      return;
   speex_bits_destroy(&enc->bits);
   speex_encoder_destroy(enc->state);
   free(enc);
}

abr_status abr_encoder_set_tier(abr_encoder *enc, abr_tier tier)
{
   spx_int32_t target;
   if (!enc)
      return ABR_ERR_ARG;
   if (!tier_valid(tier))
      return ABR_ERR_TIER;

   /* Only the sub-mode selection changes; no SPEEX_RESET_STATE, so the
      predictor/pitch memories carry across the switch untouched. */
   target = kTierTargetBps[tier];
   speex_encoder_ctl(enc->state, SPEEX_SET_BITRATE, &target);
   enc->tier = tier;
   return ABR_OK;
}

abr_status abr_encoder_get_tier(const abr_encoder *enc, abr_tier *tier)
{
   if (!enc || !tier)
      return ABR_ERR_ARG;
   *tier = enc->tier;
   return ABR_OK;
}

abr_status abr_tier_bitrate(abr_tier tier, int *bitrate_bps)
{
   if (!tier_valid(tier) || !bitrate_bps)
      return ABR_ERR_ARG;
   *bitrate_bps = kTierNominalBps[tier];
   return ABR_OK;
}

abr_status abr_encoder_encode(abr_encoder *enc,
                              const spx_int16_t *frame,
                              unsigned char *packet,
                              size_t *packet_len)
{
   unsigned char payload[ABR_MAX_PACKET - 1];
   int payload_len;
   size_t capacity;

   if (!enc || !frame || !packet || !packet_len)
      return ABR_ERR_ARG;

   capacity = *packet_len;
   if (capacity < 2)
      return ABR_ERR_NOSPACE;

   speex_bits_reset(&enc->bits);
   if (speex_encode_int(enc->state, (spx_int16_t *)frame, &enc->bits) != 1)
      return ABR_ERR_CORRUPT;

   payload_len = speex_bits_write(&enc->bits, (char *)payload,
                                  (int)sizeof(payload));
   if (payload_len <= 0 || payload_len > 255 ||
       (size_t)payload_len + 1 > capacity)
      return ABR_ERR_NOSPACE;

   packet[0] = (unsigned char)payload_len;
   memcpy(packet + 1, payload, (size_t)payload_len);
   *packet_len = (size_t)payload_len + 1;
   return ABR_OK;
}

abr_decoder *abr_decoder_open(void)
{
   abr_decoder *dec = (abr_decoder *)calloc(1, sizeof(*dec));
   if (!dec)
      return NULL;

   dec->state = speex_decoder_init(speex_lib_get_mode(SPEEX_MODEID_NB));
   if (!dec->state) {
      free(dec);
      return NULL;
   }
   speex_bits_init(&dec->bits);
   return dec;
}

void abr_decoder_close(abr_decoder *dec)
{
   if (!dec)
      return;
   speex_bits_destroy(&dec->bits);
   speex_decoder_destroy(dec->state);
   free(dec);
}

abr_status abr_decoder_decode(abr_decoder *dec,
                              const unsigned char *packet,
                              size_t packet_len,
                              spx_int16_t *frame)
{
   size_t payload_len;

   if (!dec || !packet || !frame)
      return ABR_ERR_ARG;
   if (packet_len < 2)
      return ABR_ERR_CORRUPT;

   payload_len = packet[0];
   if (payload_len == 0 || payload_len + 1 != packet_len)
      return ABR_ERR_CORRUPT;

   speex_bits_read_from(&dec->bits, (const char *)(packet + 1),
                        (int)payload_len);
   if (speex_decode_int(dec->state, &dec->bits, frame) != 0)
      return ABR_ERR_CORRUPT;
   return ABR_OK;
}
