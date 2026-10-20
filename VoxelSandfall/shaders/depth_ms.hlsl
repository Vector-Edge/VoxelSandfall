// Scene depth of the multisampled target, one value per pixel: the farthest of the pixel's samples (the fire volume glows a little
// over the silhouette of whatever hides it instead of being cut off dark).
Texture2DMS<float> depthMS : register(t7);

float PSDepthMS(float4 pos : SV_Position) : SV_Target
{
    uint w, h, n;
    depthMS.GetDimensions(w, h, n);
    float z = 0.0;
    for (uint s = 0; s < n; s++) z = max(z, depthMS.Load(int2(pos.xy), s));
    return z;
}
