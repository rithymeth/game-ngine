#include "aether/math/math.h"
#include "aether/math/octahedral.h"
#include "aether/math/vector_quant.h"
#include "aether/math/world_origin.h"
#include "test_framework.h"


using namespace aether;

AETHER_TEST(Vec3_DotAndCross) {
    Vec3 a(1, 0, 0);
    Vec3 b(0, 1, 0);

    AETHER_CHECK_NEAR(a.Dot(b), 0.0f, 1e-6);
    Vec3 c = a.Cross(b);
    AETHER_CHECK_NEAR(c.x, 0.0f, 1e-6);
    AETHER_CHECK_NEAR(c.y, 0.0f, 1e-6);
    AETHER_CHECK_NEAR(c.z, 1.0f, 1e-6);
}

AETHER_TEST(Vec3_Normalize) {
    Vec3 v(3, 4, 0);
    Vec3 n = v.Normalized();
    AETHER_CHECK_NEAR(n.Length(), 1.0f, 1e-5);
}

AETHER_TEST(Vec4_SimdDot) {
    Vec4 a(1, 2, 3, 4);
    Vec4 b(4, 3, 2, 1);
    AETHER_CHECK_NEAR(a.Dot(b), 1*4 + 2*3 + 3*2 + 4*1, 1e-5);
}

AETHER_TEST(Mat4_IdentityIsNeutral) {
    Mat4 id = Mat4::Identity();
    Vec4 v(1, 2, 3, 1);
    Vec4 r = id * v;
    AETHER_CHECK_NEAR(r.x, 1.0f, 1e-6);
    AETHER_CHECK_NEAR(r.y, 2.0f, 1e-6);
    AETHER_CHECK_NEAR(r.z, 3.0f, 1e-6);
    AETHER_CHECK_NEAR(r.w, 1.0f, 1e-6);
}

AETHER_TEST(Mat4_TranslationMovesPoint) {
    Mat4 t = Mat4::Translation(Vec3(10, 20, 30));
    Vec4 p(1, 1, 1, 1);
    Vec4 r = t * p;
    AETHER_CHECK_NEAR(r.x, 11.0f, 1e-5);
    AETHER_CHECK_NEAR(r.y, 21.0f, 1e-5);
    AETHER_CHECK_NEAR(r.z, 31.0f, 1e-5);
}

AETHER_TEST(Mat4_MultiplyComposesTransforms) {
    Mat4 t = Mat4::Translation(Vec3(1, 0, 0));
    Mat4 s = Mat4::Scale(Vec3(2, 2, 2));
    Mat4 combined = t * s; // scale first, then translate

    Vec4 p(1, 1, 1, 1);
    Vec4 r = combined * p;
    AETHER_CHECK_NEAR(r.x, 3.0f, 1e-5); // (1*2) + 1
    AETHER_CHECK_NEAR(r.y, 2.0f, 1e-5);
    AETHER_CHECK_NEAR(r.z, 2.0f, 1e-5);
}

AETHER_TEST(Quaternion_IdentityRotationIsNoOp) {
    Quaternion q = Quaternion::Identity();
    Mat4 m = q.ToMat4();
    Vec4 p(1, 2, 3, 1);
    Vec4 r = m * p;
    AETHER_CHECK_NEAR(r.x, 1.0f, 1e-5);
    AETHER_CHECK_NEAR(r.y, 2.0f, 1e-5);
    AETHER_CHECK_NEAR(r.z, 3.0f, 1e-5);
}

AETHER_TEST(Quaternion_90DegreeYawRotatesXToZ) {
    Quaternion q = Quaternion::FromAxisAngle(Vec3(0, 1, 0), Radians(90.0f));
    Mat4 m = q.ToMat4();
    Vec4 p(1, 0, 0, 1);
    Vec4 r = m * p;
    // Right-handed rotation about +Y by 90deg sends +X toward -Z.
    AETHER_CHECK_NEAR(r.x, 0.0f, 1e-4);
    AETHER_CHECK_NEAR(r.y, 0.0f, 1e-4);
    AETHER_CHECK_NEAR(r.z, -1.0f, 1e-4);
}

AETHER_TEST(Quaternion_ComposedRotationIsNormalized) {
    Quaternion a = Quaternion::FromAxisAngle(Vec3(1, 0, 0), Radians(45.0f));
    Quaternion b = Quaternion::FromAxisAngle(Vec3(0, 1, 0), Radians(30.0f));
    Quaternion c = (a * b).Normalized();
    AETHER_CHECK_NEAR(c.Length(), 1.0f, 1e-5);
}

