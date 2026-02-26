/**
 * musmark-engine — public API
 *
 * sign()   — embed a watermark into a 32-bit float WAV file
 * detect() — extract and identify a watermark from a WAV file
 *
 * Both functions work only on 32-bit float WAV files.
 * Use ffmpeg to transcode before calling (see README for examples).
 */

import crypto from "crypto";
import { embedWatermark, extractWatermark } from "./addon";
import { encodePayload, decodeBitstream, applySoftMajorityVoting, buildBitstream } from "./payload";
import type { SignResult, DetectResult, WatermarkOptions, SignatureLookupFn, WatermarkPayload } from "./types";

export type { SignResult, DetectResult, WatermarkOptions, SignatureLookupFn, WatermarkPayload };

/**
 * Embed a watermark into a 32-bit float WAV file.
 *
 * @param inputWavPath  Path to the input float32 WAV file
 * @param outputWavPath Path to write the watermarked float32 WAV file
 * @param projectId     Arbitrary project identifier (stored in your DB, not embedded)
 * @param recipientId   Who received this copy (stored in your DB, not embedded)
 * @param options       Watermark options (secret key is required)
 * @returns             SignResult containing signatureId and payload — persist these in your DB
 */
export async function sign(
  inputWavPath: string,
  outputWavPath: string,
  projectId: string,
  recipientId: string,
  options: WatermarkOptions
): Promise<SignResult> {
  const { payload, payloadHash, bitstream } = encodePayload(projectId, recipientId);

  embedWatermark(inputWavPath, outputWavPath, bitstream, {
    secret: options.secret,
    sampleRate: options.sampleRate,
    channels: options.channels,
    embedStrength: options.embedStrength,
    blockSize: options.blockSize,
    hopSize: options.hopSize,
  });

  return {
    outputPath: outputWavPath,
    signatureId: payload.signature_id,
    payloadHash,
    payload,
  };
}

/**
 * Extract and identify a watermark from a 32-bit float WAV file.
 *
 * @param inputWavPath  Path to the float32 WAV file to analyse
 * @param options       Must use the same secret key used during signing
 * @param lookupFn      Async or sync function that receives a signatureId and returns
 *                      the stored payload from your database, or null if not found
 * @returns             DetectResult — check `detected` and `payload` fields
 */
export async function detect(
  inputWavPath: string,
  options: WatermarkOptions,
  lookupFn: SignatureLookupFn
): Promise<DetectResult> {
  const extracted = extractWatermark(inputWavPath, {
    secret: options.secret,
    sampleRate: options.sampleRate,
    channels: options.channels,
    embedStrength: options.embedStrength,
    blockSize: options.blockSize,
    hopSize: options.hopSize,
  });

  const votedBitstream = applySoftMajorityVoting(extracted.correlations);
  const decoded = decodeBitstream(votedBitstream);

  const stats = {
    bitConfidence: extracted.bitConfidence,
    bandAgreement: extracted.bandAgreement,
    blocksAnalyzed: extracted.blocksAnalyzed,
    errorCount: decoded.errorCount,
  };

  if (!decoded.success || !decoded.signatureId) {
    return { detected: false, confidence: 0, payload: null, payloadHash: decoded.payloadHash, stats };
  }

  const storedPayload = await lookupFn(decoded.signatureId);

  const bitRecoveryRatio = Math.max(0, 1 - decoded.errorCount / 32);
  const confidence = Math.round(
    100 *
      (0.35 * extracted.bitConfidence +
        0.2 * extracted.bandAgreement +
        0.2 * bitRecoveryRatio +
        0.15 * (decoded.success ? 1 : 0) +
        0.1 * (storedPayload ? 1 : 0))
  );

  return {
    detected: !!storedPayload,
    confidence,
    payload: storedPayload ?? null,
    payloadHash: decoded.payloadHash,
    stats,
  };
}

/**
 * Compute the SHA-256 of a file (useful for audit logging).
 */
export function sha256File(filePath: string): string {
  const fs = require("fs") as typeof import("fs");
  const data = fs.readFileSync(filePath);
  return crypto.createHash("sha256").update(data).digest("hex");
}
