/*
 * php-vio - portable upscaler (UPSCALE-PLAN.md, OPEN-ITEMS A21)
 *
 * Fragment passes over the public draw API, so every backend with a 3D
 * pipeline runs them (GL 3.3 without compute too). Every pass addresses its
 * inputs from gl_FragCoord: storage row r of the target reads storage row r of
 * the source on every backend, whichever way up the backend stores targets.
 */

#ifndef VIO_UPSCALE_SHADERS_H
#define VIO_UPSCALE_SHADERS_H

/* One triangle over the whole target. */
static const char *vio_upscale_vs =
    "#version 450\n"
    "layout(location = 0) in vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

/* Edge-adaptive Lanczos-2 over the 4x4 texels around the sample point. The
 * luma gradient of the inner 2x2 gives the edge direction; the kernel is
 * stretched along the edge and squeezed across it in proportion to the edge
 * strength (flat areas get the plain radial kernel). The result is clamped to
 * the inner 2x2 range so the negative lobes cannot ring. */
static const char *vio_upscale_spatial_fs =
    "#version 450\n"
    "layout(location = 0) out vec4 o;\n"
    "uniform sampler2D u_src;\n"
    "uniform vec4 u_size;      /* src w, h, dst w, h */\n"
    "uniform vec4 u_rect;      /* dst origin x, y (pixels) */\n"
    "float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "vec4 tap(ivec2 p) { return texelFetch(u_src, clamp(p, ivec2(0), ivec2(u_size.xy) - 1), 0); }\n"
    "float lanczos2(float x) {\n"
    "    x = min(abs(x), 2.0);\n"
    "    if (x < 1e-4) return 1.0;\n"
    "    float px = 3.14159265 * x;\n"
    "    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);\n"
    "}\n"
    "void main() {\n"
    "    vec2 dst = gl_FragCoord.xy - u_rect.xy;\n"
    "    vec2 p = dst * u_size.xy / u_size.zw - 0.5;   /* source texel space, texel centres on integers */\n"
    "    vec2 fp = floor(p);\n"
    "    vec2 f = p - fp;\n"
    "    ivec2 b = ivec2(fp);\n"
    "    vec4 c00 = tap(b), c10 = tap(b + ivec2(1, 0)), c01 = tap(b + ivec2(0, 1)), c11 = tap(b + ivec2(1, 1));\n"
    "    float l00 = luma(c00.rgb), l10 = luma(c10.rgb), l01 = luma(c01.rgb), l11 = luma(c11.rgb);\n"
    "    vec2 g = vec2((l10 - l00) + (l11 - l01), (l01 - l00) + (l11 - l10));\n"
    "    float gl = length(g);\n"
    "    vec2 across = gl > 1e-5 ? g / gl : vec2(1.0, 0.0);\n"
    "    vec2 along = vec2(-across.y, across.x);\n"
    "    float edge = clamp(gl * 2.0, 0.0, 1.0);\n"
    "    float sAlong = mix(1.0, 0.5, edge);    /* < 1: the kernel reaches further along the edge */\n"
    "    float sAcross = mix(1.0, 1.4, edge);   /* > 1: and less far across it */\n"
    "    vec4 acc = vec4(0.0);\n"
    "    float wsum = 0.0;\n"
    "    for (int y = -1; y <= 2; y++) {\n"
    "        for (int x = -1; x <= 2; x++) {\n"
    "            vec2 d = vec2(float(x), float(y)) - f;\n"
    "            vec2 r = vec2(dot(d, along) * sAlong, dot(d, across) * sAcross);\n"
    "            float w = lanczos2(length(r));\n"
    "            acc += tap(b + ivec2(x, y)) * w;\n"
    "            wsum += w;\n"
    "        }\n"
    "    }\n"
    "    vec4 c = acc / max(wsum, 1e-4);\n"
    "    vec4 mn = min(min(c00, c10), min(c01, c11));\n"
    "    vec4 mx = max(max(c00, c10), max(c01, c11));\n"
    "    o = clamp(c, mn, mx);\n"
    "}\n";

