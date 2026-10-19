// Minimal math library for the engine (no external dependencies).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

constexpr float kPi = 3.14159265358979323846f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline int floorToInt(float v) { return (int)std::floor(v); }
inline float deg2rad(float d) { return d * (kPi / 180.0f); }

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { float i = 1.0f / s; return {x * i, y * i, z * i}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float lengthSq(const Vec3& v) { return dot(v, v); }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-12f ? v * (1.0f / l) : Vec3(0, 1, 0);
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct IVec3 {
    int x = 0, y = 0, z = 0;
    IVec3() = default;
    constexpr IVec3(int a, int b, int c) : x(a), y(b), z(c) {}
    bool operator==(const IVec3& o) const { return x == o.x && y == o.y && z == o.z; }
    IVec3 operator+(const IVec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    IVec3 operator-(const IVec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    int& operator[](int i) { return (&x)[i]; }
    int operator[](int i) const { return (&x)[i]; }
};

struct Mat3 {
    float m[3][3];
    static Mat3 identity() {
        Mat3 r;
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1;
        return r;
    }
    static Mat3 zero() { Mat3 r; std::memset(r.m, 0, sizeof(r.m)); return r; }
    Vec3 operator*(const Vec3& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
    Mat3 transposed() const {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i];
        return r;
    }
    float det() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
    Mat3 inverse() const {
        float d = det();
        if (std::fabs(d) < 1e-20f) return identity();
        float id = 1.0f / d;
        Mat3 r;
        r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
        r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
        r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
        r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
        r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
        r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
        r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
        r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
        r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
        return r;
    }
};

struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
    Quat() = default;
    Quat(float w_, float x_, float y_, float z_) : w(w_), x(x_), y(y_), z(z_) {}
    static Quat fromAxisAngle(const Vec3& axis, float a) {
        Vec3 n = normalize(axis);
        float s = std::sin(a * 0.5f);
        return {std::cos(a * 0.5f), n.x * s, n.y * s, n.z * s};
    }
    Quat operator*(const Quat& o) const {
        return {w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
    }
    Quat conjugate() const { return {w, -x, -y, -z}; }
    Quat normalized() const {
        float l = std::sqrt(w * w + x * x + y * y + z * z);
        if (l < 1e-12f) return {};
        float i = 1.0f / l;
        return {w * i, x * i, y * i, z * i};
    }
    Vec3 rotate(const Vec3& v) const {
        Vec3 q(x, y, z);
        Vec3 t = cross(q, v) * 2.0f;
        return v + t * w + cross(q, t);
    }
    Mat3 toMat3() const {
        float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
        Mat3 r;
        r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy - wz);     r.m[0][2] = 2 * (xz + wy);
        r.m[1][0] = 2 * (xy + wz);     r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz - wx);
        r.m[2][0] = 2 * (xz - wy);     r.m[2][1] = 2 * (yz + wx);     r.m[2][2] = 1 - 2 * (xx + yy);
        return r;
    }
    // Integrate an angular velocity (world frame) over dt.
    Quat integrate(const Vec3& omega, float dt) const {
        Quat dq(0, omega.x * dt * 0.5f, omega.y * dt * 0.5f, omega.z * dt * 0.5f);
        Quat r = dq * (*this);
        return Quat(w + r.w, x + r.x, y + r.y, z + r.z).normalized();
    }
};

// Row-major 4x4. HLSL side uses `row_major float4x4` and mul(M, v) (column-vector convention).
struct Mat4 {
    float m[4][4];
    static Mat4 identity() {
        Mat4 r;
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1;
        return r;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j] + m[i][3] * o.m[3][j];
        return r;
    }
    static Mat4 translation(const Vec3& t) {
        Mat4 r = identity();
        r.m[0][3] = t.x; r.m[1][3] = t.y; r.m[2][3] = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r = identity();
        r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
        return r;
    }
    static Mat4 fromRT(const Mat3& R, const Vec3& t) {
        Mat4 r = identity();
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = R.m[i][j];
        r.m[0][3] = t.x; r.m[1][3] = t.y; r.m[2][3] = t.z;
        return r;
    }
    // Right-handed, depth range [0,1].
    static Mat4 perspective(float fovY, float aspect, float zn, float zf) {
        float f = 1.0f / std::tan(fovY * 0.5f);
        Mat4 r;
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = zf / (zn - zf);
        r.m[2][3] = zn * zf / (zn - zf);
        r.m[3][2] = -1;
        return r;
    }
    static Mat4 ortho(float l, float r_, float b, float t, float zn, float zf) {
        Mat4 r = identity();
        r.m[0][0] = 2 / (r_ - l);  r.m[0][3] = -(r_ + l) / (r_ - l);
        r.m[1][1] = 2 / (t - b);   r.m[1][3] = -(t + b) / (t - b);
        r.m[2][2] = 1 / (zn - zf); r.m[2][3] = zn / (zn - zf);
        return r;
    }
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = normalize(target - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r = identity();
        r.m[0][0] = s.x; r.m[0][1] = s.y; r.m[0][2] = s.z; r.m[0][3] = -dot(s, eye);
        r.m[1][0] = u.x; r.m[1][1] = u.y; r.m[1][2] = u.z; r.m[1][3] = -dot(u, eye);
        r.m[2][0] = -f.x; r.m[2][1] = -f.y; r.m[2][2] = -f.z; r.m[2][3] = dot(f, eye);
        return r;
    }
};

struct Rng {
    uint32_t s = 0x9E3779B9u;
    explicit Rng(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float f() { return (next() >> 8) * (1.0f / 16777216.0f); }
    int below(int n) { return (int)(next() % (uint32_t)n); }
    float range(float a, float b) { return a + (b - a) * f(); }
};
