#include "Mesher.h"

namespace {

struct FaceDef {
    int n[3];       // outward normal
    int u[3], v[3]; // tangent axes, u x v == n (counter-clockwise seen from outside)
    int o[3];       // corner offset of the (0,0) vertex within the voxel
};

const FaceDef kFaces[6] = {
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}},    // +X
    {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 0, 0}},   // -X
    {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}},    // +Y
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}},   // -Y
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}},    // +Z
    {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 0}},   // -Z
};

// Meshes a padded material grid: padded dims (nx+2, ny+2, nz+2), voxel (x,y,z) lives at padded (x+1,y+1,z+1).
void meshPadded(const uint8_t* pad, int nx, int ny, int nz, const Vec3& origin, MeshData& opaque, MeshData* water) {
    const int W = nx + 2, H = ny + 2;
    auto at = [&](int x, int y, int z) -> uint8_t { return pad[(size_t)(x + 1) + (size_t)W * ((size_t)(y + 1) + (size_t)H * (size_t)(z + 1))]; };

    for (int z = 0; z < nz; z++)
        for (int y = 0; y < ny; y++)
            for (int x = 0; x < nx; x++) {
                const uint8_t m = at(x, y, z);
                if (m == M_AIR) continue;
                const bool solid = isOpaque(m);
                if (!solid && !(water && m == M_WATER)) continue;
                for (int f = 0; f < 6; f++) {
                    const FaceDef& fd = kFaces[f];
                    const uint8_t nb = at(x + fd.n[0], y + fd.n[1], z + fd.n[2]);
                    if (solid ? isOpaque(nb) : nb != M_AIR) continue;

                    MeshData& md = solid ? opaque : *water;
                    int ao[4] = {3, 3, 3, 3};
                    if (solid) {
                        const int fx = x + fd.n[0], fy = y + fd.n[1], fz = z + fd.n[2];
                        for (int q = 0; q < 4; q++) {
                            const int a = (q == 1 || q == 2) ? 1 : 0;
                            const int b = (q >= 2) ? 1 : 0;
                            const int su = a ? 1 : -1, sv = b ? 1 : -1;
                            auto occ = [&](int du, int dv) {
                                return isOpaque(at(fx + fd.u[0] * du + fd.v[0] * dv, fy + fd.u[1] * du + fd.v[1] * dv,
                                                   fz + fd.u[2] * du + fd.v[2] * dv)) ? 1 : 0;
                            };
                            const int s1 = occ(su, 0), s2 = occ(0, sv), c = occ(su, sv);
                            ao[q] = (s1 && s2) ? 0 : 3 - (s1 + s2 + c);
                        }
                    }
                    const uint32_t base = (uint32_t)md.verts.size();
                    for (int q = 0; q < 4; q++) {
                        const int a = (q == 1 || q == 2) ? 1 : 0;
                        const int b = (q >= 2) ? 1 : 0;
                        MeshVertex mv;
                        mv.x = origin.x + x + fd.o[0] + fd.u[0] * a + fd.v[0] * b;
                        mv.y = origin.y + y + fd.o[1] + fd.u[1] * a + fd.v[1] * b;
                        mv.z = origin.z + z + fd.o[2] + fd.u[2] * a + fd.v[2] * b;
                        mv.attr = (uint32_t)f | ((uint32_t)(m & 7) << 3) | ((uint32_t)ao[q] << 6);
                        md.verts.push_back(mv);
                    }
                    // Split the quad along the diagonal joining the two brighter corners (avoids AO anisotropy).
                    if (ao[1] + ao[3] >= ao[0] + ao[2]) {
                        const uint32_t t[6] = {1, 2, 3, 1, 3, 0};
                        for (uint32_t i : t) md.idx.push_back(base + i);
                    } else {
                        const uint32_t t[6] = {0, 1, 2, 0, 2, 3};
                        for (uint32_t i : t) md.idx.push_back(base + i);
                    }
                }
            }
}

}  // namespace

