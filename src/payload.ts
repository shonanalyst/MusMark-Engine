import crypto from "crypto";
import { v4 as uuidv4 } from "uuid";
import { rsEncode, rsDecode } from "./reedSolomon";
import { interleaveBits, deinterleaveBits, bitsToBytes, bytesToBits } from "./bitUtils";
import type { WatermarkPayload } from "./types";

const SYNC_PATTERN = new Uint8Array([
  1,0,1,0,1,1,0,1,  0,1,0,1,0,0,1,0,
  1,1,1,0,0,1,1,0,  0,1,1,0,0,0,1,1,
  1,0,0,1,1,0,1,0,  0,1,1,1,0,0,1,0,
  1,0,1,1,0,1,0,0,  1,1,0,0,1,0,1,1,
]);

const PARITY_BYTES = 32;
const INTERLEAVE_DEPTH = 8;

function uuidToBytes(uuid: string): Uint8Array {
  const hex = uuid.replace(/-/g, "");
  const bytes = new Uint8Array(16);
  for (let i = 0; i < 16; i++) bytes[i] = parseInt(hex.substr(i * 2, 2), 16);
  return bytes;
}

function bytesToUuid(bytes: Uint8Array): string {
  const hex = Array.from(bytes).map((b) => b.toString(16).padStart(2, "0")).join("");
  return `${hex.slice(0,8)}-${hex.slice(8,12)}-${hex.slice(12,16)}-${hex.slice(16,20)}-${hex.slice(20,32)}`;
}

function numberToBits(value: number, bitCount: number): Uint8Array {
  const bits = new Uint8Array(bitCount);
  for (let i = 0; i < bitCount; i++) bits[bitCount - 1 - i] = (value >> i) & 1;
  return bits;
}

function bitsToNumber(bits: Uint8Array): number {
  let value = 0;
  for (let i = 0; i < bits.length; i++) value = (value << 1) | (bits[i] & 1);
  return value;
}

function findSync(data: Uint8Array, pattern: Uint8Array): number {
  let bestMatch = 0;
  let bestIndex = -1;
  for (let i = 0; i <= data.length - pattern.length; i++) {
    let matches = 0;
    for (let j = 0; j < pattern.length; j++) {
      if (data[i + j] === pattern[j]) matches++;
    }
    if (matches > bestMatch) {
      bestMatch = matches;
      bestIndex = i;
    }
    if (matches / pattern.length >= 0.85) return i;
  }
  if (bestMatch / pattern.length >= 0.6) return bestIndex;
  return -1;
}

export function buildBitstream(signatureId: string): Uint8Array {
  const payloadBuffer = Buffer.from(uuidToBytes(signatureId));
  const encoded = rsEncode(new Uint8Array(payloadBuffer), PARITY_BYTES);
  const bits = bytesToBits(encoded);
  const interleaved = interleaveBits(bits, INTERLEAVE_DEPTH);
  const lengthBits = numberToBits(payloadBuffer.length, 16);
  const bitstream = new Uint8Array(SYNC_PATTERN.length + lengthBits.length + interleaved.length);
  bitstream.set(SYNC_PATTERN, 0);
  bitstream.set(lengthBits, SYNC_PATTERN.length);
  bitstream.set(interleaved, SYNC_PATTERN.length + lengthBits.length);
  return bitstream;
}

export interface EncodedPayload {
  payload: WatermarkPayload;
  payloadHash: string;
  bitstream: Uint8Array;
}

export function encodePayload(projectId: string, recipientIdentifier: string): EncodedPayload {
  const signatureId = uuidv4();
  const payload: WatermarkPayload = {
    signature_id: signatureId,
    project_id: projectId,
    recipient_identifier: recipientIdentifier,
    timestamp: new Date().toISOString(),
  };
  const payloadHash = crypto
    .createHash("sha256")
    .update(JSON.stringify(payload))
    .digest("hex");
  return { payload, payloadHash, bitstream: buildBitstream(signatureId) };
}

export interface DecodedPayload {
  signatureId: string | null;
  payloadHash: string | null;
  success: boolean;
  errorCount: number;
}

export function decodeBitstream(bitstream: Uint8Array): DecodedPayload {
  const syncIndex = findSync(bitstream, SYNC_PATTERN);
  if (syncIndex < 0) return { signatureId: null, payloadHash: null, success: false, errorCount: 0 };

  const start = syncIndex + SYNC_PATTERN.length;
  const lengthBits = bitstream.slice(start, start + 16);
  const payloadLength = bitsToNumber(lengthBits);

  const expectedCodewordBits = (payloadLength + PARITY_BYTES) * 8;
  const payloadBits = bitstream.slice(start + 16, start + 16 + expectedCodewordBits);
  const deinterleaved = deinterleaveBits(payloadBits, INTERLEAVE_DEPTH);
  const codeword = bitsToBytes(deinterleaved);

  const { data, corrected, errorCount } = rsDecode(codeword, PARITY_BYTES);
  if (!corrected) return { signatureId: null, payloadHash: null, success: false, errorCount };

  const payloadBytes = data.slice(0, payloadLength);
  if (payloadLength === 16) {
    const signatureId = bytesToUuid(payloadBytes);
    const payloadHash = crypto.createHash("sha256").update(Buffer.from(payloadBytes)).digest("hex");
    return { signatureId, payloadHash, success: true, errorCount };
  }

  return { signatureId: null, payloadHash: null, success: false, errorCount };
}

export function applySoftMajorityVoting(correlations: Float32Array): Uint8Array {
  const period = 64 + 16 + (16 + 32) * 8; // 464 bits
  const repetitions = Math.floor(correlations.length / period);

  if (repetitions < 2) {
    const result = new Uint8Array(correlations.length);
    for (let i = 0; i < correlations.length; i++) result[i] = correlations[i] > 0 ? 1 : 0;
    return result;
  }

  const positionSums = new Float32Array(period);
  for (let r = 0; r < repetitions; r++) {
    for (let i = 0; i < period; i++) {
      const idx = r * period + i;
      if (idx < correlations.length) positionSums[i] += correlations[idx];
    }
  }

  const result = new Uint8Array(period);
  for (let i = 0; i < period; i++) result[i] = positionSums[i] > 0 ? 1 : 0;
  return result;
}
