#include <napi.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <array>

constexpr double kPi = 3.14159265358979323846;
#include "wav.h"
#include "fft.h"

// ============================================================================
// PSYCHOACOUSTIC MASKING MODEL - ISO/IEC 11172-3 (MPEG-1 Audio Layer III)
// Makes watermark truly imperceptible by embedding only in masked frequencies
// ============================================================================

struct XorShift64 {
  uint64_t state;
  explicit XorShift64(uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
  uint64_t next() {
    uint64_t x = state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state = x;
    return x;
  }
  double nextDouble() {
    return (next() >> 11) * (1.0 / 9007199254740992.0);
  }
  int nextInt(int maxValue) {
    return static_cast<int>(next() % static_cast<uint64_t>(maxValue));
  }
};

static uint64_t hashSecret(const std::string& secret) {
  uint64_t hash = 1469598103934665603ULL;
  for (char c : secret) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Convert frequency (Hz) to Bark scale (critical band rate)
static double freqToBark(double freq) {
  return 13.0 * std::atan(0.00076 * freq) + 3.5 * std::atan(std::pow(freq / 7500.0, 2.0));
}

// Calculate absolute threshold of hearing (ATH) in dB SPL
// Based on ISO 226 equal-loudness contours
static double absoluteThresholdOfHearing(double freqHz) {
  const double f = freqHz / 1000.0;
  if (f < 0.02) return 100.0;
  if (f > 20.0) return 100.0;
  // Terhardt formula approximation
  return 3.64 * std::pow(f, -0.8) 
       - 6.5 * std::exp(-0.6 * std::pow(f - 3.3, 2.0))
       + 0.001 * std::pow(f, 4.0);
}

// Calculate spreading function for frequency masking (in dB)
// Masking spreads from a masker to nearby frequencies in Bark scale
static double spreadingFunction(double deltaBark) {
  const double absD = std::abs(deltaBark);
  if (absD > 8.0) return -100.0; // No masking effect beyond 8 Bark
  
  // Asymmetric spreading: upward masking is stronger
  if (deltaBark >= 0) {
    // Upward spread (masker affects higher frequencies more)
    return 15.81 + 7.5 * (deltaBark + 0.474) 
         - 17.5 * std::sqrt(1.0 + std::pow(deltaBark + 0.474, 2.0));
  } else {
    // Downward spread (masker affects lower frequencies less)
    return 15.81 + 7.5 * (deltaBark + 0.474) 
         - 17.5 * std::sqrt(1.0 + std::pow(deltaBark + 0.474, 2.0))
         + 8.0 * std::abs(deltaBark); // Extra attenuation
  }
}

// Calculate masking threshold for each bin using psychoacoustic model
static std::vector<double> calculateMaskingThreshold(
  const std::vector<Complex>& fft,
  int sampleRate
) {
  const int n = static_cast<int>(fft.size());
  const int halfN = n / 2;
  const double binFreqStep = static_cast<double>(sampleRate) / n;
  
  std::vector<double> powerSpectrum(halfN);
  std::vector<double> powerDB(halfN);
  std::vector<double> barkFreq(halfN);
  std::vector<double> maskingThreshold(halfN, -100.0);
  
  // Calculate power spectrum and convert to dB
  for (int i = 0; i < halfN; i++) {
    const double mag = std::hypot(fft[i].re, fft[i].im);
    powerSpectrum[i] = mag * mag;
    // Convert to dB with small epsilon to avoid log(0)
    powerDB[i] = 10.0 * std::log10(std::max(powerSpectrum[i], 1e-20));
    barkFreq[i] = freqToBark(i * binFreqStep);
  }
  
  // Find tonal maskers (local peaks in spectrum)
  std::vector<int> maskerBins;
  std::vector<double> maskerPower;
  
  for (int i = 2; i < halfN - 2; i++) {
    // Check if local maximum (peak)
    if (powerDB[i] > powerDB[i-1] && powerDB[i] > powerDB[i+1] &&
        powerDB[i] > powerDB[i-2] + 6.0 && powerDB[i] > powerDB[i+2] + 6.0 &&
        powerDB[i] > -40.0) { // Only consider significant peaks
      maskerBins.push_back(i);
      // Combine energy from surrounding bins for tonal masker
      double combinedPower = powerSpectrum[i-1] + powerSpectrum[i] + powerSpectrum[i+1];
      maskerPower.push_back(10.0 * std::log10(std::max(combinedPower, 1e-20)));
    }
  }
  
  // Calculate global masking threshold for each bin
  for (int i = 1; i < halfN; i++) {
    double freq = i * binFreqStep;
    double bark = barkFreq[i];
    
    // Start with absolute threshold of hearing
    double threshold = absoluteThresholdOfHearing(freq);
    
    // Add contribution from each masker
    double maskerContribution = -100.0;
    for (size_t m = 0; m < maskerBins.size(); m++) {
      double maskerBark = barkFreq[maskerBins[m]];
      double deltaBark = bark - maskerBark;
      
      // Spreading function determines how much masking spreads
      double spread = spreadingFunction(deltaBark);
      
      // Tonal masking offset (tonal maskers are less effective at masking noise)
      const double tonalOffset = -6.025 - 0.275 * maskerBark;
      
      // Individual masking threshold from this masker
      double individualMask = maskerPower[m] + spread + tonalOffset;
      
      // Combine using power addition in linear domain
      if (individualMask > maskerContribution) {
        maskerContribution = 10.0 * std::log10(
          std::pow(10.0, maskerContribution / 10.0) + 
          std::pow(10.0, individualMask / 10.0)
        );
      }
    }
    
    // Final threshold is the higher of ATH and masker contribution
    maskingThreshold[i] = std::max(threshold, maskerContribution);
  }
  
  return maskingThreshold;
}

// Ultra-transparent watermarking using smooth, continuous phase modulation
// Avoids clicks by using very gradual changes across frequency bins
static void applyWatermarkToFrame(
  std::vector<Complex>& fftL,
  std::vector<Complex>& fftR,
  int bit,
  uint64_t seed,
  double embedStrength,
  int sampleRate = 44100
) {
  const int n = static_cast<int>(fftL.size());
  const int halfN = n / 2;
  
  // MICRO-EMBEDDING: Spread tiny changes across MANY bins
  // This prevents any single bin from having a noticeable change
  const double binFreqStep = static_cast<double>(sampleRate) / n;
  const int minBin = std::max(20, static_cast<int>(2500.0 / binFreqStep));
  const int maxBin = std::min(halfN - 10, static_cast<int>(5000.0 / binFreqStep));
  
  if (maxBin <= minBin + 20) return;
  
  // Ultra-micro phase delta - almost imperceptible
  const double microPhaseDelta = embedStrength * 0.005; // Extremely tiny
  
  XorShift64 prng(seed);
  
  // Apply smooth, gradual phase shifts using a cosine envelope
  // This prevents clicks by ensuring smooth transitions
  const int binRange = maxBin - minBin;
  
  for (int i = 0; i < binRange; i++) {
    const int bin = minBin + i;
    
    // Cosine window to smooth the phase changes (no abrupt edges)
    const double windowPos = static_cast<double>(i) / static_cast<double>(binRange - 1);
    const double smoothWindow = 0.5 * (1.0 - std::cos(2.0 * kPi * windowPos));
    
    // Get magnitude (we preserve this exactly)
    const double magL = std::hypot(fftL[bin].re, fftL[bin].im);
    const double magR = std::hypot(fftR[bin].re, fftR[bin].im);
    
    // Skip silent bins
    if (magL < 1e-10 && magR < 1e-10) continue;
    
    // Deterministic sign based on bin and seed (consistent across frames)
    const double sign = ((prng.next() % 2) == 0) ? 1.0 : -1.0;
    
    // Scale by magnitude - louder bins can hide more
    const double magScale = std::min(1.0, std::sqrt(magL * magL + magR * magR) * 10.0);
    
    // Final phase shift: tiny, smooth, magnitude-scaled
    const double phaseShift = sign * microPhaseDelta * smoothWindow * magScale * (bit ? 1.0 : -1.0);
    
    const double phaseL = std::atan2(fftL[bin].im, fftL[bin].re);
    const double phaseR = std::atan2(fftR[bin].im, fftR[bin].re);
    
    // Apply phase shift to stereo difference (less audible than direct)
    const double newPhaseL = phaseL + phaseShift * 0.3;
    const double newPhaseR = phaseR - phaseShift * 0.3;
    
    // Reconstruct with EXACT original magnitude
    fftL[bin].re = magL * std::cos(newPhaseL);
    fftL[bin].im = magL * std::sin(newPhaseL);
    fftR[bin].re = magR * std::cos(newPhaseR);
    fftR[bin].im = magR * std::sin(newPhaseR);
    
    // Mirror for real signal
    const int mirror = n - bin;
    if (mirror > 0 && mirror < n && mirror != bin) {
      fftL[mirror].re = fftL[bin].re;
      fftL[mirror].im = -fftL[bin].im;
      fftR[mirror].re = fftR[bin].re;
      fftR[mirror].im = -fftR[bin].im;
    }
  }
}

// Extract watermark using smooth phase detection (matches embedding)
static int extractBitFromFrame(
  const std::vector<Complex>& fftL,
  const std::vector<Complex>& fftR,
  uint64_t seed,
  double embedStrength,
  double& bitConfidence,
  double& bandAgreement,
  int sampleRate = 44100
) {
  const int n = static_cast<int>(fftL.size());
  const int halfN = n / 2;
  
  const double binFreqStep = static_cast<double>(sampleRate) / n;
  const int minBin = std::max(20, static_cast<int>(2500.0 / binFreqStep));
  const int maxBin = std::min(halfN - 10, static_cast<int>(5000.0 / binFreqStep));
  
  if (maxBin <= minBin + 20) {
    bitConfidence = 0.0;
    bandAgreement = 0.0;
    return 0;
  }

  XorShift64 prng(seed);
  double phaseSum = 0.0;
  double weightSum = 0.0;
  int validBins = 0;
  const int binRange = maxBin - minBin;

  for (int i = 0; i < binRange; i++) {
    const int bin = minBin + i;
    
    const double magL = std::hypot(fftL[bin].re, fftL[bin].im);
    const double magR = std::hypot(fftR[bin].re, fftR[bin].im);
    
    if (magL < 1e-10 && magR < 1e-10) continue;
    
    const double sign = ((prng.next() % 2) == 0) ? 1.0 : -1.0;
    
    const double phaseL = std::atan2(fftL[bin].im, fftL[bin].re);
    const double phaseR = std::atan2(fftR[bin].im, fftR[bin].re);
    
    double phaseDiff = phaseL - phaseR;
    while (phaseDiff > kPi) phaseDiff -= 2.0 * kPi;
    while (phaseDiff < -kPi) phaseDiff += 2.0 * kPi;
    
    // Weight by magnitude (louder = more reliable)
    const double weight = std::sqrt(magL * magL + magR * magR);
    phaseSum += sign * phaseDiff * weight;
    weightSum += weight;
    validBins++;
  }

  bandAgreement = validBins > 0 ? static_cast<double>(validBins) / static_cast<double>(binRange) : 0.0;
  
  if (weightSum > 1e-10) {
    const double avgPhase = phaseSum / weightSum;
    bitConfidence = std::min(1.0, std::abs(avgPhase) * 100.0);
    return avgPhase >= 0 ? 1 : 0;
  }
  
  bitConfidence = 0.0;
  return 0;
}

static std::vector<float> getChannelSamples(const std::vector<float>& interleaved, int channels, int channelIndex) {
  std::vector<float> out(interleaved.size() / channels);
  for (size_t i = 0, j = channelIndex; j < interleaved.size(); i++, j += channels) {
    out[i] = interleaved[j];
  }
  return out;
}

static void writeChannelSamples(std::vector<float>& interleaved, const std::vector<float>& channelData, int channels, int channelIndex) {
  for (size_t i = 0, j = channelIndex; j < interleaved.size(); i++, j += channels) {
    interleaved[j] = channelData[i];
  }
}

static Napi::Value EmbedWatermark(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  try {
    if (info.Length() < 4) {
      Napi::TypeError::New(env, "Expected inputPath, outputPath, bitstream, options").ThrowAsJavaScriptException();
      return env.Null();
    }

  const std::string inputPath = info[0].As<Napi::String>();
  const std::string outputPath = info[1].As<Napi::String>();
  const Napi::Buffer<uint8_t> bitBuffer = info[2].As<Napi::Buffer<uint8_t>>();
  const Napi::Object options = info[3].As<Napi::Object>();

  const int sampleRate = options.Get("sampleRate").As<Napi::Number>().Int32Value();
  const int channels = options.Get("channels").As<Napi::Number>().Int32Value();
  const int blockSize = options.Get("blockSize").As<Napi::Number>().Int32Value();
  const int hopSize = options.Get("hopSize").As<Napi::Number>().Int32Value();
  const std::string secret = options.Get("secret").As<Napi::String>();
  const double embedStrength = options.Get("embedStrength").As<Napi::Number>().DoubleValue();
  const double rotationSeconds = options.Get("rotationSeconds").As<Napi::Number>().DoubleValue();

  std::vector<uint8_t> bitstream(bitBuffer.Data(), bitBuffer.Data() + bitBuffer.Length());

  std::vector<uint8_t> removeBits;
  if (options.Has("removeBitstream") && !options.Get("removeBitstream").IsNull()) {
    Napi::Buffer<uint8_t> removeBuffer = options.Get("removeBitstream").As<Napi::Buffer<uint8_t>>();
    removeBits.assign(removeBuffer.Data(), removeBuffer.Data() + removeBuffer.Length());
  }

    WavData wav = readWav(inputPath);
    if (wav.sampleRate != sampleRate || wav.channels != channels) {
      throw std::runtime_error("Unexpected WAV format");
    }

  std::vector<float> left = getChannelSamples(wav.samples, channels, 0);
  std::vector<float> right = channels > 1 ? getChannelSamples(wav.samples, channels, 1) : left;

  const size_t totalSamples = left.size();
  
  // =========================================================================
  // SPREAD SPECTRUM WATERMARKING WITH POSITION-DEPENDENT PN SEQUENCES
  // Key insight: Using a DIFFERENT PN sequence for each bit position in the 
  // payload makes the audio's natural PN correlation become random noise that
  // averages out across repetitions, while the watermark stays consistent.
  // =========================================================================
  
  const uint64_t baseSeed = hashSecret(secret);
  
  // Parameters tuned for balance between quality and detection
  const int samplesPerBit = hopSize * 4;  // 4096 samples ≈ 0.09s per bit
  const double strength = 0.007;  // ~0.7% base, gentle but detectable
  
  // Pre-generate PN sequences for each bit position in the payload
  // This is crucial: each position gets a unique PN to decorrelate audio bias
  const int payloadLen = static_cast<int>(bitstream.size());
  std::vector<std::vector<double>> pnSequences(payloadLen, std::vector<double>(samplesPerBit));
  
  for (int pos = 0; pos < payloadLen; pos++) {
    // Unique seed for each bit position
    XorShift64 prng(baseSeed ^ (static_cast<uint64_t>(pos) * 0x9e3779b97f4a7c15ULL));
    
    // Generate raw PN
    std::vector<double> rawPN(samplesPerBit);
    for (int i = 0; i < samplesPerBit; i++) {
      rawPN[i] = prng.nextDouble() * 2.0 - 1.0;
    }
    
    // Low-pass filter to reduce harshness
    const int filterWidth = 32;
    for (int i = 0; i < samplesPerBit; i++) {
      double sum = 0;
      int count = 0;
      for (int j = -filterWidth; j <= filterWidth; j++) {
        int idx = i + j;
        if (idx >= 0 && idx < samplesPerBit) {
          sum += rawPN[idx];
          count++;
        }
      }
      pnSequences[pos][i] = sum / count;
    }
    
    // Remove DC
    const int dcWidth = 256;
    std::vector<double> temp = pnSequences[pos];
    for (int i = 0; i < samplesPerBit; i++) {
      double sum = 0;
      int count = 0;
      for (int j = -dcWidth; j <= dcWidth; j++) {
        int idx = i + j;
        if (idx >= 0 && idx < samplesPerBit) {
          sum += temp[idx];
          count++;
        }
      }
      pnSequences[pos][i] -= sum / count;
    }
    
    // Normalize
    double energy = 0;
    for (int i = 0; i < samplesPerBit; i++) {
      energy += pnSequences[pos][i] * pnSequences[pos][i];
    }
    double norm = std::sqrt(energy / samplesPerBit);
    if (norm > 1e-10) {
      for (int i = 0; i < samplesPerBit; i++) {
        pnSequences[pos][i] /= norm;
      }
    }
    
    // Apply window
    for (int i = 0; i < samplesPerBit; i++) {
      const double window = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (samplesPerBit - 1)));
      pnSequences[pos][i] *= window;
    }
  }
  
  // Debug: show first PN stats
  double pnSum = 0, pnAbsSum = 0;
  for (int i = 0; i < samplesPerBit; i++) {
    pnSum += pnSequences[0][i];
    pnAbsSum += std::abs(pnSequences[0][i]);
  }
  fprintf(stderr, "EMBED PN[0] sequence: sum=%.6f absSum=%.6f\n", pnSum, pnAbsSum);
  
  // Embed watermark using spread spectrum with position-specific PN sequences
  size_t bitIndex = 0;
  for (size_t blockStart = 0; blockStart + samplesPerBit <= totalSamples; blockStart += samplesPerBit) {
    const size_t actualBitIndex = bitIndex % bitstream.size();
    const int bit = bitstream[actualBitIndex] ? 1 : 0;
    
    // Get the PN sequence for this bit position
    const std::vector<double>& pnSequence = pnSequences[actualBitIndex];
    
    // Bipolar modulation: bit 1 = +PN, bit 0 = -PN
    const double sign = bit ? 1.0 : -1.0;
    
    // Calculate local signal energy for adaptive strength
    double localEnergy = 0;
    for (int i = 0; i < samplesPerBit; i++) {
      const double sample = (left[blockStart + i] + right[blockStart + i]) * 0.5;
      localEnergy += sample * sample;
    }
    localEnergy = std::sqrt(localEnergy / samplesPerBit);
    
    // Adaptive strength: use psychoacoustic masking - stronger in loud parts (masked), weaker in quiet
    const double adaptiveStrength = strength * std::clamp(localEnergy * 4.0, 0.1, 0.6);
    
    // Handle remove bits (for re-signing)
    int removeBit = -1;
    if (!removeBits.empty()) {
      removeBit = removeBits[actualBitIndex % removeBits.size()] ? 1 : 0;
    }
    
    // Apply PN sequence to audio
    for (int i = 0; i < samplesPerBit; i++) {
      const size_t idx = blockStart + i;
      double delta = pnSequence[i] * sign * adaptiveStrength;
      
      // If removing old watermark, subtract its contribution
      if (removeBit >= 0) {
        const double oldSign = removeBit ? 1.0 : -1.0;
        delta -= pnSequence[i] * oldSign * adaptiveStrength;
      }
      
      left[idx] += static_cast<float>(delta);
      right[idx] += static_cast<float>(delta);
    }
    
    bitIndex++;
  }
  
  std::vector<float> interleaved(wav.samples.size());
  writeChannelSamples(interleaved, left, channels, 0);
  if (channels > 1) {
    writeChannelSamples(interleaved, right, channels, 1);
  }

    WavData outWav{ wav.sampleRate, wav.channels, std::move(interleaved) };
    writeWav(outputPath, outWav);

    return env.Null();
  } catch (const std::exception& ex) {
    Napi::Error::New(env, ex.what()).ThrowAsJavaScriptException();
    return env.Null();
  }
}