// Phase 42.1: a double-precision position keeps separation that f32 loses.
AETHER_TEST(Vec3d_PrecisionBeyondFloat) {
    const Vec3d a(100000.0, 0.0, 0.0);      // 100 km out
    const Vec3d b(100000.001, 0.0, 0.0);    // 1 mm further
    AETHER_CHECK((b - a).Length() > 0.0009);
    // f32 cannot tell them apart, which is why a world origin is needed.
    AETHER_CHECK(a.ToVec3().x == b.ToVec3().x);
}

// Phase 42.1: the origin turns absolute positions into small f32 ones and back.
AETHER_TEST(WorldOrigin_RebasesIntoRelativeSpace) {
    WorldOrigin origin;
    origin.absolute = Vec3d(100000.0, 0.0, 200000.0);
    const Vec3d world(100003.0, 1.0, 199998.0);

    const Vec3 relative = origin.ToRelative(world);
    AETHER_CHECK_NEAR(relative.x, 3.0f, 1e-4);
    AETHER_CHECK_NEAR(relative.y, 1.0f, 1e-4);
    AETHER_CHECK_NEAR(relative.z, -2.0f, 1e-4);

    const Vec3d back = origin.ToAbsolute(relative);
    AETHER_CHECK_NEAR(back.x, world.x, 1e-6);
    AETHER_CHECK_NEAR(back.y, world.y, 1e-6);
    AETHER_CHECK_NEAR(back.z, world.z, 1e-6);

    // It only moves when the point strays past the threshold.
    AETHER_CHECK(!RebaseOrigin(origin, Vec3d(100010.0, 0.0, 200000.0), 25.0));
    AETHER_CHECK(RebaseOrigin(origin, world, 1.0));
    AETHER_CHECK_NEAR(origin.absolute.x, world.x, 1e-9);
    AETHER_CHECK_NEAR(origin.ToRelative(world).Length(), 0.0f, 1e-6);
}

// 8-bit octahedral is about a degree. Best-fit searches the 3x3 around the
// direct code and keeps the closer decode, so the worst error drops.
AETHER_TEST(Octahedral_BestFitBeatsDirectAt8Bit) {
    const Vec3 axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : axes) {
        const Vec3 back = OctDecode(OctEncodeDirect(n, 8), 8);
        AETHER_CHECK_NEAR(back.x, n.x, 1e-5f);
        AETHER_CHECK_NEAR(back.y, n.y, 1e-5f);
        AETHER_CHECK_NEAR(back.z, n.z, 1e-5f);
    }

    constexpr int kCount = 128;
    const f32 gold = 3.14159265f * (3.0f - std::sqrt(5.0f));
    f32 direct_sum = 0.0f;
    f32 fit_sum = 0.0f;
    f32 direct_max = 0.0f;
    f32 fit_max = 0.0f;
    f32 wide_max = 0.0f;
    int improved = 0;
    for (int i = 0; i < kCount; ++i) {
        const f32 y = 1.0f - (static_cast<f32>(i) / (kCount - 1)) * 2.0f;
        const f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const f32 t = static_cast<f32>(i) * gold;
        const Vec3 n{std::cos(t) * r, y, std::sin(t) * r};
        // The angle between two unit vectors from the length of their cross product: acos of the dot product cannot see anything under 0.0198 degrees in float (one
        // step below 1 is already that), which is more than the 16-bit error being checked against 0.01.
        const auto degrees = [](Vec3 a, Vec3 b) {
            return std::atan2(a.Cross(b).Length(), a.Dot(b)) * (180.0f / 3.14159265f);
        };
        const f32 direct = degrees(n, OctDecode(OctEncodeDirect(n, 8), 8));
        const f32 fit = degrees(n, OctDecode(OctEncodeBestFit(n, 8), 8));
        const f32 wide = degrees(n, OctDecode(OctEncodeDirect(n, 16), 16));
        direct_sum += direct;
        fit_sum += fit;
        direct_max = std::max(direct_max, direct);
        fit_max = std::max(fit_max, fit);
        wide_max = std::max(wide_max, wide);
        if (fit < direct - 1e-4f) ++improved;
    }
    AETHER_CHECK(fit_max <= direct_max);
    AETHER_CHECK(fit_sum < direct_sum);
    AETHER_CHECK(improved > 0);
    AETHER_CHECK(direct_max < 1.0f);
    AETHER_CHECK(fit_max < 0.7f);
    AETHER_CHECK(wide_max < 0.01f);
}

