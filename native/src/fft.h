#pragma once
#include <vector>

struct Complex {
  double re;
  double im;
};

void fft(std::vector<Complex>& data, bool inverse);
void applyHannWindow(std::vector<double>& buffer);
