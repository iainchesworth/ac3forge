#include "ac3/core/fft.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

#include "fft_radix2.hpp"

namespace ac3 {

namespace {

const internal::FftRadix2Tables<static_cast<std::size_t>(kDftLength)>& tables() {
    static const internal::FftRadix2Tables<static_cast<std::size_t>(kDftLength)> t;
    return t;
}

}  // namespace

void dft512(std::span<const double, kDftLength> real_in,
           std::span<const double, kDftLength> imag_in, std::span<double, kDftLength> real_out,
           std::span<double, kDftLength> imag_out) {
    // The output spans double as the FFT's workspace (they were never
    // permitted to alias the inputs - the old direct-form sum read every
    // input element under each output index it wrote, so aliasing was
    // already incorrect before this took over).
    std::copy(real_in.begin(), real_in.end(), real_out.begin());
    std::copy(imag_in.begin(), imag_in.end(), imag_out.begin());
    internal::fft_radix2_forward<static_cast<std::size_t>(kDftLength)>(tables(), real_out,
                                                                      imag_out);
    // The spec sum's own 1/N normalisation (see fft.hpp).
    for (std::size_t k = 0; k < static_cast<std::size_t>(kDftLength); ++k) {
        real_out[k] /= static_cast<double>(kDftLength);
        imag_out[k] /= static_cast<double>(kDftLength);
    }
}

}  // namespace ac3
