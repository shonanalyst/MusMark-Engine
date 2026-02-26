#include "fft.h"
#include <cmath>

constexpr double kPi = 3.14159265358979323846;

static void bitReverse(std::vector<Complex>& data) {
  const size_t n = data.size();
  size_t j = 0;
  for (size_t i = 1; i < n; i++) {
    size_t bit = n >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
    if (i < j) {
      Complex tmp = data[i];
      data[i] = data[j];
      data[j] = tmp;
    }
  }
}

void fft(std::vector<Complex>& data, bool inverse) {
  const size_t n = data.size();
  bitReverse(data);

  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = 2 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
    const double wlen_re = std::cos(ang);
    const double wlen_im = std::sin(ang);

    for (size_t i = 0; i < n; i += len) {
      double w_re = 1.0;
      double w_im = 0.0;
      for (size_t j = 0; j < len / 2; j++) {
        Complex u = data[i + j];
        Complex v = {
          data[i + j + len / 2].re * w_re - data[i + j + len / 2].im * w_im,
          data[i + j + len / 2].re * w_im + data[i + j + len / 2].im * w_re
        };

        data[i + j].re = u.re + v.re;
        data[i + j].im = u.im + v.im;
        data[i + j + len / 2].re = u.re - v.re;
        data[i + j + len / 2].im = u.im - v.im;

        const double next_re = w_re * wlen_re - w_im * wlen_im;
        const double next_im = w_re * wlen_im + w_im * wlen_re;
        w_re = next_re;
        w_im = next_im;
      }
    }
  }

  if (inverse) {
    for (size_t i = 0; i < n; i++) {
      data[i].re /= static_cast<double>(n);
      data[i].im /= static_cast<double>(n);
    }
  }
}

void applyHannWindow(std::vector<double>& buffer) {
  const size_t n = buffer.size();
  for (size_t i = 0; i < n; i++) {
    const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (n - 1)));
    buffer[i] *= w;
  }
}
