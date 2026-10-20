// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
// Minimal row-major 4x4 math using the ROW-VECTOR convention (p' = p * M), matching the game.
#include <cmath>
#include <cstring>

namespace avr {

struct V3 { float x, y, z; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(V3 a) { float l = Len(a); return l > 1e-12f ? a * (1.0f / l) : a; }

struct M4 {
    float m[4][4];
    static M4 Identity() { M4 r{}; for (int i = 0; i < 4; ++i) r.m[i][i] = 1; return r; }
    V3 Row(int i) const { return {m[i][0], m[i][1], m[i][2]}; }
    void SetRow(int i, V3 v, float w) { m[i][0] = v.x; m[i][1] = v.y; m[i][2] = v.z; m[i][3] = w; }
};

inline M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Inverse of an affine transform whose upper 3x3 is orthonormal (rotation) - row-vector convention.
inline M4 InverseRigid(const M4& a) {
    M4 r = M4::Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    V3 t = a.Row(3);
    for (int j = 0; j < 3; ++j) r.m[3][j] = -(t.x * r.m[0][j] + t.y * r.m[1][j] + t.z * r.m[2][j]);
    return r;
}

// Rotation matrix (row-vector convention) from a unit quaternion.
inline M4 FromQuat(float x, float y, float z, float w) {
    M4 r = M4::Identity();
    // column-vector rotation R; row-vector form is R^T
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    float R[3][3] = {
        {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
        {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
        {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = R[j][i];
    return r;
}

// D3D left-handed perspective from tangents, row-vector convention, depth 0..1.
inline M4 ProjFromTangents(float tl, float tr, float tu, float td, float n, float f) {
    M4 p{};
    p.m[0][0] = 2.0f / (tr - tl);
    p.m[1][1] = 2.0f / (tu - td);
    p.m[2][0] = -(tr + tl) / (tr - tl);
    p.m[2][1] = -(tu + td) / (tu - td);
    p.m[2][2] = f / (f - n);
    p.m[2][3] = 1.0f;
    p.m[3][2] = -n * f / (f - n);
    return p;
}

} // namespace avr
