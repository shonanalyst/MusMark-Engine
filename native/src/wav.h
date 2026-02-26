#pragma once
#include <string>
#include <vector>

struct WavData {
  int sampleRate;
  int channels;
  std::vector<float> samples;
};

WavData readWav(const std::string& path);
void writeWav(const std::string& path, const WavData& data);