/* Contrast-adaptive sharpening at the target resolution: a negative lobe on the
 * four neighbours, as strong as the local range allows without leaving
 * [0, 1] (no clipping halos), scaled by u_sharp. */
static const char *vio_upscale_sharpen_fs =
    "#version 450\n"
    "layout(location = 0) out vec4 o;\n"
    "uniform sampler2D u_src;\n"
    "uniform vec4 u_size;      /* w, h of u_src (= the target) */\n"
    "uniform vec4 u_rect;      /* dst origin x, y, sharpness */\n"
    "vec4 tap(ivec2 p) { return texelFetch(u_src, clamp(p, ivec2(0), ivec2(u_size.xy) - 1), 0); }\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy - u_rect.xy);\n"
    "    vec4 e = tap(p);\n"
    "    vec3 n = tap(p + ivec2(0, -1)).rgb, w = tap(p + ivec2(-1, 0)).rgb;\n"
    "    vec3 s = tap(p + ivec2(0, 1)).rgb, x = tap(p + ivec2(1, 0)).rgb;\n"
    "    vec3 mn = min(min(n, w), min(s, x));\n"
    "    vec3 mx = max(max(n, w), max(s, x));\n"
    "    vec3 hitMin = mn / max(4.0 * mx, vec3(1e-4));\n"
    "    vec3 hitMax = (vec3(1.0) - mx) / min(4.0 * mn - 4.0, vec3(-1e-4));\n"
    "    vec3 l3 = max(-hitMin, hitMax);\n"
    "    float lobe = clamp(max(l3.r, max(l3.g, l3.b)), -0.1875, 0.0) * u_rect.z;\n"
    "    vec3 c = (lobe * (n + w + s + x) + e.rgb) / (4.0 * lobe + 1.0);\n"
    "    o = vec4(max(c, vec3(0.0)), u_rect.w > 0.5 ? 1.0 : e.a);   /* HDR stays above 1; w: opaque (temporal history) */\n"
    "}\n";

/* Temporal accumulation at the target resolution. The source was rendered with
 * a sub-pixel jitter (texel i holds the scene at i + 0.5 + jitter); every target
 * pixel takes the nearest jittered sample with a weight from its distance and
 * blends it into the history, which is reprojected with the motion vector and
 * clamped to the YCoCg range of the 3x3 source neighbourhood (no ghosting).
 * Off-screen history or u_ctl.x = 1 (reset) starts from the plain bilinear
 * upsample. Motion: UV now minus UV in the previous frame (y up, i.e. half the
 * NDC delta); u_misc turns it into storage UV for backends whose row 0 is NDC
 * top. u_ctl.zw is the sample offset in storage space (the CPU flips the
 * caller's NDC jitter the same way). */
