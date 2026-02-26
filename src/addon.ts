import path from "path";

// Resolve the native addon relative to this file's location
const addonPath = path.resolve(__dirname, "..", "native", "build", "Release", "watermark.node");
// eslint-disable-next-line @typescript-eslint/no-var-requires
const addon = require(addonPath) as {
  embedWatermark: (
    inputPath: string,
    outputPath: string,
    bitstream: Buffer,
    options: {
      sampleRate: number;
      channels: number;
      blockSize: number;
      hopSize: number;
      secret: string;
      embedStrength: number;
      rotationSeconds: number;
      removeBitstream?: Buffer | null;
    }
  ) => void;
  extractWatermark: (
    inputPath: string,
    options: {
      sampleRate: number;
      channels: number;
      blockSize: number;
      hopSize: number;
      secret: string;
      embedStrength?: number;
    }
  ) => {
    bitstream: Buffer;
    correlations: Buffer;
    bitConfidence: number;
    bandAgreement: number;
    blocksAnalyzed: number;
  };
};

export interface EmbedOptions {
  secret: string;
  sampleRate?: number;
  channels?: number;
  blockSize?: number;
  hopSize?: number;
  embedStrength?: number;
  rotationSeconds?: number;
  removeBitstream?: Uint8Array | null;
}

export interface ExtractResult {
  bitstream: Uint8Array;
  correlations: Float32Array;
  bitConfidence: number;
  bandAgreement: number;
  blocksAnalyzed: number;
}

export function embedWatermark(
  inputPath: string,
  outputPath: string,
  bitstream: Uint8Array,
  options: EmbedOptions
): void {
  addon.embedWatermark(inputPath, outputPath, Buffer.from(bitstream), {
    sampleRate: options.sampleRate ?? 44100,
    channels: options.channels ?? 2,
    blockSize: options.blockSize ?? 4096,
    hopSize: options.hopSize ?? 1024,
    secret: options.secret,
    embedStrength: options.embedStrength ?? 0.0005,
    rotationSeconds: options.rotationSeconds ?? 5,
    removeBitstream: options.removeBitstream ? Buffer.from(options.removeBitstream) : null,
  });
}

export function extractWatermark(inputPath: string, options: EmbedOptions): ExtractResult {
  const result = addon.extractWatermark(inputPath, {
    sampleRate: options.sampleRate ?? 44100,
    channels: options.channels ?? 2,
    blockSize: options.blockSize ?? 4096,
    hopSize: options.hopSize ?? 1024,
    secret: options.secret,
    embedStrength: options.embedStrength ?? 0.0005,
  });

  return {
    bitstream: new Uint8Array(result.bitstream),
    correlations: new Float32Array(
      result.correlations.buffer,
      result.correlations.byteOffset,
      result.correlations.length / 4
    ),
    bitConfidence: result.bitConfidence,
    bandAgreement: result.bandAgreement,
    blocksAnalyzed: result.blocksAnalyzed,
  };
}