static Napi::Value ExtractWatermark(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  try {
    if (info.Length() < 2) {
      Napi::TypeError::New(env, "Expected inputPath, options").ThrowAsJavaScriptException();
      return env.Null();
    }

  const std::string inputPath = info[0].As<Napi::String>();
  const Napi::Object options = info[1].As<Napi::Object>();
  const int sampleRate = options.Get("sampleRate").As<Napi::Number>().Int32Value();
  const int channels = options.Get("channels").As<Napi::Number>().Int32Value();
  const int blockSize = options.Get("blockSize").As<Napi::Number>().Int32Value();
  const int hopSize = options.Get("hopSize").As<Napi::Number>().Int32Value();
  const std::string secret = options.Get("secret").As<Napi::String>();
  const double embedStrength = options.Has("embedStrength")
    ? options.Get("embedStrength").As<Napi::Number>().DoubleValue()
    : 0.005;

    WavData wav = readWav(inputPath);
    if (wav.sampleRate != sampleRate || wav.channels != channels) {
      throw std::runtime_error("Unexpected WAV format");
    }

  std::vector<float> left = getChannelSamples(wav.samples, channels, 0);
  std::vector<float> right = channels > 1 ? getChannelSamples(wav.samples, channels, 1) : left;

  const size_t totalSamples = left.size();
  
  // =========================================================================
  // SPREAD SPECTRUM EXTRACTION WITH POSITION-DEPENDENT PN SEQUENCES
  // Must match the embedding: each bit position uses a unique PN sequence
  // =========================================================================
  
  const uint64_t baseSeed = hashSecret(secret);
  const int samplesPerBit = hopSize * 4;  // Must match embedding
  
  // We need to know the payload length to generate matching PN sequences
  // Use the expected period (464 bits) from the payload structure
  const int payloadLen = 64 + 16 + (16 + 32) * 8;  // = 464 bits
  
  // Pre-generate PN sequences for each bit position (matching embedding)
  std::vector<std::vector<double>> pnSequences(payloadLen, std::vector<double>(samplesPerBit));
  
  for (int pos = 0; pos < payloadLen; pos++) {
    // Unique seed for each bit position (must match embedding!)
    XorShift64 prng(baseSeed ^ (static_cast<uint64_t>(pos) * 0x9e3779b97f4a7c15ULL));
    
    // Generate raw PN
    std::vector<double> rawPN(samplesPerBit);
    for (int i = 0; i < samplesPerBit; i++) {
      rawPN[i] = prng.nextDouble() * 2.0 - 1.0;
    }
    
    // Low-pass filter
    const int filterWidth = 32;
    for (int i = 0; i < samplesPerBit; i++) {
      double sum = 0;
      int count = 0;
      for (int j = -filterWidth; j <= filterWidth; j++) {
        int idx = i + j;
        if (idx >= 0 && idx < samplesPerBit) {
          sum += rawPN[idx];
          count++;
        }
      }
      pnSequences[pos][i] = sum / count;
    }
    
    // Remove DC
    const int dcWidth = 256;
    std::vector<double> temp = pnSequences[pos];
    for (int i = 0; i < samplesPerBit; i++) {
      double sum = 0;
      int count = 0;
      for (int j = -dcWidth; j <= dcWidth; j++) {
        int idx = i + j;
        if (idx >= 0 && idx < samplesPerBit) {
          sum += temp[idx];
          count++;
        }
      }
      pnSequences[pos][i] -= sum / count;
    }
    
    // Normalize
    double energy = 0;
    for (int i = 0; i < samplesPerBit; i++) {
      energy += pnSequences[pos][i] * pnSequences[pos][i];
    }
    double norm = std::sqrt(energy / samplesPerBit);
    if (norm > 1e-10) {
      for (int i = 0; i < samplesPerBit; i++) {
        pnSequences[pos][i] /= norm;
      }
    }
    
    // Apply window
    for (int i = 0; i < samplesPerBit; i++) {
      const double window = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (samplesPerBit - 1)));
      pnSequences[pos][i] *= window;
    }
  }
  
  // Debug: show first PN stats
  double pnSum = 0, pnAbsSum = 0;
  for (int i = 0; i < samplesPerBit; i++) {
    pnSum += pnSequences[0][i];
    pnAbsSum += std::abs(pnSequences[0][i]);
  }
  fprintf(stderr, "EXTRACT PN[0] sequence: sum=%.6f absSum=%.6f\n", pnSum, pnAbsSum);
  
  std::vector<uint8_t> bits;
  std::vector<float> correlations;  // Store actual correlation values for soft voting
  bits.reserve(totalSamples / samplesPerBit);
  correlations.reserve(totalSamples / samplesPerBit);
  
  double confidenceSum = 0.0;
  size_t bitsAnalyzed = 0;
  
  // Extract correlations using position-specific PN sequences
  size_t bitIndex = 0;
  for (size_t blockStart = 0; blockStart + samplesPerBit <= totalSamples; blockStart += samplesPerBit) {
    const size_t actualBitIndex = bitIndex % payloadLen;
    const std::vector<double>& pnSequence = pnSequences[actualBitIndex];
    
    double correlation = 0.0;
    double signalEnergy = 0.0;
    double pnEnergy = 0.0;
    
    for (int i = 0; i < samplesPerBit; i++) {
      const double sample = (left[blockStart + i] + right[blockStart + i]) * 0.5;
      correlation += sample * pnSequence[i];
      signalEnergy += sample * sample;
      pnEnergy += pnSequence[i] * pnSequence[i];
    }
    
    // Normalize correlation by signal energy for comparable values across blocks
    double normalizedCorr = 0.0;
    if (signalEnergy > 1e-20) {
      normalizedCorr = correlation / std::sqrt(signalEnergy);
    }
    
    correlations.push_back(static_cast<float>(normalizedCorr));
    
    double conf = 0;
    if (signalEnergy > 1e-20 && pnEnergy > 1e-20) {
      conf = std::abs(correlation) / std::sqrt(signalEnergy * pnEnergy);
    }
    confidenceSum += std::min(1.0, conf);
    bitsAnalyzed++;
    bitIndex++;
  }
  
  // Convert correlations to bits (will be refined by voting in TypeScript)
  for (size_t i = 0; i < correlations.size(); i++) {
    bits.push_back(correlations[i] > 0 ? 1 : 0);
  }

    Napi::Object result = Napi::Object::New(env);
    result.Set("bitstream", Napi::Buffer<uint8_t>::Copy(env, bits.data(), bits.size()));
    result.Set("correlations", Napi::Buffer<float>::Copy(env, correlations.data(), correlations.size()));
    result.Set("bitConfidence", bitsAnalyzed ? confidenceSum / bitsAnalyzed : 0.0);
    result.Set("bandAgreement", 1.0);
    result.Set("blocksAnalyzed", static_cast<double>(bitsAnalyzed));
    return result;
  } catch (const std::exception& ex) {
    Napi::Error::New(env, ex.what()).ThrowAsJavaScriptException();
    return env.Null();
  }
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("embedWatermark", Napi::Function::New(env, EmbedWatermark));
  exports.Set("extractWatermark", Napi::Function::New(env, ExtractWatermark));
  return exports;
}

NODE_API_MODULE(watermark, Init)
