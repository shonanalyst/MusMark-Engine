# musmark-engine

Audio watermarking engine for Node.js.
Sign audio files with an invisible watermark, then detect who leaked them.

- Inaudible spread-spectrum watermark embedded directly in the audio signal
- Reed-Solomon error correction (survives re-encoding, compression, format conversion)
- Secret-key based — two engines with different keys cannot read each other's watermarks
- No external service required — runs fully on your server

---

## How it works

```
Sign:    audio + (projectId, recipientId, secret)  →  watermarked audio + signatureId
Detect:  suspect audio + (secret, your DB lookup)  →  recipientId who leaked it
```

The embedded payload is only 16 bytes (a UUID). Everything else — recipient name, project, timestamp — stays in **your** database. The engine just stores and retrieves the lookup key.

---

## Requirements

- Node.js 18+
- Python 3.x (for node-gyp)
- C++ build tools:
  - **Windows**: Visual Studio Build Tools 2019+ with "Desktop development with C++"
  - **Linux/macOS**: `gcc`/`clang` and `make`
- **ffmpeg** (for audio transcoding — see below)

---

## Installation

```bash
npm install musmark-engine
npm run build:native   # compiles the C++ addon once
```

Or if you copy the source directly into your project:

```bash
cd musmark-engine
npm install
npm run build:all
```

---

## Audio format requirement

The engine reads and writes **32-bit float WAV files only**.
Use ffmpeg to convert before signing and after signing:

```bash
# Convert input (any format) → float32 WAV for signing
ffmpeg -i input.mp3 -c:a pcm_f32le -ar 44100 -ac 2 input_float.wav

# After signing: convert the watermarked WAV → original format
ffmpeg -i signed_output.wav -c:a libmp3lame -q:a 2 signed_output.mp3
```

---

## Basic usage

```typescript
import { sign, detect } from "musmark-engine";

// --- SIGN ---
const result = await sign(
  "input_float.wav",   // float32 WAV input
  "output_float.wav",  // float32 WAV output (watermarked)
  "my-project",        // project identifier
  "user@example.com",  // recipient identifier
  { secret: "my-secret-key" }
);
// result.signatureId  → store this in your database
// result.payload      → full payload object to persist

// --- DETECT ---
const detection = await detect(
  "suspect_float.wav",
  { secret: "my-secret-key" },
  async (signatureId) => {
    // your DB lookup — return payload or null
    return db.getSignatureById(signatureId);
  }
);
if (detection.detected) {
  console.log("Leaked by:", detection.payload.recipient_identifier);
}
```

---

## Integrating with a database

The engine itself has no database. You provide a `lookupFn` during detection that queries your storage.

### SQLite (better-sqlite3)

```typescript
import Database from "better-sqlite3";
import { sign, detect } from "musmark-engine";

const db = new Database("signatures.db");

// Create table once
db.exec(`
  CREATE TABLE IF NOT EXISTS signatures (
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    recipient_identifier TEXT NOT NULL,
    payload_hash TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    created_at TEXT NOT NULL
  )
`);

// Sign a file and persist the signature
async function signAndStore(
  inputWav: string,
  outputWav: string,
  projectId: string,
  recipientId: string
) {
  const result = await sign(inputWav, outputWav, projectId, recipientId, {
    secret: process.env.WATERMARK_SECRET!,
  });

  db.prepare(`
    INSERT INTO signatures (id, project_id, recipient_identifier, payload_hash, payload_json, created_at)
    VALUES (?, ?, ?, ?, ?, ?)
  `).run(
    result.signatureId,
    result.payload.project_id,
    result.payload.recipient_identifier,
    result.payloadHash,
    JSON.stringify(result.payload),
    result.payload.timestamp
  );

  return result;
}

// Detect and look up from SQLite
async function detectFromDb(suspectWav: string) {
  return detect(
    suspectWav,
    { secret: process.env.WATERMARK_SECRET! },
    (signatureId) => {
      const row = db
        .prepare("SELECT payload_json FROM signatures WHERE id = ?")
        .get(signatureId) as { payload_json: string } | undefined;
      return row ? JSON.parse(row.payload_json) : null;
    }
  );
}
```

### PostgreSQL (pg / Prisma)