namespace {

// Greedy mesher for one 32^3 chunk. `pad` is the (N+2)^3 padded material grid. Chunk meshes carry no
// per-vertex AO: the pixel shader derives it from the occupancy volume, which lets coplanar voxels of the
// same material merge into large quads (the water sheet or a wall becomes a handful of triangles).
void greedyFaces(const uint8_t* pad, int N, int yLo, int yHi, const Vec3& origin, bool water, MeshData& out) {
    const int P = N + 2;
    static thread_local uint8_t mask[32 * 32];
    auto at = [&](const int c[3]) -> uint8_t { return pad[(size_t)(c[0] + 1) + (size_t)P * ((size_t)(c[1] + 1) + (size_t)P * (size_t)(c[2] + 1))]; };
    constexpr float kGrow = 0.003f;  // overlap neighbouring quads slightly so T-junctions cannot crack

    for (int f = 0; f < 6; f++) {
        const FaceDef& fd = kFaces[f];
        int aN = 0, aU = 0, aV = 0;
        for (int i = 0; i < 3; i++) {
            if (fd.n[i]) aN = i;
            if (fd.u[i]) aU = i;
            if (fd.v[i]) aV = i;
        }
        // Only the y-range that holds voxels can produce faces, so skip everything else.
        const int sLo = aN == 1 ? yLo : 0, sHi = aN == 1 ? yHi : N - 1;
        const int uLo = aU == 1 ? yLo : 0, uHi = aU == 1 ? yHi : N - 1;
        const int vLo = aV == 1 ? yLo : 0, vHi = aV == 1 ? yHi : N - 1;
        for (int s = sLo; s <= sHi; s++) {
            bool any = false;
            for (int v = vLo; v <= vHi; v++)
                for (int u = uLo; u <= uHi; u++) {
                    int c[3];
                    c[aN] = s; c[aU] = u; c[aV] = v;
                    const uint8_t m = at(c);
                    uint8_t val = 0;
                    if (m != M_AIR) {
                        int nb[3] = {c[0] + fd.n[0], c[1] + fd.n[1], c[2] + fd.n[2]};
                        const uint8_t t = at(nb);
                        if (water) val = (m == M_WATER && t == M_AIR) ? (uint8_t)M_WATER : (uint8_t)0;
                        else val = (isOpaque(m) && !isOpaque(t)) ? m : (uint8_t)0;
                    }
                    mask[v * N + u] = val;
                    any |= val != 0;
                }
            if (!any) continue;
            for (int v0 = vLo; v0 <= vHi; v0++)
                for (int u0 = uLo; u0 <= uHi; u0++) {
                    const uint8_t val = mask[v0 * N + u0];
                    if (!val) continue;
                    int w = 1;
                    while (u0 + w <= uHi && mask[v0 * N + u0 + w] == val) w++;
                    int h = 1;
                    for (; v0 + h <= vHi; h++) {
                        bool ok = true;
                        for (int k = 0; k < w; k++)
                            if (mask[(v0 + h) * N + u0 + k] != val) { ok = false; break; }
                        if (!ok) break;
                    }
                    for (int vv = 0; vv < h; vv++)
                        for (int uu = 0; uu < w; uu++) mask[(v0 + vv) * N + u0 + uu] = 0;

                    float base[3] = {0, 0, 0};
                    base[aN] = (float)s + (float)fd.o[aN];
                    base[aU] = (float)u0;
                    base[aV] = (float)v0;
                    const uint32_t first = (uint32_t)out.verts.size();
                    const uint32_t attr = (uint32_t)f | ((uint32_t)(val & 7) << 3) | (3u << 6);
                    for (int q = 0; q < 4; q++) {
                        const bool ua = (q == 1 || q == 2), vb = (q >= 2);
                        float p3[3] = {base[0], base[1], base[2]};
                        p3[aU] += ua ? (float)w + kGrow : -kGrow;
                        p3[aV] += vb ? (float)h + kGrow : -kGrow;
                        out.verts.push_back({origin.x + p3[0], origin.y + p3[1], origin.z + p3[2], attr});
                    }
                    const uint32_t t[6] = {0, 1, 2, 0, 2, 3};
                    for (uint32_t i : t) out.idx.push_back(first + i);
                }
        }
    }
}

}  // namespace

