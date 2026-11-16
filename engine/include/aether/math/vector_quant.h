#pragma once

// Vector quantization: store an index into a codebook instead of the vector.
//
// Scalar quantization (snorm, the exponential meshopt filter) rounds each
// component on its own. A lattice (octahedral.h) is a codebook you can compute
// instead of store, and it is the right choice when directions are spread
// over the whole sphere.
//
// An unstructured codebook spends its entries where the data actually is.
// Lloyd's algorithm (k-means on the sphere) is that codebook for normals:
// assign each sample to the nearest entry, replace the entry with the
// normalized mean, repeat. An 8-bit index is 256 entries. On upward-biased
// normals that beats both a uniform Fibonacci book of the same size and
// 4-bit octahedral (also 8 bits). It loses on the rare directions it was
// not trained on, and it loses to 8-bit octahedral, which spends 16 bits.

#include "aether/math/vec.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aether {

struct VecBook {
    std::vector<Vec3> entries;

    static VecBook Fibonacci(int count) {
        VecBook book;
        book.entries.resize(static_cast<usize>(std::max(count, 1)));
        const f32 golden = 3.14159265f * (3.0f - std::sqrt(5.0f));
        for (int i = 0; i < count; ++i) {
            const f32 y = count == 1 ? 0.0f : 1.0f - (static_cast<f32>(i) / static_cast<f32>(count - 1)) * 2.0f;
            const f32 radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const f32 theta = static_cast<f32>(i) * golden;
            book.entries[static_cast<usize>(i)] = {std::cos(theta) * radius, y, std::sin(theta) * radius};
        }
        return book;
    }

    // Spherical k-means. Means are renormalized, so every entry stays a direction.
    static VecBook TrainNormals(const Vec3* samples, int sample_count, int codebook_size, int iterations) {
        VecBook book;
        const int k = std::clamp(codebook_size, 1, std::max(sample_count, 1));
        book.entries.reserve(static_cast<usize>(k));
        book.entries.push_back(samples[0].Normalized());
        while (static_cast<int>(book.entries.size()) < k) {
            int far = 0;
            f32 far_dist = -1.0f;
            for (int i = 0; i < sample_count; ++i) {
                const f32 dist = 1.0f - book.NearestDot(samples[i]);
                if (dist > far_dist) {
                    far_dist = dist;
                    far = i;
                }
            }
            book.entries.push_back(samples[far].Normalized());
        }
        std::vector<Vec3> sum(static_cast<usize>(k));
        std::vector<int> count(static_cast<usize>(k));
        for (int iter = 0; iter < iterations; ++iter) {
            std::fill(sum.begin(), sum.end(), Vec3{});
            std::fill(count.begin(), count.end(), 0);
            for (int i = 0; i < sample_count; ++i) {
                const int entry = book.Encode(samples[i]);
                sum[static_cast<usize>(entry)] = sum[static_cast<usize>(entry)] + samples[i];
                ++count[static_cast<usize>(entry)];
            }
            for (int i = 0; i < k; ++i) {
                if (count[static_cast<usize>(i)] == 0) {
                    int far = 0;
                    f32 far_dist = -1.0f;
                    for (int s = 0; s < sample_count; ++s) {
                        const f32 dist = 1.0f - book.NearestDot(samples[s]);
                        if (dist > far_dist) {
                            far_dist = dist;
                            far = s;
                        }
                    }
                    book.entries[static_cast<usize>(i)] = samples[far].Normalized();
                    continue;
                }
                book.entries[static_cast<usize>(i)] = sum[static_cast<usize>(i)].Normalized();
            }
        }
        return book;
    }

    int Encode(Vec3 v) const {
        int best = 0;
        f32 best_dot = -2.0f;
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const f32 d = v.Dot(entries[static_cast<usize>(i)]);
            if (d > best_dot) {
                best_dot = d;
                best = i;
            }
        }
        return best;
    }

    Vec3 Decode(int index) const { return entries[static_cast<usize>(index)]; }

    f32 NearestDot(Vec3 v) const { return v.Dot(Decode(Encode(v))); }
};

}  // namespace aether