```typescript
import { Pool } from "pg";
import { sign, detect } from "musmark-engine";

const pool = new Pool({ connectionString: process.env.DATABASE_URL });

// Schema (run once via migration):
// CREATE TABLE signatures (
//   id UUID PRIMARY KEY,
//   project_id TEXT NOT NULL,
//   recipient_identifier TEXT NOT NULL,
//   payload_hash TEXT NOT NULL,
//   payload_json JSONB NOT NULL,
//   created_at TIMESTAMPTZ DEFAULT NOW()
// );

async function signAndStore(inputWav: string, outputWav: string, projectId: string, recipientId: string) {
  const result = await sign(inputWav, outputWav, projectId, recipientId, {
    secret: process.env.WATERMARK_SECRET!,
  });

  await pool.query(
    `INSERT INTO signatures (id, project_id, recipient_identifier, payload_hash, payload_json)
     VALUES ($1, $2, $3, $4, $5)`,
    [
      result.signatureId,
      result.payload.project_id,
      result.payload.recipient_identifier,
      result.payloadHash,
      result.payload,
    ]
  );

  return result;
}

async function detectFromDb(suspectWav: string) {
  return detect(
    suspectWav,
    { secret: process.env.WATERMARK_SECRET! },
    async (signatureId) => {
      const { rows } = await pool.query(
        "SELECT payload_json FROM signatures WHERE id = $1",
        [signatureId]
      );
      return rows[0]?.payload_json ?? null;
    }
  );
}
```

### MongoDB (mongoose)

```typescript
import mongoose from "mongoose";
import { sign, detect } from "musmark-engine";

const SignatureSchema = new mongoose.Schema({
  _id: String,          // signatureId (UUID)
  project_id: String,
  recipient_identifier: String,
  payload_hash: String,
  payload: Object,
  created_at: { type: Date, default: Date.now },
});
const Signature = mongoose.model("Signature", SignatureSchema);

async function signAndStore(inputWav: string, outputWav: string, projectId: string, recipientId: string) {
  const result = await sign(inputWav, outputWav, projectId, recipientId, {
    secret: process.env.WATERMARK_SECRET!,
  });

  await Signature.create({
    _id: result.signatureId,
    project_id: result.payload.project_id,
    recipient_identifier: result.payload.recipient_identifier,
    payload_hash: result.payloadHash,
    payload: result.payload,
  });

  return result;
}

async function detectFromDb(suspectWav: string) {
  return detect(
    suspectWav,
    { secret: process.env.WATERMARK_SECRET! },
    async (signatureId) => {
      const doc = await Signature.findById(signatureId).lean();
      return doc?.payload ?? null;
    }
  );
}
```

---

## Integrating with a web server (Express / Next.js)

### Express — sign endpoint

```typescript
import express from "express";
import multer from "multer";
import { execSync } from "child_process";
import { sign } from "musmark-engine";
import fs from "fs";
import path from "path";

const app = express();
const upload = multer({ dest: "/tmp/uploads/" });

app.post("/sign", upload.single("audio"), async (req, res) => {
  const { project_id, recipient_identifier } = req.body;
  if (!req.file || !project_id || !recipient_identifier) {
    return res.status(400).json({ error: "Missing fields" });
  }

  const floatWav = req.file.path + "_float.wav";
  const signedWav = req.file.path + "_signed.wav";
  const outputMp3 = req.file.path + "_signed.mp3";

  try {
    // Convert input → float32 WAV
    execSync(`ffmpeg -i "${req.file.path}" -c:a pcm_f32le -ar 44100 -ac 2 "${floatWav}" -y`);

    // Watermark
    const result = await sign(floatWav, signedWav, project_id, recipient_identifier, {
      secret: process.env.WATERMARK_SECRET!,
    });

    // Convert signed WAV → mp3 for download
    execSync(`ffmpeg -i "${signedWav}" -c:a libmp3lame -q:a 2 "${outputMp3}" -y`);

    // Persist result.signatureId + result.payload to your DB here

    res.setHeader("X-Signature-Id", result.signatureId);
    res.download(outputMp3, "signed.mp3", () => {
      fs.unlinkSync(req.file!.path);
      fs.unlinkSync(floatWav);
      fs.unlinkSync(signedWav);
      fs.unlinkSync(outputMp3);
    });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "Signing failed" });
  }
});
```

### Express — detect endpoint

```typescript
app.post("/detect", upload.single("audio"), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: "No file" });

  const floatWav = req.file.path + "_float.wav";

  try {
    execSync(`ffmpeg -i "${req.file.path}" -c:a pcm_f32le -ar 44100 -ac 2 "${floatWav}" -y`);

    const result = await detect(
      floatWav,
      { secret: process.env.WATERMARK_SECRET! },
      async (signatureId) => {
        // Replace with your real DB query
        const row = db.prepare("SELECT payload_json FROM signatures WHERE id = ?").get(signatureId);
        return row ? JSON.parse(row.payload_json) : null;
      }
    );

    res.json(result);
  } finally {
    fs.unlinkSync(req.file!.path);
    if (fs.existsSync(floatWav)) fs.unlinkSync(floatWav);
  }
});
```

