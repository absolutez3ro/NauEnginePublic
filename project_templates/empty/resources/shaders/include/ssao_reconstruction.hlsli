// Copyright 2026 N-GINN LLC. All rights reserved.
#ifndef NAU_SSAO_RECONSTRUCTION
#define NAU_SSAO_RECONSTRUCTION
float ssaoLinearDepth(float d, bool linearDepth, float2 ba)
{
    return d <= 0.0 ? 65504.0 : (linearDepth ? d : abs(ba.x / (-d - ba.y)));
}
float ssaoReconstruct(float2 uv, float fullDepth, Texture2D<float> ao, Texture2D<float> depth,
                      SamplerState linearClamp, float4 method, float4 parameters)
{
    float result = 1.0;
    if (method.x < 0.0) result = ao.Load(int3(int2(uv * parameters.zw), 0));
    else if (method.x == 1.0) result = ao.SampleLevel(linearClamp, uv, 0);
    else
    {
    uint w,h,dw,dh;
    ao.GetDimensions(w,h); depth.GetDimensions(dw,dh);
    int2 size = int2(w,h), inputSize = int2(dw,dh);
    float2 samplePosition = uv * size - 0.5;
    int2 origin = int2(floor(samplePosition));
    float fullZ = ssaoLinearDepth(fullDepth, false, parameters.xy);
    float nearestDistance = 3.402823e+38, nearestAo = 1.0;
    float total = 0.0, weights = 0.0;
    // GatherRed is exact for matching AO/depth grids; full inputs use mapped Load samples.
    float4 gatheredAo = 1.0, gatheredDepth = 0.0;
    bool gathered = method.z != 0.0 && all(size == inputSize) && method.x != 2.0;
    if (gathered)
    {
        float2 gatherUv = (origin + 1.0) / float2(size);
        gatheredAo = ao.GatherRed(linearClamp, gatherUv).wzxy;
        gatheredDepth = depth.GatherRed(linearClamp, gatherUv).wzxy;
    }
    int first = method.x == 2.0 ? -1 : 0;
    int last = method.x == 2.0 ? 2 : 1;
    for (int y = first; y <= last; ++y)
    for (int x = first; x <= last; ++x)
    {
        int2 q = clamp(origin + int2(x,y), 0, size-1);
        int2 dq = clamp(int2((q + 0.5) / size * inputSize), 0, inputSize-1);
        uint idx = uint(clamp(y*2+x,0,3));
        float a = gathered ? gatheredAo[idx] : ao.Load(int3(q,0));
        float d = gathered ? gatheredDepth[idx] : depth.Load(int3(dq,0));
        float distance = abs(ssaoLinearDepth(d, method.y != 0.0, parameters.xy) - fullZ);
        if (distance < nearestDistance) { nearestDistance = distance; nearestAo = a; }
        float spatial = method.x == 3.0 ? max(1.0-abs(samplePosition.x-(origin.x+x)),0.0) * max(1.0-abs(samplePosition.y-(origin.y+y)),0.0) : 1.0;
        float weight = spatial * saturate(1.0-distance/max(method.w,1e-5));
        total += weight*a; weights += weight;
    }
    result = method.x == 0.0 || weights <= 1e-6 ? nearestAo : saturate(total/weights);
    }
    return result;
}
#endif
