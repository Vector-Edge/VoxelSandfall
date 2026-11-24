#include "ViewModel.h"

namespace {

uint32_t rgb(int r, int g, int b) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (255u << 24); }

// Oriented box; `rotXDeg` pitches the box about its own centre (used for the grip and the curved magazine).
void addBox(ViewModelMesh& m, const Vec3& c, const Vec3& half, float rotXDeg, uint32_t color) {
    const Quat q = Quat::fromAxisAngle(Vec3(1, 0, 0), deg2rad(rotXDeg));
    static const int N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const int U[6][3] = {{0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    static const int V[6][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
    for (int f = 0; f < 6; f++) {
        const Vec3 n = q.rotate(Vec3((float)N[f][0], (float)N[f][1], (float)N[f][2]));
        const uint32_t base = (uint32_t)m.verts.size();
        for (int k = 0; k < 4; k++) {
            const float a = (k == 1 || k == 2) ? 1.f : -1.f;
            const float b = (k >= 2) ? 1.f : -1.f;
            Vec3 local(N[f][0] * half.x + U[f][0] * a * half.x + V[f][0] * b * half.x,
                       N[f][1] * half.y + U[f][1] * a * half.y + V[f][1] * b * half.y,
                       N[f][2] * half.z + U[f][2] * a * half.z + V[f][2] * b * half.z);
            const Vec3 p = c + q.rotate(local);
            m.verts.push_back({p.x, p.y, p.z, n.x, n.y, n.z, color});
        }
        const uint32_t t[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t i : t) m.idx.push_back(base + i);
    }
}

void addQuad(ViewModelMesh& m, const Vec3& c, const Vec3& u, const Vec3& v, uint32_t color) {
    const uint32_t base = (uint32_t)m.verts.size();
    const Vec3 n = normalize(cross(u, v));
    const Vec3 p[4] = {c - u - v, c + u - v, c + u + v, c - u + v};
    for (int k = 0; k < 4; k++) m.verts.push_back({p[k].x, p[k].y, p[k].z, n.x, n.y, n.z, color});
    const uint32_t t[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i : t) m.idx.push_back(base + i);
}

}  // namespace

ViewModelMesh buildRifle() {
    ViewModelMesh m;
    const uint32_t metal = rgb(52, 55, 60), metalDark = rgb(28, 29, 32), wood = rgb(142, 82, 38), woodDark = rgb(104, 58, 26);
    const uint32_t blued = rgb(38, 40, 44);

    // Rifle space: origin at the middle of the receiver, -Z is the firing direction (metres).
    addBox(m, Vec3(0, 0.000f, 0.00f), Vec3(0.024f, 0.036f, 0.135f), 0, metal);              // receiver
    addBox(m, Vec3(0, 0.040f, 0.01f), Vec3(0.020f, 0.008f, 0.120f), 0, metalDark);          // dust cover
    addBox(m, Vec3(0, -0.006f, -0.285f), Vec3(0.027f, 0.026f, 0.105f), 0, wood);            // lower handguard
    addBox(m, Vec3(0, 0.030f, -0.280f), Vec3(0.021f, 0.012f, 0.100f), 0, woodDark);         // upper handguard
    addBox(m, Vec3(0, 0.050f, -0.330f), Vec3(0.012f, 0.011f, 0.100f), 0, metal);            // gas tube
    addBox(m, Vec3(0, 0.006f, -0.505f), Vec3(0.010f, 0.010f, 0.150f), 0, blued);            // barrel
    addBox(m, Vec3(0, 0.012f, -0.630f), Vec3(0.015f, 0.032f, 0.014f), 0, metal);            // front sight base
    addBox(m, Vec3(0, 0.050f, -0.630f), Vec3(0.002f, 0.014f, 0.003f), 0, metalDark);        // front sight post
    addBox(m, Vec3(0, 0.008f, -0.676f), Vec3(0.016f, 0.016f, 0.028f), 0, metalDark);        // muzzle brake
    addBox(m, Vec3(0, 0.050f, -0.085f), Vec3(0.016f, 0.010f, 0.026f), 0, metal);            // rear sight
    addBox(m, Vec3(0, -0.082f, 0.105f), Vec3(0.017f, 0.052f, 0.022f), -20, woodDark);       // pistol grip
    addBox(m, Vec3(0, -0.046f, 0.040f), Vec3(0.005f, 0.014f, 0.040f), 0, metalDark);        // trigger guard
    addBox(m, Vec3(0, -0.012f, 0.300f), Vec3(0.020f, 0.044f, 0.125f), 0, wood);             // stock
    addBox(m, Vec3(0, -0.016f, 0.432f), Vec3(0.022f, 0.052f, 0.010f), 0, metalDark);        // butt plate

    // Banana magazine: a short arc of boxes curving toward the muzzle.
    {
        float y = -0.060f, z = -0.060f;
        for (int i = 0; i < 5; i++) {
            const float ang = 7.0f * (float)i;  // each segment leans a bit further forward
            addBox(m, Vec3(0, y, z), Vec3(0.017f, 0.034f, 0.026f), -ang, metalDark);
            y -= 0.062f * std::cos(deg2rad(ang));
            z -= 0.062f * std::sin(deg2rad(ang));
        }
    }

    m.muzzle = Vec3(0, 0.008f, -0.71f);
    m.flashFirstIndex = (uint32_t)m.idx.size();
    // Muzzle flash: three crossed quads, additive.
    const uint32_t hot = rgb(255, 205, 110), core = rgb(255, 245, 200);
    addQuad(m, m.muzzle + Vec3(0, 0, -0.02f), Vec3(0.085f, 0, 0), Vec3(0, 0.085f, 0), hot);
    addQuad(m, m.muzzle + Vec3(0, 0, -0.05f), Vec3(0, 0, 0.09f), Vec3(0, 0.032f, 0), hot);
    addQuad(m, m.muzzle + Vec3(0, 0, -0.05f), Vec3(0, 0, 0.09f), Vec3(0.032f, 0, 0), hot);
    addQuad(m, m.muzzle + Vec3(0, 0, -0.01f), Vec3(0.035f, 0, 0), Vec3(0, 0.035f, 0), core);
    return m;
}
