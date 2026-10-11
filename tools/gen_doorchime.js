// tools/gen_doorchime.js — synthesize the vehicle door chime WAV.
// Original two-note-plus-tail bell chime in the style of luxury-car door
// warnings (NOT a copy of any existing recording or melody).
// Usage: node gen_doorchime.js [outfile.wav]
const fs = require('fs');

const SR = 22050;
const DUR = 2.1;
const n = Math.floor(SR * DUR);
const samples = new Float64Array(n);

// classic descending door-bell figure (original choice of voicing)
const notes = [
  { f: 880.00, at: 0.00, dur: 1.30, amp: 0.55 },  // A5  "ding"
  { f: 587.33, at: 0.26, dur: 1.45, amp: 0.50 },  // D5  "dong"
  { f: 440.00, at: 0.52, dur: 1.55, amp: 0.32 },  // A4  soft tail
];

for (const nt of notes) {
  const start = Math.floor(nt.at * SR);
  const len = Math.floor(nt.dur * SR);
  for (let i = 0; i < len && start + i < n; i++) {
    const t = i / SR;
    const env = Math.exp(-3.2 * t) * (1 - Math.exp(-260 * t)); // bell attack + decay
    // additive bell timbre: fundamental + inharmonic partials
    const s =
      Math.sin(2 * Math.PI * nt.f * t) * 1.00 +
      Math.sin(2 * Math.PI * nt.f * 2.76 * t) * 0.22 +
      Math.sin(2 * Math.PI * nt.f * 5.40 * t) * 0.08;
    samples[start + i] += nt.amp * env * s;
  }
}

// normalize with headroom
let peak = 0;
for (let i = 0; i < n; i++) peak = Math.max(peak, Math.abs(samples[i]));
const norm = 0.8 * 32767 / peak;

const data = Buffer.alloc(n * 2);
for (let i = 0; i < n; i++) data.writeInt16LE(Math.round(samples[i] * norm), i * 2);

const hdr = Buffer.alloc(44);
hdr.write('RIFF', 0);
hdr.writeUInt32LE(36 + data.length, 4);
hdr.write('WAVE', 8);
hdr.write('fmt ', 12);
hdr.writeUInt32LE(16, 16);
hdr.writeUInt16LE(1, 20);            // PCM
hdr.writeUInt16LE(1, 22);            // mono
hdr.writeUInt32LE(SR, 24);
hdr.writeUInt32LE(SR * 2, 28);
hdr.writeUInt16LE(2, 32);
hdr.writeUInt16LE(16, 34);
hdr.write('data', 36);
hdr.writeUInt32LE(data.length, 40);

const out = process.argv[2] || 'doorchime.wav';
fs.writeFileSync(out, Buffer.concat([hdr, data]));
console.log('wrote ' + out + ' (' + (44 + data.length) + ' bytes, ' + DUR + 's)');
