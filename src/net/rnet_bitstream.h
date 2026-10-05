// rnet_bitstream.h — MSB-first bit stream writer/reader. Header-only.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rnet {

class BitWriter {
public:
	void WriteBits(uint32_t value, int bits)
	{
		for(int i = bits - 1; i >= 0; i--){
			size_t byteIdx = m_bitPos / 8;
			int bitIdx = 7 - (int)(m_bitPos % 8);
			if(byteIdx >= m_data.size())
				m_data.push_back(0);
			if((value >> i) & 1)
				m_data[byteIdx] |= (uint8_t)(1u << bitIdx);
			m_bitPos++;
		}
	}
	void WriteU8 (uint8_t v)  { WriteBits(v, 8); }
	void WriteU16(uint16_t v) { WriteBits(v, 16); }
	void WriteU32(uint32_t v) { WriteBits(v, 32); }
	void WriteI8 (int8_t v)   { WriteBits((uint8_t)v, 8); }
	void WriteI16(int16_t v)  { WriteBits((uint16_t)v, 16); }
	void WriteI32(int32_t v)  { WriteBits((uint32_t)v, 32); }
	void WriteF32(float v)
	{
		uint32_t bits;
		std::memcpy(&bits, &v, 4);
		WriteU32(bits);
	}
	void WriteBool(bool v) { WriteBits(v ? 1 : 0, 1); }
	// u16 length-prefixed string
	void WriteString(const char* s, uint16_t maxLen)
	{
		uint16_t len = (uint16_t)strlen(s);
		if(len > maxLen) len = maxLen;
		WriteU16(len);
		for(uint16_t i = 0; i < len; i++)
			WriteU8((uint8_t)s[i]);
	}
	void WriteString(const std::string& s, uint16_t maxLen) { WriteString(s.c_str(), maxLen); }

	const uint8_t* Data() const { return m_data.data(); }
	size_t ByteSize() const { return (m_bitPos + 7) / 8; }
	size_t BitSize() const { return m_bitPos; }
	void Reset() { m_data.clear(); m_bitPos = 0; }

private:
	std::vector<uint8_t> m_data;
	size_t m_bitPos = 0;
};

class BitReader {
public:
	BitReader(const uint8_t* data, size_t bytes) : m_data(data), m_bits(bytes * 8) {}

	uint32_t ReadBits(int bits)
	{
		uint32_t value = 0;
		for(int i = 0; i < bits; i++){
			if(m_bitPos >= m_bits){ m_ok = false; return value; }
			size_t byteIdx = m_bitPos / 8;
			int bitIdx = 7 - (int)(m_bitPos % 8);
			value = (value << 1) | ((m_data[byteIdx] >> bitIdx) & 1);
			m_bitPos++;
		}
		return value;
	}
	uint8_t  ReadU8 () { return (uint8_t)ReadBits(8); }
	uint16_t ReadU16() { return (uint16_t)ReadBits(16); }
	uint32_t ReadU32() { return ReadBits(32); }
	int8_t   ReadI8 () { return (int8_t)ReadBits(8); }
	int16_t  ReadI16() { return (int16_t)ReadBits(16); }
	int32_t  ReadI32() { return (int32_t)ReadBits(32); }
	float    ReadF32()
	{
		uint32_t bits = ReadU32();
		float v;
		std::memcpy(&v, &bits, 4);
		return v;
	}
	bool ReadBool() { return ReadBits(1) != 0; }
	std::string ReadString(uint16_t maxLen)
	{
		uint16_t len = ReadU16();
		if(len > maxLen){ m_ok = false; return std::string(); }
		std::string s;
		s.reserve(len);
		for(uint16_t i = 0; i < len; i++)
			s.push_back((char)ReadU8());
		return s;
	}

	bool Ok() const { return m_ok; }
	size_t BitsLeft() const { return m_bits - m_bitPos; }

private:
	const uint8_t* m_data;
	size_t m_bits;
	size_t m_bitPos = 0;
	bool m_ok = true;
};

} // namespace rnet
