#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct Y
{
    float y[1];
};

struct X
{
    float x[1];
};

struct X4
{
    float4 x4[1];
};

constant uint3 gl_WorkGroupSize [[maybe_unused]] = uint3(64u, 1u, 1u);

struct spvDescriptorSetBuffer0
{
    device Y* m_20 [[id(0)]];
    device X* m_27 [[id(1)]];
    device X4* m_36 [[id(2)]];
};

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    uint i = gl_GlobalInvocationID.x;
    (*spvDescriptorSet0.m_20).y[i] = (*spvDescriptorSet0.m_27).x[i] + ((device float*)&(*spvDescriptorSet0.m_36).x4[i >> 2u])[i & 3u];
}