void buildChunkMesh(const World& w, int cx, int cy, int cz, MeshData& opaque, MeshData& water) {
    constexpr int N = World::CS, P = N + 2;
    static thread_local std::vector<uint8_t> pad;
    pad.resize((size_t)P * P * P);
    const int ox = cx * N, oy = cy * N, oz = cz * N;
    const uint8_t* chunk = w.chunkData(w.chunkIndex(cx, cy, cz));
    bool any = false;
    int yLo = N, yHi = -1;  // y-range (chunk-local) that contains non-air voxels
    for (int z = 0; z < P; z++)
        for (int y = 0; y < P; y++) {
            uint8_t* row = &pad[(size_t)P * ((size_t)y + (size_t)P * (size_t)z)];
            const bool inside = z >= 1 && z <= N && y >= 1 && y <= N;
            if (inside) {
                // Interior of the chunk: rows are contiguous in x (x + 32*y + 1024*z).
                const uint8_t* src = chunk + ((size_t)(z - 1) << 10) + ((size_t)(y - 1) << 5);
                uint8_t acc = 0;
                for (int x = 0; x < N; x++) {
                    uint8_t m = src[x] & VOX_MASK;
                    row[x + 1] = m;
                    acc |= m;
                }
                if (acc) {
                    any = true;
                    yLo = std::min(yLo, y - 1);
                    yHi = std::max(yHi, y - 1);
                }
                row[0] = w.getForMesh(ox - 1, oy + y - 1, oz + z - 1);
                row[P - 1] = w.getForMesh(ox + N, oy + y - 1, oz + z - 1);
            } else {
                for (int x = 0; x < P; x++) row[x] = w.getForMesh(ox + x - 1, oy + y - 1, oz + z - 1);
            }
        }
    opaque.clear();
    water.clear();
    if (!any) return;
    greedyFaces(pad.data(), N, yLo, yHi, Vec3((float)ox, (float)oy, (float)oz), false, opaque);
    greedyFaces(pad.data(), N, yLo, yHi, Vec3((float)ox, (float)oy, (float)oz), true, water);
}

// Occupancy bricks (8^3 voxels each, 4x4x4 per chunk) used by the shader to skip empty space.
void computeBricks(const World& w, int chunkIndex, uint8_t out[64]) {
    std::memset(out, 0, 64);
    const uint8_t* c = w.chunkData(chunkIndex);
    for (int z = 0; z < 32; z++)
        for (int y = 0; y < 32; y++) {
            const uint8_t* row = c + (z << 10) + (y << 5);
            uint8_t* o = out + ((y >> 3) << 2) + ((z >> 3) << 4);
            for (int b = 0; b < 4; b++) {
                uint8_t occupied = 0;
                for (int x = 0; x < 8; x++) {
                    uint8_t m = row[b * 8 + x] & VOX_MASK;
                    occupied |= (m == M_SAND || m >= M_STONE);
                }
                o[b] |= occupied;
            }
        }
}

void buildBodyMesh(const Body& b, MeshData& opaque) {
    opaque.clear();
    if (b.count == 0) return;
    const int P0 = b.nx + 2, P1 = b.ny + 2, P2 = b.nz + 2;
    std::vector<uint8_t> pad((size_t)P0 * P1 * P2, 0);
    for (int k = 0; k < b.nz; k++)
        for (int j = 0; j < b.ny; j++)
            for (int i = 0; i < b.nx; i++)
                if (b.occupiedAt(i, j, k)) pad[(size_t)(i + 1) + (size_t)P0 * ((size_t)(j + 1) + (size_t)P1 * (size_t)(k + 1))] = M_STONE;
    meshPadded(pad.data(), b.nx, b.ny, b.nz, Vec3(0, 0, 0), opaque, nullptr);
}

void appendCube(MeshData& m, const Vec3& c, float size, uint8_t material) {
    const float h = size * 0.5f;
    for (int f = 0; f < 6; f++) {
        const FaceDef& fd = kFaces[f];
        const uint32_t base = (uint32_t)m.verts.size();
        for (int q = 0; q < 4; q++) {
            const int a = (q == 1 || q == 2) ? 1 : 0;
            const int b = (q >= 2) ? 1 : 0;
            MeshVertex mv;
            // corner in [-h, h]: (o + a*u + b*v) in {0,1} per axis mapped to -h/+h
            mv.x = c.x + ((fd.o[0] + fd.u[0] * a + fd.v[0] * b) ? h : -h);
            mv.y = c.y + ((fd.o[1] + fd.u[1] * a + fd.v[1] * b) ? h : -h);
            mv.z = c.z + ((fd.o[2] + fd.u[2] * a + fd.v[2] * b) ? h : -h);
            mv.attr = (uint32_t)f | ((uint32_t)(material & 7) << 3) | (3u << 6);
            m.verts.push_back(mv);
        }
        const uint32_t t[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t i : t) m.idx.push_back(base + i);
    }
}
