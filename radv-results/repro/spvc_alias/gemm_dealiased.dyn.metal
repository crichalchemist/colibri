#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

// Implementation of signed integer mod accurate to SPIR-V specification
template<typename Tx, typename Ty>
inline Tx spvSMod(Tx x, Ty y)
{
    Tx remainder = x - y * (x / y);
    return select(Tx(remainder + y), remainder, remainder == 0 || (x >= 0) == (y >= 0));
}

struct P
{
    int fmt;
    int S;
    int I;
    int O;
    int rowWords;
    int gs;
};

struct W2
{
    uint2 w2[1];
};

struct W
{
    uint w[1];
};

struct Sc
{
    float scale[1];
};

struct W4
{
    uint4 w4[1];
};

struct X4
{
    float4 x4[1];
};

#ifndef SPIRV_CROSS_CONSTANT_ID_0
#define SPIRV_CROSS_CONSTANT_ID_0 64
#endif
constant int BM = SPIRV_CROSS_CONSTANT_ID_0;
#ifndef SPIRV_CROSS_CONSTANT_ID_2
#define SPIRV_CROSS_CONSTANT_ID_2 32
#endif
constant int BK = SPIRV_CROSS_CONSTANT_ID_2;
constant int _985 = (BK / 8);
constant int _986 = (BM * _985);
#ifndef SPIRV_CROSS_CONSTANT_ID_3
#define SPIRV_CROSS_CONSTANT_ID_3 4
#endif
constant int TM = SPIRV_CROSS_CONSTANT_ID_3;
constant int NTM = (BM / TM);
#ifndef SPIRV_CROSS_CONSTANT_ID_1
#define SPIRV_CROSS_CONSTANT_ID_1 64
#endif
constant int BN = SPIRV_CROSS_CONSTANT_ID_1;
#ifndef SPIRV_CROSS_CONSTANT_ID_4
#define SPIRV_CROSS_CONSTANT_ID_4 4
#endif
constant int TN = SPIRV_CROSS_CONSTANT_ID_4;
constant int NTN = (BN / TN);
constant int NT = (NTM * NTN);
constant int _993 = (_986 + NT);
constant int _994 = (_993 - 1);
constant int WU = (_994 / NT);
constant int _1004 = (BK / 8);
constant int _1009 = (BK / 8);
constant int _1029 = (BK / 8);
constant int _1030 = (BM * _1029);
constant int _1078 = (BK / 4);
constant int _1079 = (BN * _1078);
constant int _1080 = (_1079 + NT);
constant int _1081 = (_1080 - 1);
constant int XU = (_1081 / NT);
constant int _1091 = (BK / 4);
constant int _1100 = (BK / 4);
constant int _1107 = (BK / 4);
constant int _1108 = (BN * _1107);
constant bool PF_tmp [[function_constant(6)]];
constant bool PF = is_function_constant_defined(PF_tmp) ? PF_tmp : true;
constant bool _1270 = (!PF);
constant int _1296 = (BK / 8);
constant int _1297 = (BM * _1296);
constant int _1303 = (BK / 8);
constant int _1307 = (BK / 8);
constant int _1329 = (BK / 4);
constant int KQ = (_1329 + 1);
constant int _1331 = (BM * KQ);
constant int _1365 = (BK / 4);
constant int _1366 = (BN * _1365);
constant int _1370 = (BN * KQ);
constant int _1375 = (BK / 4);
constant int _1379 = (BK / 4);
constant int _1440 = (BK / 4);

struct Y
{
    float y[1];
};

constant uint _1668_tmp [[function_constant(5)]];
constant uint _1668 = is_function_constant_defined(_1668_tmp) ? _1668_tmp : 1u;
constant uint3 gl_WorkGroupSize [[maybe_unused]] = uint3(_1668, 1u, 1u);

struct spvDescriptorSetBuffer0
{
    device W2* m_245 [[id(0)]];
    device W* m_264 [[id(1)]];
    device Sc* m_322 [[id(2)]];
    device W4* m_368 [[id(3)]];
    device X4* m_967 [[id(4)]];
    device Y* m_1647 [[id(5)]];
};