### Next.js App Router — route handler

```typescript
// app/api/sign/route.ts
import { NextRequest, NextResponse } from "next/server";
import { sign } from "musmark-engine";
import { writeFile, unlink } from "fs/promises";
import { execSync } from "child_process";
import os from "os";
import path from "path";

export async function POST(req: NextRequest) {
  const formData = await req.formData();
  const file = formData.get("audio") as File;
  const projectId = formData.get("project_id") as string;
  const recipientId = formData.get("recipient_identifier") as string;

  if (!file || !projectId || !recipientId) {
    return NextResponse.json({ error: "Missing fields" }, { status: 400 });
  }

  const tmpDir = os.tmpdir();
  const inputPath = path.join(tmpDir, `${Date.now()}_input`);
  const floatWav = inputPath + "_float.wav";
  const signedWav = inputPath + "_signed.wav";
  const signedMp3 = inputPath + "_signed.mp3";

  try {
    await writeFile(inputPath, Buffer.from(await file.arrayBuffer()));
    execSync(`ffmpeg -i "${inputPath}" -c:a pcm_f32le -ar 44100 -ac 2 "${floatWav}" -y`);

    const result = await sign(floatWav, signedWav, projectId, recipientId, {
      secret: process.env.WATERMARK_SECRET!,
    });

    execSync(`ffmpeg -i "${signedWav}" -c:a libmp3lame -q:a 2 "${signedMp3}" -y`);

    // Persist result.signatureId + result.payload to your DB here

    const { readFile } = await import("fs/promises");
    const audioBuffer = await readFile(signedMp3);

    return new NextResponse(audioBuffer, {
      headers: {
        "Content-Type": "audio/mpeg",
        "Content-Disposition": `attachment; filename="signed.mp3"`,
        "X-Signature-Id": result.signatureId,
      },
    });
  } finally {
    for (const p of [inputPath, floatWav, signedWav, signedMp3]) {
      await unlink(p).catch(() => {});
    }
  }
}
```

---

## API reference

### `sign(inputWavPath, outputWavPath, projectId, recipientId, options): Promise<SignResult>`

| Param | Type | Description |
|---|---|---|
| `inputWavPath` | `string` | Path to float32 WAV input |
| `outputWavPath` | `string` | Path to write watermarked float32 WAV |
| `projectId` | `string` | Arbitrary project label (stored in DB only) |
| `recipientId` | `string` | Who receives this copy (stored in DB only) |
| `options.secret` | `string` | Secret key — must match at detect-time |
| `options.sampleRate` | `number` | Default: 44100 |
| `options.channels` | `number` | Default: 2 |
| `options.embedStrength` | `number` | Default: 0.0005 (inaudible) |

Returns `SignResult`:
```typescript
{
  outputPath: string;
  signatureId: string;   // store this in DB
  payloadHash: string;   // SHA-256 of payload JSON
  payload: WatermarkPayload;  // persist alongside signatureId
}
```

### `detect(inputWavPath, options, lookupFn): Promise<DetectResult>`

| Param | Type | Description |
|---|---|---|
| `inputWavPath` | `string` | Path to float32 WAV to analyse |
| `options.secret` | `string` | Same secret used when signing |
| `lookupFn` | `(id: string) => Promise<WatermarkPayload \| null>` | Your DB lookup |

Returns `DetectResult`:
```typescript
{
  detected: boolean;
  confidence: number;       // 0–100
  payload: WatermarkPayload | null;
  payloadHash: string | null;
  stats: {
    bitConfidence: number;
    bandAgreement: number;
    blocksAnalyzed: number;
    errorCount: number;
  };
}
```

---

## Security notes

- Keep `WATERMARK_SECRET` in an environment variable. Never commit it.
- The watermark survives MP3/AAC re-encoding, pitch shifting, and moderate EQ. It does not survive hard clipping or silence replacement.
- Minimum recommended audio length: ~30 seconds for reliable detection.

---

## Building from source

```bash
git clone https://github.com/shonanalyst/musmark-engine
cd musmark-engine
npm install
npm run build:all
```

The native addon is compiled per-platform. Prebuilt binaries are not distributed.

---

## License

MIT
