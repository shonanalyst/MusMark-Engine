/**
 * Example: Detect who owns a watermarked audio file.
 *
 * The lookup function bridges the engine to your storage.
 * It receives a signatureId and must return the stored payload or null.
 */

import { detect } from "musmark-engine";
import path from "path";

// ---- Stub database (replace with your real DB) ----
const fakeDb = new Map<string, object>();
// fakeDb.set(signatureId, payload);  // populated at sign-time
// ---------------------------------------------------

async function main() {
  const result = await detect(
    path.resolve("suspect_file.wav"), // the file to analyse (must be float32 WAV)
    { secret: process.env.WATERMARK_SECRET! },
    async (signatureId) => {
      // Replace with your real DB query, e.g.:
      //   return db.prepare("SELECT * FROM signatures WHERE id = ?").get(signatureId);
      return fakeDb.get(signatureId) as any ?? null;
    }
  );

  if (result.detected) {
    console.log("Watermark detected!");
    console.log("Recipient:", result.payload?.recipient_identifier);
    console.log("Project:", result.payload?.project_id);
    console.log("Signed at:", result.payload?.timestamp);
    console.log("Confidence:", result.confidence + "%");
  } else {
    console.log("No watermark found (or file not in database).");
    console.log("Confidence:", result.confidence + "%");
    console.log("Stats:", result.stats);
  }
}

main().catch(console.error);
