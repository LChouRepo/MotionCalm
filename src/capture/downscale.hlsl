// downscale.hlsl — 截屏降采样着色器（构建时由 scripts\build_shaders.cmd 用 fxc 编译为 vs_4_0 / ps_4_0 字节码头文件）：全屏三角形 VS；区域平均灰度 PS。cbuffer 布局必须与 gpu_downscale.cpp 的 DownscaleConstants 一致。
// 桌面纹理 -> 每格区域平均的灰度 0.299R + 0.587G + 0.114B（capture-motion-detection design D4）。
Texture2D<float4> g_src : register(t0);
SamplerState g_linear : register(s0);

cbuffer Params : register(b0) {
    float2 g_src_size;   // 源纹理像素尺寸
    float2 g_cell;       // 每格覆盖的源像素
    float2 g_step;       // 样本间距（源像素）= cell / taps
    int2   g_taps;       // 每格每轴的双线性样本数
};

// 全屏三角形：id 0,1,2 -> (-1,1), (3,1), (-1,-3)，无顶点缓冲。
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float PSMain(float4 pos : SV_Position) : SV_Target {
    float2 origin = floor(pos.xy) * g_cell;
    float acc = 0.0;
    [loop] for (int j = 0; j < g_taps.y; ++j) {
        float y = origin.y + (float(j) + 0.5) * g_step.y;
        [loop] for (int i = 0; i < g_taps.x; ++i) {
            float2 p = float2(origin.x + (float(i) + 0.5) * g_step.x, y);
            acc += dot(g_src.SampleLevel(g_linear, p / g_src_size, 0).rgb, float3(0.299, 0.587, 0.114));
        }
    }
    return acc / float(g_taps.x * g_taps.y);
}
