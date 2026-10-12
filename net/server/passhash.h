// net/server/passhash.h — self-contained SHA-256 + PBKDF2-HMAC-SHA256 (RFC 8018).
// Passwords are stored as a per-account random salt + iterated derived key; the
// plaintext never touches the database. Header-only, no external crypto deps.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace passhash {

constexpr int    PBKDF2_ITERATIONS = 100000;  // ~50 ms on a server core
constexpr size_t SALT_LEN         = 16;       // random bytes
constexpr size_t KEY_LEN          = 32;       // SHA-256 output
constexpr int    MIN_PASSWORD_LEN = 4;
constexpr int    MAX_PASSWORD_LEN = 64;

// ------------------------------------------------------------------ SHA-256
namespace detail {

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void Sha256Block(uint32_t st[8], const uint8_t block[64])
{
	static const uint32_t K[64] = {
		0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
		0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
		0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
		0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
		0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
		0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
		0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
		0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
	};
	uint32_t w[64];
	for(int i = 0; i < 16; i++)
		w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
		       ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
	for(int i = 16; i < 64; i++){
		uint32_t s0 = Rotr(w[i-15], 7) ^ Rotr(w[i-15], 18) ^ (w[i-15] >> 3);
		uint32_t s1 = Rotr(w[i-2], 17) ^ Rotr(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
	uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
	for(int i = 0; i < 64; i++){
		uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + K[i] + w[i];
		uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
		uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + mj;
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d;
	st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

} // namespace detail

inline void Sha256(const uint8_t* data, size_t len, uint8_t out[32])
{
	uint32_t st[8] = {
		0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
		0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
	};
	size_t i = 0;
	for(; i + 64 <= len; i += 64)
		detail::Sha256Block(st, data + i);
	uint8_t tail[128] = {};
	size_t rem = len - i;
	std::memcpy(tail, data + i, rem);
	tail[rem] = 0x80;
	size_t total = (rem < 56) ? 64 : 128;
	uint64_t bits = (uint64_t)len * 8;
	for(int b = 0; b < 8; b++)
		tail[total - 1 - b] = (uint8_t)((bits >> (8 * b)) & 0xFF);
	for(size_t off = 0; off < total; off += 64)
		detail::Sha256Block(st, tail + off);
	for(int j = 0; j < 8; j++){
		out[j*4]   = (uint8_t)(st[j] >> 24);
		out[j*4+1] = (uint8_t)(st[j] >> 16);
		out[j*4+2] = (uint8_t)(st[j] >> 8);
		out[j*4+3] = (uint8_t)(st[j]);
	}
}

// ------------------------------------------------------------------- HMAC
inline void HmacSha256(const uint8_t* key, size_t keyLen,
                       const uint8_t* msg, size_t msgLen, uint8_t out[32])
{
	uint8_t k[64] = {};
	if(keyLen > 64){
		Sha256(key, keyLen, k);
	}else{
		std::memcpy(k, key, keyLen);
	}
	uint8_t pad[64], inner[32];
	for(int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;

	// inner = SHA256(pad ^ key || msg) — stream pad then msg through one digest
	uint8_t* ibuf = new uint8_t[64 + msgLen];
	std::memcpy(ibuf, pad, 64);
	std::memcpy(ibuf + 64, msg, msgLen);
	Sha256(ibuf, 64 + msgLen, inner);
	delete[] ibuf;

	for(int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
	uint8_t obuf[64 + 32];
	std::memcpy(obuf, pad, 64);
	std::memcpy(obuf + 64, inner, 32);
	Sha256(obuf, 64 + 32, out);
}

// ----------------------------------------------------------------- PBKDF2
inline void Pbkdf2HmacSha256(const char* password, size_t passLen,
                             const uint8_t* salt, size_t saltLen,
                             int iterations, uint8_t* out, size_t outLen)
{
	// single 32-byte block is all we need (KEY_LEN == hash length)
	uint8_t blk[4] = { 0, 0, 0, 1 };
	uint8_t* msg = new uint8_t[saltLen + 4];
	std::memcpy(msg, salt, saltLen);
	std::memcpy(msg + saltLen, blk, 4);

	uint8_t u[32], t[32];
	HmacSha256((const uint8_t*)password, passLen, msg, saltLen + 4, u);
	std::memcpy(t, u, 32);
	for(int it = 1; it < iterations; it++){
		HmacSha256((const uint8_t*)password, passLen, u, 32, u);
		for(int i = 0; i < 32; i++) t[i] ^= u[i];
	}
	delete[] msg;
	std::memcpy(out, t, outLen < 32 ? outLen : 32);
}

// Constant-time compare; returns true when equal.
inline bool SecureEqual(const uint8_t* a, const uint8_t* b, size_t n)
{
	uint8_t diff = 0;
	for(size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
	return diff == 0;
}

// Derive the 32-byte key for a password + salt.
inline void DeriveKey(const char* password, const uint8_t salt[SALT_LEN], uint8_t out[KEY_LEN])
{
	Pbkdf2HmacSha256(password, std::strlen(password), salt, SALT_LEN,
	                 PBKDF2_ITERATIONS, out, KEY_LEN);
}

} // namespace passhash