constant spvUnsafeArray<float, 8> _116 = spvUnsafeArray<float, 8>({ 0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0 });

static inline __attribute__((always_inline))
void fetch8(thread const int& o, thread const int& k, thread uint4& lo, thread uint4& hi, thread float2& sc, constant P& p, device W2& _245, device W& _264, device Sc& _322, device W4& _368)
{
    uint rb = uint(o) * uint(p.rowWords);
    lo = uint4(0u);
    hi = uint4(0u);
    sc = float2(0.0);
    bool _216 = p.fmt == 1;
    bool _224;
    if (!_216)
    {
        _224 = p.fmt == 12;
    }
    else
    {
        _224 = _216;
    }
    bool _232;
    if (!_224)
    {
        _232 = p.fmt == 13;
    }
    else
    {
        _232 = _224;
    }
    if (_232)
    {
        if ((p.rowWords & 1) == 0)
        {
            uint _251 = (rb >> uint(1)) + uint(k >> 3);
            lo.x = _245.w2[_251].x;
            lo.y = _245.w2[_251].y;
        }
        else
        {
            lo.x = _264.w[rb + uint(k >> 2)];
            if ((k + 4) < p.I)
            {
                lo.y = _264.w[(rb + uint(k >> 2)) + 1u];
            }
        }
        if ((k + 4) >= p.I)
        {
            lo.y = 0u;
        }
        if (p.fmt != 1)
        {
            uint sb = uint(o) * uint(((p.I + p.gs) - 1) / p.gs);
            sc.x = _322.scale[sb + uint(k / p.gs)];
            if ((k + 4) < p.I)
            {
                sc.y = _322.scale[sb + uint((k + 4) / p.gs)];
            }
        }
    }
    else
    {
        if (p.fmt == 10)
        {
            if ((p.I & 7) == 0)
            {
                lo = _368.w4[(rb + uint(k)) >> uint(2)];
                hi = _368.w4[((rb + uint(k)) >> uint(2)) + 1u];
            }
            else
            {
                for (int j = 0; j < 4; j++)
                {
                    if ((k + j) < p.I)
                    {
                        lo[j] = _264.w[rb + uint(k + j)];
                    }
                    if (((k + 4) + j) < p.I)
                    {
                        hi[j] = _264.w[rb + uint((k + 4) + j)];
                    }
                }
            }
        }
        else
        {
            if (p.fmt == 11)
            {
                if ((p.I & 7) == 0)
                {
                    lo = _368.w4[(rb + uint(k >> 1)) >> uint(2)];
                }
                else
                {
                    for (int j_1 = 0; j_1 < 4; j_1++)
                    {
                        if ((k + (2 * j_1)) < p.I)
                        {
                            lo[j_1] = _264.w[rb + uint((k >> 1) + j_1)];
                        }
                    }
                }
            }
            else
            {
                if (p.fmt == 5)
                {
                    int g = k >> 6;
                    int j_2 = k & 63;
                    lo.x = _264.w[rb + uint((g * 6) + (j_2 >> 4))] >> uint((j_2 & 15) * 2);
                    lo.y = _264.w[rb + uint(((g * 6) + 4) + (j_2 >> 5))] >> uint(j_2 & 31);
                    sc.x = _322.scale[(uint(o) * uint((p.I + 63) >> 6)) + uint(g)];
                }
                else
                {
                    lo.x = _264.w[rb + uint(k >> 3)];
                    if (p.fmt != 2)
                    {
                        sc.x = _322.scale[(uint(o) * uint(((p.I + p.gs) - 1) / p.gs)) + uint(k / p.gs)];
                    }
                }
            }
        }
    }
}

static inline __attribute__((always_inline))
float x_at(thread const uint& i, device X4& _967)
{
    return ((device float*)&_967.x4[i >> uint(2)])[i & 3u];
}

