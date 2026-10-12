// rnet_voice.h — Opus codec for proximity voice (vendor/opus).
// 8 kHz mono, 20 ms frames, VOIP tuned, VBR at VOICE_BITRATE. Packets are
// variable length (<= VOICE_MAX_PACKET); each speaker gets its own decoder
// because Opus decoding is stateful.
#pragma once

#include "rnet_common.h"

#include <opus.h>

#include <cstdint>

namespace rnet {

class VoiceEncoder {
public:
	VoiceEncoder()
	{
		int err = OPUS_OK;
		m_enc = opus_encoder_create(VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
		if(err != OPUS_OK || !m_enc){ m_enc = nullptr; return; }
		opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(VOICE_BITRATE));
		opus_encoder_ctl(m_enc, OPUS_SET_VBR(1));
		opus_encoder_ctl(m_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	}
	~VoiceEncoder() { if(m_enc) opus_encoder_destroy(m_enc); }
	VoiceEncoder(const VoiceEncoder&) = delete;
	VoiceEncoder& operator=(const VoiceEncoder&) = delete;

	bool Ok() const { return m_enc != nullptr; }

	// one frame of VOICE_FRAME_SAMPLES -> packet in `out` (VOICE_MAX_PACKET
	// bytes). Returns the packet length, or <= 0 on error.
	int Encode(const int16_t* pcm, uint8_t* out)
	{
		if(!m_enc) return -1;
		return opus_encode(m_enc, pcm, VOICE_FRAME_SAMPLES, out, VOICE_MAX_PACKET);
	}

private:
	OpusEncoder* m_enc = nullptr;
};

class VoiceDecoder {
public:
	VoiceDecoder()
	{
		int err = OPUS_OK;
		m_dec = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &err);
		if(err != OPUS_OK || !m_dec) m_dec = nullptr;
	}
	~VoiceDecoder() { if(m_dec) opus_decoder_destroy(m_dec); }
	VoiceDecoder(const VoiceDecoder&) = delete;
	VoiceDecoder& operator=(const VoiceDecoder&) = delete;

	bool Ok() const { return m_dec != nullptr; }

	// forget the previous speaker's state (a source is being reused)
	void Reset() { if(m_dec) opus_decoder_ctl(m_dec, OPUS_RESET_STATE); }

	// packet -> up to VOICE_FRAME_SAMPLES samples. Returns the sample count,
	// or <= 0 for a corrupt packet (the caller drops it).
	int Decode(const uint8_t* packet, int len, int16_t* pcm)
	{
		if(!m_dec) return -1;
		return opus_decode(m_dec, packet, len, pcm, VOICE_FRAME_SAMPLES, 0);
	}

private:
	OpusDecoder* m_dec = nullptr;
};

} // namespace rnet
