export interface WatermarkPayload {
  signature_id: string;
  project_id: string;
  recipient_identifier: string;
  timestamp: string;
}

export interface SignResult {
  /** The watermarked audio file path (WAV, float32) */
  outputPath: string;
  /** Unique ID for this signature — store this in your database */
  signatureId: string;
  /** SHA-256 of the full payload JSON — use for integrity verification */
  payloadHash: string;
  /** Full payload object — persist this alongside signatureId */
  payload: WatermarkPayload;
}

export interface DetectResult {
  /** True when the watermark was decoded and a matching signature was found in the lookup fn */
  detected: boolean;
  /** 0–100 confidence score */
  confidence: number;
  /** The decoded payload if detected */
  payload: WatermarkPayload | null;
  /** SHA-256 of the embedded payload bytes */
  payloadHash: string | null;
  /** Raw extraction stats for debugging */
  stats: {
    bitConfidence: number;
    bandAgreement: number;
    blocksAnalyzed: number;
    errorCount: number;
  };
}

export interface WatermarkOptions {
  /** Secret key — keep consistent between signing and detection */
  secret: string;
  sampleRate?: number;
  channels?: number;
  embedStrength?: number;
  blockSize?: number;
  hopSize?: number;
}

/**
 * Called during detection to look up a signature_id from your storage.
 * Return the payload JSON if found, or null/undefined if not found.
 */
export type SignatureLookupFn = (
  signatureId: string
) => Promise<WatermarkPayload | null | undefined> | WatermarkPayload | null | undefined;
