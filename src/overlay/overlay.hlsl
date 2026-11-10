// overlay.hlsl — overlay 着色器（构建时由 scripts\build_shaders.cmd 用 fxc 编译为 vs_4_0 / ps_4_0 字节码头文件）：全屏三角形 VS；遮罩 + 8 个边缘点 + 中心点描边与中心点的 PS，输出预乘 alpha。cbuffer 布局必须与 overlay/renderer.h 的 OverlayConstants 一致。
cbuffer OverlayConstants : register(b0)
{
    float4 g_size_radii;   // W, H（像素）, rx, ry（满强度清晰区半径，像素）
    float4 g_mask;         // R（屏幕角的归一化距离）, feather, intensity i, max_opacity
    float4 g_mask_rgb;     // 遮罩颜色 rgb（0..1）, w 未用
    float4 g_center;       // 中心点半径 px, 描边宽 px, 中心点开关, 描边开关
    float4 g_center_rgba;  // 中心点颜色 rgb + alpha
    float4 g_edge;         // 边缘点半径 px, 内缩 px, 边缘点开关, alpha
    float4 g_edge_rgb;     // 边缘点颜色 rgb, w 未用
};

struct VSOut
{
    float4 pos : SV_Position;
};

// 全屏三角形：id 0,1,2 → (-1,1) (3,1) (-1,-3)，不需要顶点缓冲。
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2(float((id << 1) & 2), float(id & 2));
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

// spec：smoothstep(e0, e1, x) = t*t*(3 - 2*t)，t = clamp((x - e0) / (e1 - e0), 0, 1)
float smooth_step01(float e0, float e1, float x)
{
    float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

float4 over(float4 src, float4 dst)
{
    return src + dst * (1.0 - src.a);
}

float4 PSMain(VSOut input) : SV_Target
{
    float2 size = g_size_radii.xy;
    float2 pix = input.pos.xy;          // 像素中心（已在 .5 处）
    float2 p = pix - size * 0.5;
    float4 col = float4(0.0, 0.0, 0.0, 0.0);

    // 遮罩：a = max_opacity * i * smoothstep(r_in, r_in + feather, d)，r_in = lerp(R + feather, 1, i)
    float i = g_mask.z;
    if (i > 0.0)
    {
        float d = length(p / g_size_radii.zw);
        float feather = g_mask.y;
        float r_in = lerp(g_mask.x + feather, 1.0, i);
        float a = g_mask.w * i * smooth_step01(r_in, r_in + feather, d);
        col = float4(g_mask_rgb.rgb * a, a);
    }

    // 8 个边缘点：四角 + 四边中点，各向内缩 margin；1 px 抗锯齿。
    // 点列 x ∈ {m, W/2, W − m}、点行 y ∈ {m, H/2, H − m}（去掉正中心）。离像素最近的点只可能是：
    // 本侧的角点、本侧边列的中点、本侧边行的中点——3 次距离测试代替 8 次；离所有点列或点行都超过
    // r + 0.5 px 的像素（几乎整个屏幕）直接跳过。
    float reach = g_edge.x + 0.5;
    if (g_edge.z > 0.5)
    {
        float m = g_edge.y;
        float2 half_size = size * 0.5;
        float2 side = float2(pix.x < half_size.x ? m : size.x - m, pix.y < half_size.y ? m : size.y - m);
        float2 d_side = abs(pix - side);        // 到本侧边列 / 边行
        float2 d_mid = abs(pix - half_size);    // 到中列 / 中行
        if (min(d_side.x, d_mid.x) < reach && min(d_side.y, d_mid.y) < reach)
        {
            float dmin = length(d_side);                                  // 角点
            dmin = min(dmin, length(float2(d_side.x, d_mid.y)));          // 左 / 右边的中点
            dmin = min(dmin, length(float2(d_mid.x, d_side.y)));          // 上 / 下边的中点
            float ea = g_edge.w * saturate(reach - dmin);
            col = over(float4(g_edge_rgb.rgb * ea, ea), col);
        }
    }

    // 中心点：先画 [r, r + outline] 的黑色描边环（alpha 0.6），再画实心点（离中心超过 r + outline + 0.5 px 的像素跳过）
    if (g_center.z > 0.5 && max(abs(p.x), abs(p.y)) < g_center.x + g_center.y + 0.5)
    {
        float dc = length(p);
        float r = g_center.x;
        float inner = saturate(r + 0.5 - dc);
        if (g_center.w > 0.5)
        {
            float ring = saturate(r + g_center.y + 0.5 - dc) - inner;
            float ra = 0.6 * ring;
            col = over(float4(0.0, 0.0, 0.0, ra), col);
        }
        float ca = g_center_rgba.a * inner;
        col = over(float4(g_center_rgba.rgb * ca, ca), col);
    }
    return col;
}
