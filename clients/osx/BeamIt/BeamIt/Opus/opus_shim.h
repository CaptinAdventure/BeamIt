#include <opus/opus.h>

static inline int beamit_opus_set_complexity(OpusEncoder *encoder, int value) {
    return opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(value));
}

static inline int beamit_opus_set_fec(OpusEncoder *encoder, int enabled) {
    return opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(enabled));
}

static inline int beamit_opus_set_dtx(OpusEncoder *encoder, int enabled) {
    return opus_encoder_ctl(encoder, OPUS_SET_DTX(enabled));
}

static inline int beamit_opus_reset_decoder(OpusDecoder *decoder) {
    return opus_decoder_ctl(decoder, OPUS_RESET_STATE);
}