static inline __attribute__((always_inline))
void fetch_step(thread const int& tid, thread const int& o0, thread const int& s0, thread const int& k0, constant P& p, device W2& _245, device W& _264, device Sc& _322, device W4& _368, device X4& _967, thread spvUnsafeArray<uint4, WU>& rwa, thread spvUnsafeArray<uint4, WU>& rwb, thread spvUnsafeArray<float2, WU>& rws, thread spvUnsafeArray<float4, XU>& rx)
{
    uint4 param_2;
    uint4 param_3;
    float2 param_4;
    for (int r = 0; r < WU; r++)
    {
        int u = tid + (r * NT);
        int m = u / _1004;
        int k = k0 + (spvSMod(u, _1009) * 8);
        rwa[r] = uint4(0u);
        rwb[r] = uint4(0u);
        rws[r] = float2(0.0);
        bool _1031 = u < _1030;
        bool _1040;
        if (_1031)
        {
            _1040 = (o0 + m) < p.O;
        }
        else
        {
            _1040 = _1031;
        }
        bool _1047;
        if (_1040)
        {
            _1047 = k < p.I;
        }
        else
        {
            _1047 = _1040;
        }
        if (_1047)
        {
            int param = o0 + m;
            int param_1 = k;
            fetch8(param, param_1, param_2, param_3, param_4, p, _245, _264, _322, _368);
            rwa[r] = param_2;
            rwb[r] = param_3;
            rws[r] = param_4;
        }
    }
    for (int r_1 = 0; r_1 < XU; r_1++)
    {
        int u_1 = tid + (r_1 * NT);
        int n = u_1 / _1091;
        int s = s0 + n;
        int k_1 = k0 + (spvSMod(u_1, _1100) * 4);
        float4 v = float4(0.0);
        bool _1109 = u_1 < _1108;
        bool _1116;
        if (_1109)
        {
            _1116 = s < p.S;
        }
        else
        {
            _1116 = _1109;
        }
        bool _1123;
        if (_1116)
        {
            _1123 = k_1 < p.I;
        }
        else
        {
            _1123 = _1116;
        }
        if (_1123)
        {
            uint b = (uint(s) * uint(p.I)) + uint(k_1);
            if ((p.I & 3) == 0)
            {
                v = _967.x4[b >> uint(2)];
            }
            else
            {
                uint param_5 = b;
                v.x = x_at(param_5, _967);
                if ((k_1 + 1) < p.I)
                {
                    uint param_6 = b + 1u;
                    v.y = x_at(param_6, _967);
                }
                if ((k_1 + 2) < p.I)
                {
                    uint param_7 = b + 2u;
                    v.z = x_at(param_7, _967);
                }
                if ((k_1 + 3) < p.I)
                {
                    uint param_8 = b + 3u;
                    v.w = x_at(param_8, _967);
                }
            }
        }
        rx[r_1] = v;
    }
}

static inline __attribute__((always_inline))
float i8(thread const uint& word, thread const int& lane)
{
    int b = int((word >> (uint(lane) * 8u)) & 255u);
    if (b >= 128)
    {
        b -= 256;
    }
    return float(b);
}

static inline __attribute__((always_inline))
float e4m3(thread const uint& word, thread const int& lane)
{
    uint b = (word >> (uint(lane) * 8u)) & 255u;
    if ((b & 127u) == 127u)
    {
        return as_type<float>(2143289344u);
    }
    uint e = (b >> uint(3)) & 15u;
    uint m = b & 7u;
    float _165;
    if (e == 0u)
    {
        _165 = float(m) * 0.001953125;
    }
    else
    {
        _165 = ldexp(1.0 + (float(m) * 0.125), int(e) - 7);
    }
    float v = _165;
    float _188;
    if ((b & 128u) != 0u)
    {
        _188 = -v;
    }
    else
    {
        _188 = v;
    }
    return _188;
}

static inline __attribute__((always_inline))
float mx4(thread const uint& word, thread const int& lane)
{
    uint n = (word >> (uint(lane) * 4u)) & 15u;
    float v = _116[n & 7u];
    float _128;
    if ((n & 8u) != 0u)
    {
        _128 = -v;
    }
    else
    {
        _128 = v;
    }
    return _128;
}