static const char *vio_upscale_temporal_fs =
    "#version 450\n"
    "layout(location = 0) out vec4 o;\n"
    "uniform sampler2D u_src;\n"
    "uniform sampler2D u_hist;\n"
    "uniform sampler2D u_motion;\n"
    "uniform vec4 u_size;      /* src w, h, dst w, h */\n"
    "uniform vec4 u_ctl;       /* reset, has motion, sample offset x, y (source pixels, storage space) */\n"
    "uniform vec4 u_misc;      /* motion scale x, y: UV with y up -> storage UV */\n"
    "vec4 tap(ivec2 p) { return texelFetch(u_src, clamp(p, ivec2(0), ivec2(u_size.xy) - 1), 0); }\n"
    "vec3 ycocg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }\n"
    "vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }\n"
    "vec4 bilinear(vec2 p) {\n"
    "    vec2 fp = floor(p); vec2 f = p - fp; ivec2 b = ivec2(fp);\n"
    "    return mix(mix(tap(b), tap(b + ivec2(1, 0)), f.x), mix(tap(b + ivec2(0, 1)), tap(b + ivec2(1, 1)), f.x), f.y);\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / u_size.zw;\n"
    "    vec2 p = uv * u_size.xy - 0.5 - u_ctl.zw;   /* jittered sample index space */\n"
    "    ivec2 i = ivec2(floor(p + 0.5));\n"
    "    /* this frame's samples around the pixel, each weighted by its distance\n"
    "     * (in target pixels) to the pixel centre: their weighted sum joins the mean */\n"
    "    vec3 mn = vec3(1e9), mx = vec3(-1e9), cs = vec3(0.0);\n"
    "    float ws = 0.0;\n"
    "    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
    "        vec3 c = tap(i + ivec2(x, y)).rgb;\n"
    "        vec2 d = (p - vec2(i + ivec2(x, y))) * u_size.zw / u_size.xy;\n"
    "        float w = exp(-5.0 * dot(d, d));          /* 1 at the sample, 0.29 half a target pixel away */\n"
    "        cs += c * w; ws += w;\n"
    "        vec3 yc = ycocg(c);\n"
    "        mn = min(mn, yc); mx = max(mx, yc);\n"
    "    }\n"
    "    vec2 m = u_ctl.y > 0.5 ? texelFetch(u_motion, clamp(i, ivec2(0), ivec2(u_size.xy) - 1), 0).rg * u_misc.xy : vec2(0.0);\n"
    "    vec2 prev = uv - m;\n"
    "    bool valid = u_ctl.x < 0.5 && all(greaterThanEqual(prev, vec2(0.0))) && all(lessThanEqual(prev, vec2(1.0)));\n"
    "    /* history alpha = accumulated sample weight (a running weighted mean) */\n"
    "    if (!valid) { o = vec4(bilinear(uv * u_size.xy - 0.5 - u_ctl.zw).rgb, 0.05); return; }\n"
    "    /* Catmull-Rom over the history: bilinear would blur it a little at every\n"
    "     * sub-pixel shift, and the blur accumulates frame after frame */\n"
    "    vec2 hp = prev * u_size.zw - 0.5;\n"
    "    vec2 hf = floor(hp); vec2 t = hp - hf; ivec2 hb = ivec2(hf);\n"
    "    vec2 w0 = t * (-0.5 + t * (1.0 - 0.5 * t)), w1 = 1.0 + t * t * (-2.5 + 1.5 * t);\n"
    "    vec2 w2 = t * (0.5 + t * (2.0 - 1.5 * t)), w3 = t * t * (-0.5 + 0.5 * t);\n"
    "    vec4 hist = vec4(0.0);\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "        float wy = y == 0 ? w0.y : y == 1 ? w1.y : y == 2 ? w2.y : w3.y;\n"
    "        for (int x = 0; x < 4; x++) {\n"
    "            float wx = x == 0 ? w0.x : x == 1 ? w1.x : x == 2 ? w2.x : w3.x;\n"
    "            hist += wx * wy * texelFetch(u_hist, clamp(hb + ivec2(x - 1, y - 1), ivec2(0), ivec2(u_size.zw) - 1), 0);\n"
    "        }\n"
    "    }\n"
    "    vec3 hy = clamp(ycocg(hist.rgb), mn, mx);\n"
    "    /* older samples fade (the mean follows changes), faster while the picture\n"
    "     * moves by fractions of a target pixel: resampling costs detail */\n"
    "    vec2 mp = m * u_size.zw;\n"
    "    float sub = clamp(length(mp - floor(mp + 0.5)) * 2.0, 0.0, 1.0);\n"
    "    float W = min(max(hist.a, 0.0), mix(8.0, 2.0, sub)) * 0.9;\n"
    "    vec3 c = (rgb(hy) * W + cs) / max(W + ws, 1e-4);\n"
    "    o = vec4(max(c, vec3(0.0)), W + ws);\n"
    "}\n";

/* A plain copy (temporal output without sharpening, history -> target). */
static const char *vio_upscale_copy_fs =
    "#version 450\n"
    "layout(location = 0) out vec4 o;\n"
    "uniform sampler2D u_src;\n"
    "uniform vec4 u_size;\n"
    "uniform vec4 u_rect;\n"
    "void main() {\n"
    "    vec4 c = texelFetch(u_src, clamp(ivec2(gl_FragCoord.xy - u_rect.xy), ivec2(0), ivec2(u_size.xy) - 1), 0);\n"
    "    o = vec4(c.rgb, u_rect.w > 0.5 ? 1.0 : c.a);\n"
    "}\n";

#endif /* VIO_UPSCALE_SHADERS_H */
