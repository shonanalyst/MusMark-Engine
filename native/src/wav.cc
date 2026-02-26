#include "wav.h"
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <cstring>

struct RiffHeader {
  char chunkId[4];
  uint32_t chunkSize;
  char format[4];
};

struct FmtChunk {
  char subchunk1Id[4];
  uint32_t subchunk1Size;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
};

struct DataChunkHeader {
  char subchunk2Id[4];
  uint32_t subchunk2Size;
};

static void readExact(std::ifstream& in, char* buffer, size_t size) {
  in.read(buffer, size);
  if (in.gcount() != static_cast<std::streamsize>(size)) {
    throw std::runtime_error("Unexpected EOF");
  }
}

WavData readWav(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Failed to open WAV file");
  }

  RiffHeader riff{};
  readExact(in, reinterpret_cast<char*>(&riff), sizeof(RiffHeader));
  if (std::strncmp(riff.chunkId, "RIFF", 4) != 0 || std::strncmp(riff.format, "WAVE", 4) != 0) {
    throw std::runtime_error("Invalid WAV file");
  }

  FmtChunk fmt{};
  readExact(in, reinterpret_cast<char*>(&fmt), sizeof(FmtChunk));
  if (std::strncmp(fmt.subchunk1Id, "fmt ", 4) != 0) {
    throw std::runtime_error("Invalid fmt chunk");
  }
  if (fmt.subchunk1Size > 16) {
    in.seekg(fmt.subchunk1Size - 16, std::ios::cur);
  }
  if (fmt.audioFormat != 3 || fmt.bitsPerSample != 32) {
    throw std::runtime_error("Only 32-bit float WAV supported");
  }

  DataChunkHeader dataHeader{};
  readExact(in, reinterpret_cast<char*>(&dataHeader), sizeof(DataChunkHeader));
  while (std::strncmp(dataHeader.subchunk2Id, "data", 4) != 0) {
    in.seekg(dataHeader.subchunk2Size, std::ios::cur);
    readExact(in, reinterpret_cast<char*>(&dataHeader), sizeof(DataChunkHeader));
  }

  const size_t sampleCount = dataHeader.subchunk2Size / sizeof(float);
  std::vector<float> samples(sampleCount);
  readExact(in, reinterpret_cast<char*>(samples.data()), dataHeader.subchunk2Size);

  return { static_cast<int>(fmt.sampleRate), static_cast<int>(fmt.numChannels), std::move(samples) };
}

void writeWav(const std::string& path, const WavData& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Failed to open output WAV file");
  }

  const uint32_t dataSize = static_cast<uint32_t>(data.samples.size() * sizeof(float));
  const uint32_t fmtChunkSize = 16;
  const uint32_t riffSize = 4 + (8 + fmtChunkSize) + (8 + dataSize);

  RiffHeader riff{};
  std::memcpy(riff.chunkId, "RIFF", 4);
  riff.chunkSize = riffSize;
  std::memcpy(riff.format, "WAVE", 4);

  FmtChunk fmt{};
  std::memcpy(fmt.subchunk1Id, "fmt ", 4);
  fmt.subchunk1Size = fmtChunkSize;
  fmt.audioFormat = 3;
  fmt.numChannels = static_cast<uint16_t>(data.channels);
  fmt.sampleRate = static_cast<uint32_t>(data.sampleRate);
  fmt.bitsPerSample = 32;
  fmt.byteRate = fmt.sampleRate * fmt.numChannels * (fmt.bitsPerSample / 8);
  fmt.blockAlign = fmt.numChannels * (fmt.bitsPerSample / 8);

  DataChunkHeader dataHeader{};
  std::memcpy(dataHeader.subchunk2Id, "data", 4);
  dataHeader.subchunk2Size = dataSize;

  out.write(reinterpret_cast<char*>(&riff), sizeof(RiffHeader));
  out.write(reinterpret_cast<char*>(&fmt), sizeof(FmtChunk));
  out.write(reinterpret_cast<char*>(&dataHeader), sizeof(DataChunkHeader));
  out.write(reinterpret_cast<const char*>(data.samples.data()), dataSize);
}
