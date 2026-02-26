/**
 * Example: Sign (watermark) a song.
 *
 * Prerequisites:
 *   npm install musmark-engine
 *   npm run build:native  (compiles the C++ addon)
 *
 * The input must be a 32-bit float WAV file.
 * Use ffmpeg to convert:
 *   ffmpeg -i input.mp3 -c:a pcm_f32le -ar 44100 -ac 2 input_float.wav
 */

import { sign } from "musmark-engine";
import path from "path";

async function main() {
  const result = await sign(
    path.resolve("input_float.wav"),   // input: float32 WAV
    path.resolve("signed_output.wav"), // output: watermarked float32 WAV
    "project-abc",                     // project ID (your identifier)
    "user@example.com",                // recipient identifier
    { secret: process.env.WATERMARK_SECRET! }
  );

  console.log("Signed successfully.");
  console.log("Signature ID:", result.signatureId); // store this in your DB
  console.log("Payload Hash:", result.payloadHash);
  console.log("Full payload:", result.payload);

  // Store result.signatureId + result.payload in your database
  // so detect() can look them up later.
}

main().catch(console.error);
