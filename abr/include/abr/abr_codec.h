#ifndef ABR_CODEC_H
#define ABR_CODEC_H
/**
   @file abr_codec.h
   @brief Narrow adaptive-bit-rate (ABR) interface over Speex narrowband.

   Exactly three bit-rate tiers are exposed. The active tier can be changed
   at run time, between any two frames, without resetting codec state. Every
   encoded packet is self-describing: the in-band Speex mode id lets the
   decoder follow tier changes with no out-of-band signalling, so no frame is
   dropped and the decoded sample stream stays continuous across switches.
*/

#include <stddef.h>
#include <speex/speex_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sample rate of the narrowband mode, in Hz. */
#define ABR_SAMPLE_RATE 8000

/** PCM samples per codec frame (20 ms at 8 kHz). */
#define ABR_FRAME_SAMPLES 160

/** Number of selectable bit-rate tiers. */
#define ABR_TIER_COUNT 3

/** Worst-case packet size (one-byte length prefix included), in bytes. */
#define ABR_MAX_PACKET 70

/** API status codes. */
typedef enum {
   ABR_OK = 0,
   ABR_ERR_ARG = -1,       /**< NULL handle/buffer or bad length */
   ABR_ERR_TIER = -2,      /**< tier id outside [0, ABR_TIER_COUNT) */
   ABR_ERR_NOSPACE = -3,   /**< output packet buffer too small */
   ABR_ERR_CORRUPT = -4    /**< decoder rejected the packet */
} abr_status;

/** Bit-rate tiers, ordered low -> medium -> high. */
typedef enum {
   ABR_TIER_LOW = 0,    /**< 5950 bps  (Speex NB submode 2) */
   ABR_TIER_MEDIUM = 1, /**< 11000 bps (Speex NB submode 4) */
   ABR_TIER_HIGH = 2    /**< 24600 bps (Speex NB submode 7) */
} abr_tier;

typedef struct abr_encoder abr_encoder;
typedef struct abr_decoder abr_decoder;

/**
 * Create an encoder pinned to the narrowband mode.
 * @param initial_tier tier used until the first abr_encoder_set_tier() call
 * @return heap-allocated encoder, or NULL on a bad tier / allocation failure
 */
abr_encoder *abr_encoder_open(abr_tier initial_tier);

/** Free an encoder created by abr_encoder_open(). NULL is accepted. */
void abr_encoder_close(abr_encoder *enc);

/**
 * Change the bit-rate tier. Takes effect on the next abr_encoder_encode()
 * call; the call does not touch codec memory, so samples spanning the
 * switch point remain continuous.
 */
abr_status abr_encoder_set_tier(abr_encoder *enc, abr_tier tier);

/** Return the tier that will be used by the next encode call. */
abr_status abr_encoder_get_tier(const abr_encoder *enc, abr_tier *tier);

/** Return the nominal bit rate in bps of a tier (5950/11000/24600). */
abr_status abr_tier_bitrate(abr_tier tier, int *bitrate_bps);

/**
 * Encode one 160-sample frame.
 * @param frame      exactly ABR_FRAME_SAMPLES 16-bit PCM samples
 * @param packet     output buffer; ABR_MAX_PACKET bytes always suffices
 * @param packet_len in: capacity of packet; out: bytes written (>= 1)
 *
 * The packet is one length-prefixed Speex frame; the prefix allows a byte
 * stream to be re-framed, while the in-band Speex mode id carries the tier.
 */
abr_status abr_encoder_encode(abr_encoder *enc,
                              const spx_int16_t *frame,
                              unsigned char *packet,
                              size_t *packet_len);

/**
 * Create a decoder. A single decoder consumes streams that may contain
 * packets produced under any tier, in any order.
 */
abr_decoder *abr_decoder_open(void);

/** Free a decoder created by abr_decoder_open(). NULL is accepted. */
void abr_decoder_close(abr_decoder *dec);

/**
 * Decode one packet produced by abr_encoder_encode().
 * @param frame output buffer of exactly ABR_FRAME_SAMPLES samples
 *
 * The decoder reads the in-band mode id of each frame, so tier switches
 * need no notification and never cause a state reset.
 */
abr_status abr_decoder_decode(abr_decoder *dec,
                              const unsigned char *packet,
                              size_t packet_len,
                              spx_int16_t *frame);

#ifdef __cplusplus
}
#endif

#endif