// An 8-bit codebook (256 normals) trained on upward-biased directions beats a
// uniform book of the same size, and beats 4-bit octahedral, which is also 8 bits.
// 8-bit octahedral spends 16 bits and is still much finer.
AETHER_TEST(VectorQuant_TrainedBookBeatsUniformOnBiasedNormals) {
    auto fib = [](int i, int n, f32 phase) {
        const f32 golden = 3.14159265f * (3.0f - std::sqrt(5.0f));
        const f32 y = 1.0f - (static_cast<f32>(i) / static_cast<f32>(n - 1)) * 2.0f;
        const f32 radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const f32 theta = static_cast<f32>(i) * golden + phase;
        return Vec3{std::cos(theta) * radius, y, std::sin(theta) * radius};
    };
    auto bias = [&](int i, int n, f32 phase) {
        Vec3 v = fib(i, n, phase);
        if (v.y < 0.0f) v.y *= 0.25f;
        return v.Normalized();
    };
    constexpr int kTrain = 600;
    constexpr int kTest = 120;
    Vec3 train[kTrain];
    Vec3 test[kTest];
    for (int i = 0; i < kTrain; ++i) train[i] = bias(i, kTrain, 0.0f);
    for (int i = 0; i < kTest; ++i) test[i] = bias(i, kTest, 0.7f);

    const VecBook trained = VecBook::TrainNormals(train, kTrain, 256, 4);
    const VecBook uniform = VecBook::Fibonacci(256);
    const auto mean_error = [](const VecBook& book, const Vec3* samples, int count) {
        f32 sum = 0.0f;
        for (int i = 0; i < count; ++i) {
            const Vec3 got = book.Decode(book.Encode(samples[i]));
            const f32 degrees = std::acos(std::clamp(samples[i].Dot(got), -1.0f, 1.0f)) * (180.0f / 3.14159265f);
            sum += degrees;
        }
        return sum / static_cast<f32>(count);
    };
    f32 oct4 = 0.0f;
    f32 oct8 = 0.0f;
    for (int i = 0; i < kTest; ++i) {
        const auto degrees = [&](int bits) {
            const Vec3 got = OctDecode(OctEncodeDirect(test[i], bits), bits);
            return std::acos(std::clamp(test[i].Dot(got), -1.0f, 1.0f)) * (180.0f / 3.14159265f);
        };
        oct4 += degrees(4);
        oct8 += degrees(8);
    }
    oct4 /= kTest;
    oct8 /= kTest;
    const f32 trained_mean = mean_error(trained, test, kTest);
    const f32 uniform_mean = mean_error(uniform, test, kTest);
    AETHER_CHECK(trained_mean < uniform_mean);
    AETHER_CHECK(trained_mean < oct4);
    AETHER_CHECK(oct8 < 1.0f);
}

// A second book on the residual, 16 + 16 entries, beats stopping after the
// 16-entry coarse book. Same search shape as one small book, two indexes.
AETHER_TEST(VectorQuant_ResidualStageCutsNormalError) {
    auto fib = [](int i, int n, f32 phase) {
        const f32 golden = 3.14159265f * (3.0f - std::sqrt(5.0f));
        const f32 y = 1.0f - (static_cast<f32>(i) / static_cast<f32>(n - 1)) * 2.0f;
        const f32 radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const f32 theta = static_cast<f32>(i) * golden + phase;
        return Vec3{std::cos(theta) * radius, y, std::sin(theta) * radius};
    };
    auto bias = [&](int i, int n, f32 phase) {
        Vec3 v = fib(i, n, phase);
        if (v.y < 0.0f) v.y *= 0.25f;
        return v.Normalized();
    };
    constexpr int kTrain = 400;
    constexpr int kTest = 80;
    Vec3 train[kTrain];
    Vec3 test[kTest];
    for (int i = 0; i < kTrain; ++i) train[i] = bias(i, kTrain, 0.0f);
    for (int i = 0; i < kTest; ++i) test[i] = bias(i, kTest, 0.7f);

    const VecBook coarse = VecBook::TrainNormals(train, kTrain, 16, 4);
    const ResidualNormals residual = ResidualNormals::Train(train, kTrain, 16, 16, 4);
    const auto mean_degrees = [](auto&& decode, const Vec3* samples, int count) {
        f32 sum = 0.0f;
        for (int i = 0; i < count; ++i) {
            const Vec3 got = decode(samples[i]);
            sum += std::acos(std::clamp(samples[i].Dot(got.Normalized()), -1.0f, 1.0f)) * (180.0f / 3.14159265f);
        }
        return sum / static_cast<f32>(count);
    };
    const f32 coarse_mean = mean_degrees([&](Vec3 v) { return coarse.Decode(coarse.Encode(v)); }, test, kTest);
    const f32 residual_mean = mean_degrees([&](Vec3 v) { return residual.Decode(v); }, test, kTest);
    AETHER_CHECK(residual_mean < coarse_mean * 0.7f);
}
