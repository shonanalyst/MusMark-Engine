export function bytesToBits(bytes: Uint8Array): Uint8Array {
  const bits = new Uint8Array(bytes.length * 8);
  for (let i = 0; i < bytes.length; i++) {
    for (let b = 0; b < 8; b++) {
      bits[i * 8 + b] = (bytes[i] >> (7 - b)) & 1;
    }
  }
  return bits;
}

export function bitsToBytes(bits: Uint8Array): Uint8Array {
  const byteCount = Math.ceil(bits.length / 8);
  const bytes = new Uint8Array(byteCount);
  for (let i = 0; i < byteCount; i++) {
    let value = 0;
    for (let b = 0; b < 8; b++) {
      const idx = i * 8 + b;
      value = (value << 1) | (idx < bits.length ? bits[idx] & 1 : 0);
    }
    bytes[i] = value;
  }
  return bytes;
}

export function interleaveBits(bits: Uint8Array, depth: number): Uint8Array {
  if (depth <= 1) return bits;
  const rows = depth;
  const cols = Math.ceil(bits.length / rows);
  const padded = new Uint8Array(rows * cols);
  padded.set(bits, 0);

  const interleaved = new Uint8Array(rows * cols);
  let idx = 0;
  for (let c = 0; c < cols; c++) {
    for (let r = 0; r < rows; r++) {
      interleaved[idx++] = padded[r * cols + c];
    }
  }
  return interleaved.slice(0, bits.length);
}

export function deinterleaveBits(bits: Uint8Array, depth: number): Uint8Array {
  if (depth <= 1) return bits;
  const rows = depth;
  const cols = Math.ceil(bits.length / rows);
  const padded = new Uint8Array(rows * cols);
  padded.set(bits, 0);

  const deinterleaved = new Uint8Array(rows * cols);
  let idx = 0;
  for (let c = 0; c < cols; c++) {
    for (let r = 0; r < rows; r++) {
      deinterleaved[r * cols + c] = padded[idx++];
    }
  }
  return deinterleaved.slice(0, bits.length);
}