static inline __attribute__((always_inline))
float i4(thread const uint& word, thread const int& lane)
{
    return float(int((word >> (uint(lane) * 4u)) & 15u) - 8);
}

static inline __attribute__((always_inline))
void decode8(thread const uint4& a, thread const uint4& b, thread const float2& sc, thread float4& lo, thread float4& hi, constant P& p)
{
    if (p.fmt == 1)
    {
        uint param = a.x;
        int param_1 = 0;
        uint param_2 = a.x;
        int param_3 = 1;
        uint param_4 = a.x;
        int param_5 = 2;
        uint param_6 = a.x;
        int param_7 = 3;
        lo = float4(i8(param, param_1), i8(param_2, param_3), i8(param_4, param_5), i8(param_6, param_7));
        uint param_8 = a.y;
        int param_9 = 0;
        uint param_10 = a.y;
        int param_11 = 1;
        uint param_12 = a.y;
        int param_13 = 2;
        uint param_14 = a.y;
        int param_15 = 3;
        hi = float4(i8(param_8, param_9), i8(param_10, param_11), i8(param_12, param_13), i8(param_14, param_15));
    }
    else
    {
        if (p.fmt == 12)
        {
            uint param_16 = a.x;
            int param_17 = 0;
            uint param_18 = a.x;
            int param_19 = 1;
            uint param_20 = a.x;
            int param_21 = 2;
            uint param_22 = a.x;
            int param_23 = 3;
            lo = float4(e4m3(param_16, param_17), e4m3(param_18, param_19), e4m3(param_20, param_21), e4m3(param_22, param_23)) * sc.x;
            uint param_24 = a.y;
            int param_25 = 0;
            uint param_26 = a.y;
            int param_27 = 1;
            uint param_28 = a.y;
            int param_29 = 2;
            uint param_30 = a.y;
            int param_31 = 3;
            hi = float4(e4m3(param_24, param_25), e4m3(param_26, param_27), e4m3(param_28, param_29), e4m3(param_30, param_31)) * sc.y;
        }
        else
        {
            if (p.fmt == 13)
            {
                uint param_32 = a.x;
                int param_33 = 0;
                uint param_34 = a.x;
                int param_35 = 1;
                uint param_36 = a.x;
                int param_37 = 2;
                uint param_38 = a.x;
                int param_39 = 3;
                lo = float4(i8(param_32, param_33), i8(param_34, param_35), i8(param_36, param_37), i8(param_38, param_39)) * sc.x;
                uint param_40 = a.y;
                int param_41 = 0;
                uint param_42 = a.y;
                int param_43 = 1;
                uint param_44 = a.y;
                int param_45 = 2;
                uint param_46 = a.y;
                int param_47 = 3;
                hi = float4(i8(param_40, param_41), i8(param_42, param_43), i8(param_44, param_45), i8(param_46, param_47)) * sc.y;
            }
            else
            {
                if (p.fmt == 10)
                {
                    lo = as_type<float4>(a);
                    hi = as_type<float4>(b);
                }
                else
                {
                    if (p.fmt == 11)
                    {
                        lo = float4(as_type<float>(a.x << uint(16)), as_type<float>(a.x & 4294901760u), as_type<float>(a.y << uint(16)), as_type<float>(a.y & 4294901760u));
                        hi = float4(as_type<float>(a.z << uint(16)), as_type<float>(a.z & 4294901760u), as_type<float>(a.w << uint(16)), as_type<float>(a.w & 4294901760u));
                    }
                    else
                    {
                        if (p.fmt == 5)
                        {
                            spvUnsafeArray<float, 8> v;
                            for (int t = 0; t < 8; t++)
                            {
                                v[t] = float(int(((a.x >> uint(2 * t)) & 3u) | (((a.y >> uint(t)) & 1u) << uint(2))) - 4);
                            }
                            lo = float4(v[0], v[1], v[2], v[3]) * sc.x;
                            hi = float4(v[4], v[5], v[6], v[7]) * sc.x;
                        }
                        else
                        {
                            if (p.fmt == 7)
                            {
                                uint param_48 = a.x;
                                int param_49 = 0;
                                uint param_50 = a.x;
                                int param_51 = 1;
                                uint param_52 = a.x;
                                int param_53 = 2;
                                uint param_54 = a.x;
                                int param_55 = 3;
                                lo = float4(mx4(param_48, param_49), mx4(param_50, param_51), mx4(param_52, param_53), mx4(param_54, param_55)) * sc.x;
                                uint param_56 = a.x;
                                int param_57 = 4;
                                uint param_58 = a.x;
                                int param_59 = 5;
                                uint param_60 = a.x;
                                int param_61 = 6;
                                uint param_62 = a.x;
                                int param_63 = 7;
                                hi = float4(mx4(param_56, param_57), mx4(param_58, param_59), mx4(param_60, param_61), mx4(param_62, param_63)) * sc.x;
                            }
                            else
                            {
                                uint param_64 = a.x;
                                int param_65 = 0;
                                uint param_66 = a.x;
                                int param_67 = 1;
                                uint param_68 = a.x;
                                int param_69 = 2;
                                uint param_70 = a.x;
                                int param_71 = 3;
                                lo = float4(i4(param_64, param_65), i4(param_66, param_67), i4(param_68, param_69), i4(param_70, param_71));
                                uint param_72 = a.x;
                                int param_73 = 4;
                                uint param_74 = a.x;
                                int param_75 = 5;
                                uint param_76 = a.x;
                                int param_77 = 6;
                                uint param_78 = a.x;
                                int param_79 = 7;
                                hi = float4(i4(param_72, param_73), i4(param_74, param_75), i4(param_76, param_77), i4(param_78, param_79));
                                if (p.fmt == 4)
                                {
                                    lo *= sc.x;
                                    hi *= sc.x;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], constant uint* spvDynamicOffsets [[buffer(23)]], constant P& p [[buffer(1)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]])
{
    device auto& _967 = *(device X4* )((device char* )spvDescriptorSet0.m_967 + spvDynamicOffsets[0]);
    device auto& _1647 = *(device Y* )((device char* )spvDescriptorSet0.m_1647 + spvDynamicOffsets[1]);
    threadgroup spvUnsafeArray<float4, _1331> wsh;
    threadgroup spvUnsafeArray<float4, _1370> xsh;
    int tid = int(gl_LocalInvocationID.x);
    int tm = spvSMod(tid, NTM);
    int tn = tid / NTM;
    int o0 = int(gl_WorkGroupID.x) * BM;
    int s0 = int(gl_WorkGroupID.y) * BN;
    spvUnsafeArray<uint4, WU> rwa;
    spvUnsafeArray<uint4, WU> rwb;
    spvUnsafeArray<float2, WU> rws;
    spvUnsafeArray<float4, XU> rx;
    spvUnsafeArray<spvUnsafeArray<float, TN>, TM> acc;
    for (int i = 0; i < TM; i++)
    {
        for (int j = 0; j < TN; j++)
        {
            acc[i][j] = 0.0;
        }
    }
    if (PF)
    {
        int param = tid;
        int param_1 = o0;
        int param_2 = s0;
        int param_3 = 0;
        fetch_step(param, param_1, param_2, param_3, p, (*spvDescriptorSet0.m_245), (*spvDescriptorSet0.m_264), (*spvDescriptorSet0.m_322), (*spvDescriptorSet0.m_368), _967, rwa, rwb, rws, rx);
    }
    float4 param_11;
    float4 param_12;
    spvUnsafeArray<spvUnsafeArray<float, TN>, TM> part;
    spvUnsafeArray<float4, TM> a;
    spvUnsafeArray<float4, TN> b;
    for (int k0 = 0; k0 < p.I; k0 += BK)
    {
        if (_1270)
        {
            int param_4 = tid;
            int param_5 = o0;
            int param_6 = s0;
            int param_7 = k0;
            fetch_step(param_4, param_5, param_6, param_7, p, (*spvDescriptorSet0.m_245), (*spvDescriptorSet0.m_264), (*spvDescriptorSet0.m_322), (*spvDescriptorSet0.m_368), _967, rwa, rwb, rws, rx);
        }
        for (int r = 0; r < WU; r++)
        {
            int u = tid + (r * NT);
            if (u < _1297)
            {
                int m = u / _1303;
                int kq = spvSMod(u, _1307) * 2;
                uint4 param_8 = rwa[r];
                uint4 param_9 = rwb[r];
                float2 param_10 = rws[r];
                decode8(param_8, param_9, param_10, param_11, param_12, p);
                float4 lo = param_11;
                float4 hi = param_12;
                wsh[(m * KQ) + kq] = lo;
                wsh[((m * KQ) + kq) + 1] = hi;
            }
        }
        for (int r_1 = 0; r_1 < XU; r_1++)
        {
            int u_1 = tid + (r_1 * NT);
            if (u_1 < _1366)
            {
                xsh[((u_1 / _1375) * KQ) + spvSMod(u_1, _1379)] = rx[r_1];
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        bool _1396;
        if (PF)
        {
            _1396 = (k0 + BK) < p.I;
        }
        else
        {
            _1396 = PF;
        }
        if (_1396)
        {
            int param_13 = tid;
            int param_14 = o0;
            int param_15 = s0;
            int param_16 = k0 + BK;
            fetch_step(param_13, param_14, param_15, param_16, p, (*spvDescriptorSet0.m_245), (*spvDescriptorSet0.m_264), (*spvDescriptorSet0.m_322), (*spvDescriptorSet0.m_368), _967, rwa, rwb, rws, rx);
        }
        for (int i_1 = 0; i_1 < TM; i_1++)
        {
            for (int j_1 = 0; j_1 < TN; j_1++)
            {
                part[i_1][j_1] = 0.0;
            }
        }
        for (int q = 0; q < _1440; q++)
        {
            for (int i_2 = 0; i_2 < TM; i_2++)
            {
                a[i_2] = wsh[((tm + (i_2 * NTM)) * KQ) + q];
            }
            for (int j_2 = 0; j_2 < TN; j_2++)
            {
                b[j_2] = xsh[((tn + (j_2 * NTN)) * KQ) + q];
            }
            for (int i_3 = 0; i_3 < TM; i_3++)
            {
                for (int j_3 = 0; j_3 < TN; j_3++)
                {
                    part[i_3][j_3] = fma(a[i_3].w, b[j_3].w, fma(a[i_3].z, b[j_3].z, fma(a[i_3].y, b[j_3].y, fma(a[i_3].x, b[j_3].x, part[i_3][j_3]))));
                }
            }
        }
        for (int i_4 = 0; i_4 < TM; i_4++)
        {
            for (int j_4 = 0; j_4 < TN; j_4++)
            {
                acc[i_4][j_4] += part[i_4][j_4];
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    bool _1583 = p.fmt == 1;
    bool _1590;
    if (!_1583)
    {
        _1590 = p.fmt == 2;
    }
    else
    {
        _1590 = _1583;
    }
    bool perrow = _1590;
    float _1615;
    for (int i_5 = 0; i_5 < TM; i_5++)
    {
        int o = (o0 + tm) + (i_5 * NTM);
        if (o >= p.O)
        {
            continue;
        }
        if (perrow)
        {
            _1615 = (*spvDescriptorSet0.m_322).scale[o];
        }
        else
        {
            _1615 = 1.0;
        }
        float sc = _1615;
        for (int j_5 = 0; j_5 < TN; j_5++)
        {
            int s = (s0 + tn) + (j_5 * NTN);
            if (s < p.S)
            {
                _1647.y[(uint(s) * uint(p.O)) + uint(o)] = acc[i_5][j_5] * sc;
            }
        }
    }
}

