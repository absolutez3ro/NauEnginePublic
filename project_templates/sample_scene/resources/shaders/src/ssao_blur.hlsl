// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ssao_common.hlsli"
#include "ssao_reconstruction.hlsli"
Texture2D<float> aoFullDepth : register(t3);

struct DownsampleOutput { float2 normal : SV_Target0; float depth : SV_Target1; };
DownsampleOutput PSDownsample(float4 position : SV_Position)
{
    uint w,h; aoDepth.GetDimensions(w,h);
    int2 pixel = min(int2(position.xy) * int2(ceil(float2(w,h)/aoSizes.zw)), int2(w,h)-1);
    float d = aoDepth.Load(int3(pixel,0));
    float2 n = aoNormals.Load(int3(pixel,0));
    DownsampleOutput o;
    o.normal = aoOptions.y != 0.0 ? encodeOct(decodeSphere(n)) : n;
    o.depth = aoOptions.z != 0.0 && d > 0.0 ? min(abs(reconstructViewPosition((pixel+0.5)/float2(w,h),d).z),65504.0) : d;
    return o;
}
DownsampleOutput PSFullInputs(float4 p : SV_Position) { return PSDownsample(p); }

float storedAoDepth(int2 pixel)
{
    int2 size = int2(aoSizes.xy);
    int2 q = clamp(pixel, 0, size-1);
    return aoDepth.Load(int3(clamp(int2((q+0.5)/size*aoSizes.zw),0,int2(aoSizes.zw)-1),0));
}

float gaussian(float4 position)
{
    static const float weights[11] = {0.022190548,0.045589000,0.079811408,0.119064630,0.151360810,0.163967208,0.151360810,0.119064630,0.079811408,0.045589000,0.022190548};
    int2 pixel = int2(position.xy), size = int2(aoSizes.xy);
    float2 uv = (pixel+0.5)/size;
    // Match the original Bogdanov full-input sampling; half inputs use the same real texels as box.
    bool fullInputs = any(aoSizes.zw != aoSizes.xy);
    float stored = fullInputs ? aoDepth.SampleLevel(aoLinearClamp,uv,0) : storedAoDepth(pixel);
    if (stored <= 0.0) return 1.0;
    float3 n = readViewNormalAtUv(uv);
    float center = viewDepth(stored), total = weights[5]*aoInput.Load(int3(pixel,0)), sum = weights[5];
    for (int i=-5; i<=5; ++i)
    {
        if (i==0) continue;
        int2 q=clamp(pixel+(aoPipeline.y==0.0 ? int2(i,0) : int2(0,i)),0,size-1);
        float2 quv=(q+0.5)/size;
        float d=fullInputs ? aoDepth.SampleLevel(aoLinearClamp,quv,0) : storedAoDepth(q);
        if (d>0.0 && dot(n,readViewNormalAtUv(quv))>=0.8 && abs(viewDepth(d)-center)<=aoFilter.z)
        { float weight=weights[i+5]; total+=weight*aoInput.Load(int3(q,0)); sum+=weight; }
    }
    return saturate(total/sum);
}
float PSHalfBlur(float4 position : SV_Position) : SV_Target
{
    if (aoPipeline.x == 0.0 || aoPipeline.x == 4.0) return aoInput.Load(int3(int2(position.xy),0));
    if (aoPipeline.x==3.0) return gaussian(position);
    int2 pixel=int2(position.xy), size=int2(aoSizes.xy);
    float center=viewDepth(storedAoDepth(pixel));
    float total=0.0, sum=0.0;
    if (aoPipeline.x==2.0 && all(aoSizes.xy==aoSizes.zw))
    {
        // Four 2x2 blocks cover exactly the same [-1,+2] box, with clamp at the border.
        [unroll] for (int by=0;by<2;++by)
        [unroll] for (int bx=0;bx<2;++bx)
        {
            float2 uv=(pixel+int2(bx*2,by*2))/float2(size);
            float4 depths=aoDepth.GatherRed(aoLinearClamp,uv);
            float4 values=aoInput.GatherRed(aoLinearClamp,uv);
            [unroll] for (int j=0;j<4;++j)
            { float weight=saturate(1.0-abs(viewDepth(depths[j])-center)/max(aoFilter.z,1e-5)); total+=weight*values[j]; sum+=weight; }
        }
    }
    else
    {
        [unroll] for (int y=-1;y<=2;++y)
        [unroll] for (int x=-1;x<=2;++x)
        {
            int2 q=clamp(pixel+int2(x,y),0,size-1);
            float weight=saturate(1.0-abs(viewDepth(storedAoDepth(q))-center)/max(aoFilter.z,1e-5));
            total+=weight*aoInput.Load(int3(q,0)); sum+=weight;
        }
    }
    return sum>1e-6 ? saturate(total/sum) : aoInput.Load(int3(pixel,0));
}
float PSBogdanov(float4 p : SV_Position) : SV_Target { return gaussian(p); }
float PSMain(float4 p : SV_Position) : SV_Target { return PSHalfBlur(p); }
float reconstruction(float4 p)
{
    uint w,h; aoFullDepth.GetDimensions(w,h);
    return ssaoReconstruct(p.xy/float2(w,h),aoFullDepth.Load(int3(int2(p.xy),0)),aoInput,aoDepth,aoLinearClamp,
        float4(aoReconstruction.x,aoOptions.z,aoReconstruction.y,aoFilter.z),
        float4(aoProjection[2][3],aoProjection[2][2],w,h));
}
float PSUpsample(float4 p : SV_Position) : SV_Target { return reconstruction(p); }
float4 PSDebugBilinear(float4 p : SV_Position) : SV_Target { float a=reconstruction(p); return float4(a.xxx,1); }
float4 PSDebug(float4 p : SV_Position) : SV_Target { float a=reconstruction(p); return float4(a.xxx,1); }
