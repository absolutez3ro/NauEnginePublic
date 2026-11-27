float4 blurUi(Texture2D<float4> layer, int2 pixel)
{
    uint width, height;
    layer.GetDimensions(width, height);
    int2 limit = int2(width, height) - 1;
    float4 sum = 0;
    // Symmetric tent kernel, radius eight. Each axis has total weight 81.
    [loop] for (int y = -8; y <= 8; ++y)
        [loop] for (int x = -8; x <= 8; ++x)
            sum += layer.Load(int3(clamp(pixel + int2(x, y), 0, limit), 0))
                * ((9 - abs(x)) * (9 - abs(y)));
    return sum / 6561.0;
}
