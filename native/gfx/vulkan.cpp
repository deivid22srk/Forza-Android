// Milestone 3-b Vulkan renderer: the game's fixed function draws come across.
//
// The D3D8 HLE hands every draw over as a gpu::State (render states, texture
// stages, streams, transforms) plus the primitive call. The SDK's gpu/
// translates that to HLSL per draw (shaders.cpp) and decodes the guest's
// textures (texture.cpp); this side compiles the HLSL to SPIR-V in process
// with the NDK's shaderc and renders it. The reference for the whole flow is
// the SDK's d3d12.cpp, this file mirrors it on Vulkan.
//
// Targets live as host images keyed by the guest surface address: CPU written
// content is uploaded on bind (a memory signature decides, like MemSig in
// d3d12.cpp), GPU results are copied back into guest memory before Present,
// Flush and Sync, so the milestones 1..3-a CPU picture of a frame - decode
// the front surface, gamma, blit - keeps working untouched. Depth, stencil,
// points and shadow reads stay out (milestone 3-c); a draw that needs them
// reports once and is skipped, the menus of the front end do not.
//
// The Dear ImGui FPS counter records on top in the same overlay pass. Draw
// reports itself and waits for nothing.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <dxgiformat.h>
#include <shaderc/shaderc.h>
#include <vulkan/vulkan.h>

#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "host.h"
#include "kernel/kernel.h"
#include "ui/overlay.h"
#include "vulkan.h"

namespace gpu {

namespace {

constexpr uint32_t kFramesInFlight = 2;
// the guest formats a colour target the game may draw into: the XDK's A8R8G8B8
// as packed and as linear, both 32 bit BGRA in memory
constexpr uint32_t kFmtArgb = 0x12, kFmtLinArgb = 0x1E;
constexpr uint32_t kRingBytes = 8 * 1024 * 1024;  // per frame upload arena
constexpr uint32_t kMaxDescriptorSets = 4096;     // per frame in flight

#define FORZA_VK(f)                                     \
    do {                                                \
        if (VkResult r = (f); r != VK_SUCCESS) {        \
            LOGE("gpu: %s failed with %d", #f, int(r)); \
            return false;                               \
        }                                               \
    } while (0)

// a failure inside a frame: report once per site and drop the frame, not the app
#define FORZA_VK_SOFT(f, ...)                    \
    do {                                         \
        if (VkResult r = (f); r != VK_SUCCESS) { \
            LOG_FIRST(8, __VA_ARGS__, int(r));   \
            return false;                        \
        }                                        \
    } while (0)

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t wanted, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        if ((wanted & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

// float to half, for the F16 depth clear
uint16_t Half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, mag = x & 0x7FFFFFFF;
    if (mag >= 0x47800000) return uint16_t(sign | 0x7C00);
    if (mag < 0x38800000) return uint16_t(sign);
    int e = int((mag >> 23) - 127) + 15, m = (mag >> 13) & 0x3FF;
    if (e <= 0) return uint16_t(sign | ((mag | 0x800000) >> (13 - e)));
    return uint16_t(sign | (e << 10) | m);
}

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// console D3DCOLOR ARGB to RGBA floats
void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DBLEND, the console uses GL values; alpha picks the colour or alpha factor
VkBlendFactor Blend(uint32_t v, bool alpha) {
    constexpr uint32_t kDestColor = 0x306;
    switch (v) {
        case 0:
            return VK_BLEND_FACTOR_ZERO;
        case 1:
            return VK_BLEND_FACTOR_ONE;
        case 0x300:
            return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301:
            return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case kDestColor:
            return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307:
            return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308:
            return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001:
        case 0x8003:
            return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002:
        case 0x8004:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default:
            return VK_BLEND_FACTOR_ONE;
    }
}

// D3DBLENDOP, GL values
VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A:
            return VK_BLEND_OP_SUBTRACT;
        case 0x800B:
            return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007:
            return VK_BLEND_OP_MIN;
        case 0x8008:
            return VK_BLEND_OP_MAX;
        default:
            return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, in Vulkan's order
VkCompareOp CmpOp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return VkCompareOp(n);
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3:
        case 5:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// MemSig hashes a surface up to 64 KiB whole, a bigger one as evenly spaced
// samples: the cheap way to notice the title wrote over a target's memory
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    if (!p) return 0;
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

std::string ReplaceAll(std::string s, const std::string& what, const std::string& with) {
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + with.size()))
        s.replace(p, what.size(), with);
    return s;
}

// The generated HLSL names its resources with D3D registers; fixed Vulkan
// bindings keep the descriptor set layout one shape for every shader:
// b0 vertex constants, b1 draw constants, t0..3 stage images, s0..3 samplers.
std::string BindHlslResources(std::string s) {
    s = ReplaceAll(std::move(s), "cbuffer VC : register(b0)", "[[vk::binding(0, 0)]]\ncbuffer VC : register(b0)");
    s = ReplaceAll(std::move(s), "cbuffer DC : register(b1)", "[[vk::binding(1, 0)]]\ncbuffer DC : register(b1)");
    for (uint32_t i = 0; i < 4; i++) {
        std::string n = std::to_string(i), b = std::to_string(2 + i), sm = std::to_string(6 + i);
        s = ReplaceAll(std::move(s), "tex" + n + " : register(t" + n + ")",
                       "[[vk::binding(" + b + ", 0)]] tex" + n + " : register(t" + n + ")");
        s = ReplaceAll(std::move(s), "smp" + n + " : register(s" + n + ")",
                       "[[vk::binding(" + sm + ", 0)]] smp" + n + " : register(s" + n + ")");
    }
    // glslang's HLSL front end does not take the HLSL infinity literal
    s = ReplaceAll(std::move(s), "1.#INF", "1e38");
    s = ReplaceAll(std::move(s), "1.#QNAN", "0");
    return s;
}

// the vertex formats the input assembler reads raw; the SDK's HostFormat picks
// these DXGI names per D3DVSDT type (vertex.cpp), this maps them to Vulkan.
// Keyed by the enum names, not by numbers: the SDK's harness/compat
// dxgiformat.h carries a few non-standard values (R32_FLOAT is 41 there).
VkFormat VertexVkFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:  // the converted path
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT:  // FLOAT and FLOAT2H
            return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT:
            return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32_FLOAT:
            return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32_UINT:  // NORMPACKED3
            return VK_FORMAT_R32_UINT;
        case DXGI_FORMAT_B8G8R8A8_UNORM:  // D3DCOLOR
            return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:  // PBYTE4 and PBYTE3
            return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM:  // PBYTE2
            return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_UNORM:  // PBYTE1
            return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM:  // NORMSHORT4 and NORMSHORT3
            return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT:  // SHORT4 and SHORT3
            return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R16G16_SNORM:  // NORMSHORT2
            return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT:  // SHORT2
            return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16_SNORM:  // NORMSHORT1
            return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16_SINT:  // SHORT1
            return VK_FORMAT_R16_SINT;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

// what the SDK's decoder uploads a format as, to Vulkan
struct HostVkFormat {
    VkFormat format;
    bool compressed;
};
HostVkFormat HostVk(Host h) {
    switch (h) {
        case Host::BC1:
            return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true};
        case Host::BC2:
            return {VK_FORMAT_BC2_UNORM_BLOCK, true};
        case Host::BC3:
            return {VK_FORMAT_BC3_UNORM_BLOCK, true};
        case Host::R32F:
            return {VK_FORMAT_R32_SFLOAT, false};
        case Host::R16Unorm:
            return {VK_FORMAT_R16_UNORM, false};
        default:
            return {VK_FORMAT_B8G8R8A8_UNORM, false};
    }
}

// ---- draw constants, mirrors of d3d12.cpp's Fill* -------------------------

struct GuestExtent {
    uint32_t w = 0, h = 0;
};

constexpr uint32_t kRsFogEnable = 92, kRsFogTableMode = 93, kRsFogStart = 94, kRsFogEnd = 95, kRsFogDensity = 96;
constexpr uint32_t kRsFogColor = 138, kRsAmbient = 115, kRsSpecularEnable = 103, kRsZEnable = 143, kRsCullMode = 147,
                   kRsFillMode = 139, kRsTextureFactor = 148, kRsShadowFunc = 156, kRsStencilEnable = 144;

constexpr uint32_t kMAlphaTestEnable = 0x300, kMAlphaFunc = 0x33C, kMAlphaRef = 0x340, kMSrcBlend = 0x344,
                   kMDestBlend = 0x348, kMBlendColor = 0x34C, kMBlendOp = 0x350, kMColorWriteEnable = 0x358,
                   kMPsConstant0 = 0xA60, kMPsConstant1 = 0xA80, kMPsFinalConstant0 = 0x1E20,
                   kMPsFinalConstant1 = 0x1E24;

constexpr uint32_t kTssAddressU = 0, kTssAddressV = 1, kTssMagFilter = 3, kTssMinFilter = 4, kTssMipFilter = 5,
                   kTssMipLodBias = 6, kTssMaxMipLevel = 7, kTssMaxAnisotropy = 8, kTssBumpEnvMat00 = 21,
                   kTssBumpEnvMat01 = 22, kTssBumpEnvMat11 = 23, kTssBumpEnvMat10 = 24, kTssBumpEnvLScale = 25,
                   kTssBumpEnvLOffset = 26;

constexpr float kAlphaTestAlways = 7.0f;  // Cmp(ALWAYS) - 1, the shader's always-compare code

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    // the alpha test compares in the shader: misc[1] is the comparison, 0 never to 7 always, in Vulkan's order
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpOp(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpOp(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void FillTextureConstants(const State& s, bool rt_offset, const bool rt_stage[4], const TextureDesc* desc,
                          DrawConstants& c) {
    // host targets are guest sized in this milestone: the console's half pixel
    // screen space offset moves a target read half a host pixel back
    for (int i = 0; i < 4; i++) {
        if (desc[i].data) {
            c.texsize[i][0] = float(desc[i].width);
            c.texsize[i][1] = float(desc[i].height);
        }
        if (rt_stage[i]) c.rtshift[i] = rt_offset ? -0.5f : 0.0f;
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] =
                x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] + x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

// the vertex shader cache key, salts of d3d12.cpp: the declaration, the decode,
// the pretransformed split and the fixed function light/texture state
constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kQuadSalt = 0xC3C30000;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, bool prog, const FixedKey& fk) {
    uint64_t seed;
    if (prog) {
        if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
        seed = s.vs->hash ^ a.regs;
    } else {
        seed = Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    }
    uint64_t k = Hash(&a.decode, 8, seed);
    if (!prog && a.pretransformed) k ^= kPretransformedSalt;
    if (!prog) k = Hash(&fk, sizeof(fk), k);
    return k;
}

// a colour target the game draws into, as a host image; guest memory catches
// up through ReadbackTarget (before Present, Flush, Sync and Copy)
struct Target {
    uint32_t data = 0;  // the guest surface's data address, the map key
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer pass_fb = VK_NULL_HANDLE;            // the draw render pass's target
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // where the image sits between recordings
    // the sampled copy a draw reads when the title binds a target as texture
    VkImage fb = VK_NULL_HANDLE;
    VkDeviceMemory fb_mem = VK_NULL_HANDLE;
    VkImageView fb_view = VK_NULL_HANDLE;
    VkImageLayout fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // GPU results land here on the way back to guest memory
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_mem = VK_NULL_HANDLE;
    uint8_t* readback_map = nullptr;
    uint32_t w = 0, h = 0;
    bool gpu_drawn = false;  // GPU results the guest memory does not have yet
    bool uploaded = false;   // the host image matches the guest memory's content
    uint64_t sig = 0;        // the guest memory signature at the last upload
};

// a decoded guest texture
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, levels = 1;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    bool compressed = false;
    uint64_t epoch = ~0ull;  // the guest's DirtyEpoch the upload came from
};

// a per frame upload arena: vertices, indices, constant buffers, texture and
// target staging all come out of it, reset once its command buffer finished
struct Ring {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceSize size = 0, used = 0;
};

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;

    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
              uint32_t dst_points_va) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;

private:
    // swapchain side, milestone 3-a
    void DestroySwapchain();
    bool CreateSurfaceAndSwapchain();
    bool EnsureGuestBuffers(uint32_t w, uint32_t h, uint32_t flight);
    void ApplyGamma(std::vector<uint8_t>& px) const;
    bool SubmitFrame(const std::vector<uint8_t>& px, uint32_t flight);

    // draw side, milestone 3-b
    bool BeginDrawCmd();
    bool OpenPass(Target* t);
    void ClosePass();
    bool SubmitDrawsAndReadback();
    bool ReadbackTargets(std::vector<Target*>& list);
    Ring& NewRing(uint32_t flight, VkDeviceSize bytes);
    bool RingAlloc(uint32_t flight, VkDeviceSize bytes, VkDeviceSize align, Ring& ring, VkDeviceSize& offset);
    Target* EnsureTarget(uint32_t data, uint32_t w, uint32_t h);
    void DropTarget(uint32_t data);
    bool UploadTargetImage(Target& t, const TextureDesc& d);
    bool CopyTargetToFb(Target& t);
    Texture* EnsureTexture(uint32_t data, const TextureDesc& d, const FormatInfo& fi, const uint32_t* palette);
    bool UploadTexture(Texture& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* palette);
    VkSampler EnsureSampler(const State& s, int stage);
    VkShaderModule EnsureShader(uint64_t key, const std::string& hlsl, shaderc_shader_kind kind);
    bool EnsureShaders(const State& s, const Assembled& a, uint64_t vkey, uint64_t pkey, const PixelKey& pk,
                       VkShaderModule& vs, VkShaderModule& ps);
    VkPipeline EnsurePipeline(const State& s, const Assembled& a, VkShaderModule vs, VkShaderModule ps);
    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                      VkAccessFlags dst_access);

    bool ok_ = false, recreate_next_ = false;
    bool ui_ready_ = false;  // the overlay backend survives swapchain rebuilds
    uint32_t guest_w_ = 0, guest_h_ = 0;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    VkFormat swap_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{1920, 1080};
    VkRenderPass overlay_pass_ = VK_NULL_HANDLE;  // survives swapchain rebuilds, the format does not change
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> rendered_;  // one per swapchain image
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds_;  // one per swapchain image
    VkBuffer staging_[kFramesInFlight] = {};
    VkDeviceMemory staging_mem_[kFramesInFlight] = {};
    uint8_t* staging_map_[kFramesInFlight] = {};
    VkDeviceSize staging_size_[kFramesInFlight] = {};
    VkImage frame_[kFramesInFlight] = {};
    VkDeviceMemory frame_mem_[kFramesInFlight] = {};
    uint32_t frame_w_[kFramesInFlight] = {}, frame_h_[kFramesInFlight] = {};
    VkSemaphore acquired_[kFramesInFlight] = {};
    VkFence fences_[kFramesInFlight] = {};
    uint32_t flight_ = 0;
    uint32_t epoch_ = 0;
    uint8_t gamma_[3 * 256] = {};
    bool gamma_identity_ = true;

    // draws
    std::unordered_map<uint32_t, Target> targets_;    // by guest surface data address
    std::unordered_map<uint32_t, Texture> textures_;  // by guest texture data address
    std::vector<Ring> rings_[kFramesInFlight];
    VkCommandBuffer draw_cmd_[kFramesInFlight] = {};
    VkFence draw_fence_[kFramesInFlight] = {};
    bool draw_open_[kFramesInFlight] = {};
    VkFence readback_fence_ = VK_NULL_HANDLE;
    VkRenderPass draw_pass_ = VK_NULL_HANDLE;  // targets are BGRA8 by the format guard
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_[kFramesInFlight] = {};
    VkPipelineCache pipe_cache_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    std::unordered_map<uint64_t, VkShaderModule> shaders_;  // pixel keys come in with the salt applied
    std::unordered_map<uint64_t, VkSampler> samplers_;
    VkImage null_image_ = VK_NULL_HANDLE;  // 1x1 white for stages without a texture
    VkDeviceMemory null_mem_ = VK_NULL_HANDLE;
    VkImageView null_view_ = VK_NULL_HANDLE;
    Target* pass_target_ = nullptr;  // the render pass open in the current draw command
    shaderc_compiler_t compiler_ = nullptr;
};

VulkanBackend::~VulkanBackend() {
    if (!device_) {
        if (instance_) {
            if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            vkDestroyInstance(instance_, nullptr);
        }
        if (compiler_) shaderc_compiler_release(compiler_);
        return;
    }
    vkDeviceWaitIdle(device_);
    DestroySwapchain();
    if (overlay_pass_) vkDestroyRenderPass(device_, overlay_pass_, nullptr);
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        if (staging_[i]) vkDestroyBuffer(device_, staging_[i], nullptr);
        if (staging_mem_[i]) vkFreeMemory(device_, staging_mem_[i], nullptr);
        if (frame_[i]) vkDestroyImage(device_, frame_[i], nullptr);
        if (frame_mem_[i]) vkFreeMemory(device_, frame_mem_[i], nullptr);
        if (desc_pool_[i]) vkDestroyDescriptorPool(device_, desc_pool_[i], nullptr);
        if (draw_fence_[i]) vkDestroyFence(device_, draw_fence_[i], nullptr);
        for (Ring& r : rings_[i]) {
            if (r.buf) vkDestroyBuffer(device_, r.buf, nullptr);
            if (r.mem) vkFreeMemory(device_, r.mem, nullptr);
        }
    }
    if (readback_fence_) vkDestroyFence(device_, readback_fence_, nullptr);
    for (VkSemaphore s : acquired_) vkDestroySemaphore(device_, s, nullptr);
    for (VkFence f : fences_) vkDestroyFence(device_, f, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    for (auto& [key, pipe] : pipelines_) vkDestroyPipeline(device_, pipe, nullptr);
    for (auto& [key, mod] : shaders_) vkDestroyShaderModule(device_, mod, nullptr);
    for (auto& [key, smp] : samplers_) vkDestroySampler(device_, smp, nullptr);
    for (auto& [data, t] : targets_) {
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.mem) vkFreeMemory(device_, t.mem, nullptr);
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.pass_fb) vkDestroyFramebuffer(device_, t.pass_fb, nullptr);
        if (t.fb) vkDestroyImage(device_, t.fb, nullptr);
        if (t.fb_mem) vkFreeMemory(device_, t.fb_mem, nullptr);
        if (t.fb_view) vkDestroyImageView(device_, t.fb_view, nullptr);
        if (t.readback) vkDestroyBuffer(device_, t.readback, nullptr);
        if (t.readback_mem) vkFreeMemory(device_, t.readback_mem, nullptr);
    }
    for (auto& [data, tx] : textures_) {
        if (tx.image) vkDestroyImage(device_, tx.image, nullptr);
        if (tx.mem) vkFreeMemory(device_, tx.mem, nullptr);
        if (tx.view) vkDestroyImageView(device_, tx.view, nullptr);
    }
    if (null_view_) vkDestroyImageView(device_, null_view_, nullptr);
    if (null_image_) vkDestroyImage(device_, null_image_, nullptr);
    if (null_mem_) vkFreeMemory(device_, null_mem_, nullptr);
    if (draw_pass_) vkDestroyRenderPass(device_, draw_pass_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (pipe_layout_) vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
    if (pipe_cache_) vkDestroyPipelineCache(device_, pipe_cache_, nullptr);
    if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
    if (compiler_) shaderc_compiler_release(compiler_);
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    SDL_Window* window = (SDL_Window*)host::Window();
    if (!window) {
        LOGE("gpu: Vulkan needs the SDL window before the game boots");
        return false;
    }
    compiler_ = shaderc_compiler_initialize();
    uint32_t n = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!exts || !n) {
        LOGE("gpu: no Vulkan instance extensions from SDL");
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Forza Motorsport";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &app;
    ic.enabledExtensionCount = n;
    ic.ppEnabledExtensionNames = exts;
    FORZA_VK(vkCreateInstance(&ic, nullptr, &instance_));
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        LOGE("gpu: Vulkan surface failed: %s", SDL_GetError());
        return false;
    }
    // a device with graphics and present in one family: frames are recorded and
    // shown from the same thread
    uint32_t ndev = 0;
    if (vkEnumeratePhysicalDevices(instance_, &ndev, nullptr) != VK_SUCCESS || !ndev) {
        LOGE("gpu: no Vulkan physical device");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(ndev);
    FORZA_VK(vkEnumeratePhysicalDevices(instance_, &ndev, devs.data()));
    for (VkPhysicalDevice d : devs) {
        uint32_t nfam = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nfam, nullptr);
        std::vector<VkQueueFamilyProperties> fams(nfam);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nfam, fams.data());
        for (uint32_t i = 0; i < nfam; i++) {
            if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if (!present) continue;
            physical_ = d;
            queue_family_ = i;
            break;
        }
        if (physical_) break;
    }
    if (!physical_) {
        LOGE("gpu: no Vulkan device with graphics+present");
        return false;
    }
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    float prio = 1.0f;
    qc.queueFamilyIndex = queue_family_;
    qc.queueCount = 1;
    qc.pQueuePriorities = &prio;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    dc.enabledExtensionCount = 1;
    dc.ppEnabledExtensionNames = dev_exts;
    FORZA_VK(vkCreateDevice(physical_, &dc, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = queue_family_;
    FORZA_VK(vkCreateCommandPool(device_, &pc, nullptr, &pool_));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        FORZA_VK(vkCreateSemaphore(device_, &sci, nullptr, &acquired_[i]));
        FORZA_VK(vkCreateFence(device_, &fci, nullptr, &fences_[i]));
        FORZA_VK(vkCreateFence(device_, &fci, nullptr, &draw_fence_[i]));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool_;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        FORZA_VK(vkAllocateCommandBuffers(device_, &ca, &draw_cmd_[i]));
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDescriptorSets * 2},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxDescriptorSets * 4},
            {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxDescriptorSets * 4},
        };
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets = kMaxDescriptorSets;
        dp.poolSizeCount = 3;
        dp.pPoolSizes = sizes;
        FORZA_VK(vkCreateDescriptorPool(device_, &dp, nullptr, &desc_pool_[i]));
    }
    FORZA_VK(vkCreateFence(device_, &fci, nullptr, &readback_fence_));
    // one descriptor shape for every shader the SDK generates
    VkDescriptorSetLayoutBinding binds[10]{};
    binds[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                nullptr};
    binds[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                nullptr};
    for (uint32_t i = 0; i < 4; i++)
        binds[2 + i] = {2 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 0; i < 4; i++)
        binds[6 + i] = {6 + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 10;
    sl.pBindings = binds;
    FORZA_VK(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &set_layout_));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    FORZA_VK(vkCreatePipelineLayout(device_, &pl, nullptr, &pipe_layout_));
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    FORZA_VK(vkCreatePipelineCache(device_, &pci, nullptr, &pipe_cache_));
    if (!CreateSurfaceAndSwapchain()) return false;
    // the null texture unbound stages read: 1x1 white BGRA8
    {
        uint32_t white = 0xFFFFFFFFu;
        Ring r = NewRing(0, 4);
        if (!r.size || !r.map) {
            LOGE("gpu: staging for the null texture failed");
            return false;
        }
        std::memcpy(r.map, &white, 4);
        VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        im.imageType = VK_IMAGE_TYPE_2D;
        im.format = VK_FORMAT_R8G8B8A8_UNORM;
        im.extent = {1, 1, 1};
        im.mipLevels = 1;
        im.arrayLayers = 1;
        im.samples = VK_SAMPLE_COUNT_1_BIT;
        im.tiling = VK_IMAGE_TILING_OPTIMAL;
        im.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        im.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        FORZA_VK(vkCreateImage(device_, &im, nullptr, &null_image_));
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device_, null_image_, &mr);
        uint32_t type = FindMemoryType(physical_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            LOGE("gpu: no device memory for the null texture");
            return false;
        }
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = type;
        FORZA_VK(vkAllocateMemory(device_, &ma, nullptr, &null_mem_));
        FORZA_VK(vkBindImageMemory(device_, null_image_, null_mem_, 0));
        VkCommandBuffer cmd = draw_cmd_[0];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        FORZA_VK(vkBeginCommandBuffer(cmd, &bi));
        ImageBarrier(cmd, null_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy cpy{};
        cpy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cpy.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cmd, r.buf, null_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
        ImageBarrier(cmd, null_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        FORZA_VK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        FORZA_VK(vkQueueSubmit(queue_, 1, &si, readback_fence_));
        FORZA_VK(vkWaitForFences(device_, 1, &readback_fence_, VK_TRUE, ~0ull));
        FORZA_VK(vkResetFences(device_, 1, &readback_fence_));
        FORZA_VK(vkResetCommandBuffer(cmd, 0));
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = null_image_;
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = VK_FORMAT_R8G8B8A8_UNORM;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        FORZA_VK(vkCreateImageView(device_, &vc, nullptr, &null_view_));
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    LOGR("gpu: Vulkan ready on %s: guest %ux%u, swapchain %ux%u", props.deviceName, guest_w_, guest_h_, extent_.width,
         extent_.height);
    return true;
}

// ---- swapchain side, milestone 3-a ---------------------------------------

// The swapchain side of the renderer, rebuilt whenever the Android surface is
// replaced (background/rotation): images, views, framebuffers, the per-image
// command buffers and present semaphores, and the overlay pass the first time.
bool VulkanBackend::CreateSurfaceAndSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    FORZA_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX || !extent_.width || !extent_.height) {
        // the window is not laid out yet: take the SDL size inside the caps
        int pw = 0, ph = 0;
        host::WindowPixels(pw, ph);
        extent_.width =
            std::clamp(uint32_t(std::max(pw, 1)), caps.minImageExtent.width, std::max(caps.maxImageExtent.width, 1u));
        extent_.height =
            std::clamp(uint32_t(std::max(ph, 1)), caps.minImageExtent.height, std::max(caps.maxImageExtent.height, 1u));
    }
    extent_.width = std::clamp(extent_.width, caps.minImageExtent.width, std::max(caps.maxImageExtent.width, 1u));
    extent_.height = std::clamp(extent_.height, caps.minImageExtent.height, std::max(caps.maxImageExtent.height, 1u));
    uint32_t nf = 0;
    FORZA_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &nf, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(nf);
    FORZA_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &nf, formats.data()));
    formats.resize(nf);
    // BGRA8 is what the SDK's decoder produces; RGBA8 differs by a swap the CPU
    // would have to make per present, so prefer it only if BGRA8 is missing
    VkSurfaceFormatKHR picked = formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) picked = f;
    swap_format_ = picked.format;
    if (swap_format_ != VK_FORMAT_B8G8R8A8_UNORM && swap_format_ != VK_FORMAT_R8G8B8A8_UNORM) {
        LOGE("gpu: swapchain format %d is not an 8-bit colour format", int(swap_format_));
        return false;
    }
    uint32_t count = 3;
    if (caps.maxImageCount) count = std::clamp(count, caps.minImageCount, caps.maxImageCount);
    else
        count = std::max(count, caps.minImageCount);
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = count;
    sc.imageFormat = swap_format_;
    sc.imageColorSpace = picked.colorSpace;
    sc.imageExtent = extent_;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        LOGE("gpu: the swapchain does not take transfers, the blit cannot run");
        return false;
    }
    FORZA_VK(vkCreateSwapchainKHR(device_, &sc, nullptr, &swapchain_));
    uint32_t nimg = 0;
    FORZA_VK(vkGetSwapchainImagesKHR(device_, swapchain_, &nimg, nullptr));
    images_.resize(nimg);
    FORZA_VK(vkGetSwapchainImagesKHR(device_, swapchain_, &nimg, images_.data()));
    // the overlay pass, once: the swapchain format is stable on Android
    if (!overlay_pass_) {
        VkAttachmentDescription att{};
        att.format = swap_format_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // the blit is already in the image
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        FORZA_VK(vkCreateRenderPass(device_, &rp, nullptr, &overlay_pass_));
    }
    views_.resize(nimg);
    framebuffers_.resize(nimg);
    for (uint32_t i = 0; i < nimg; i++) {
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = images_[i];
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = swap_format_;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        FORZA_VK(vkCreateImageView(device_, &vc, nullptr, &views_[i]));
        VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fc.renderPass = overlay_pass_;
        fc.attachmentCount = 1;
        fc.pAttachments = &views_[i];
        fc.width = extent_.width;
        fc.height = extent_.height;
        fc.layers = 1;
        FORZA_VK(vkCreateFramebuffer(device_, &fc, nullptr, &framebuffers_[i]));
    }
    cmds_.resize(nimg);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool_;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = nimg;
    FORZA_VK(vkAllocateCommandBuffers(device_, &ca, cmds_.data()));
    rendered_.resize(nimg);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < nimg; i++) FORZA_VK(vkCreateSemaphore(device_, &sci, nullptr, &rendered_[i]));
    // the acquire semaphores are redone with the swapchain: a rebuild abandons
    // whatever acquire was in the air, and a signalled-but-unconsumed semaphore
    // is undefined behaviour on the next take
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        if (acquired_[i]) vkDestroySemaphore(device_, acquired_[i], nullptr);
        FORZA_VK(vkCreateSemaphore(device_, &sci, nullptr, &acquired_[i]));
    }
    if (!EnsureGuestBuffers(guest_w_, guest_h_, 0)) {
        ok_ = false;
        return false;
    }
    if (!ui_ready_) {
        forza::ui::VulkanInit ui{instance_, physical_, device_, queue_family_, queue_, overlay_pass_, nimg};
        if (forza::ui::InitVulkan(ui)) ui_ready_ = true;
        else
            LOGW("gpu: the FPS overlay is unavailable, the game carries on without it");
    }
    return true;
}

void VulkanBackend::DestroySwapchain() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(device_, f, nullptr);
    framebuffers_.clear();
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    views_.clear();
    for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);
    rendered_.clear();
    if (!cmds_.empty()) vkFreeCommandBuffers(device_, pool_, uint32_t(cmds_.size()), cmds_.data());
    cmds_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    images_.clear();
    // whatever acquire was in the air died with it; fresh semaphores come with
    // the rebuilt swapchain, a stale signalled one would poison the next take
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        if (acquired_[i]) vkDestroySemaphore(device_, acquired_[i], nullptr);
        acquired_[i] = VK_NULL_HANDLE;
    }
}

// The staging buffer and the device image the guest frame is uploaded through,
// one pair per frame in flight (submissions on one queue may overlap, so frame
// N+1 must not rewrite what frame N is still reading). Redone when the guest
// surface size changes; the wait idle first, the pair may still be in use.
bool VulkanBackend::EnsureGuestBuffers(uint32_t w, uint32_t h, uint32_t flight) {
    if (frame_[flight] && frame_w_[flight] == w && frame_h_[flight] == h) return true;
    vkDeviceWaitIdle(device_);
    if (staging_[flight]) vkDestroyBuffer(device_, staging_[flight], nullptr);
    if (staging_mem_[flight]) vkFreeMemory(device_, staging_mem_[flight], nullptr);
    if (frame_[flight]) vkDestroyImage(device_, frame_[flight], nullptr);
    if (frame_mem_[flight]) vkFreeMemory(device_, frame_mem_[flight], nullptr);
    staging_[flight] = VK_NULL_HANDLE;
    staging_mem_[flight] = VK_NULL_HANDLE;
    staging_map_[flight] = nullptr;
    frame_[flight] = VK_NULL_HANDLE;
    frame_mem_[flight] = VK_NULL_HANDLE;
    frame_w_[flight] = frame_h_[flight] = 0;
    VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = bytes;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    FORZA_VK(vkCreateBuffer(device_, &bc, nullptr, &staging_[flight]));
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, staging_[flight], &mr);
    uint32_t type = FindMemoryType(physical_, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        LOGE("gpu: no host-visible memory for the upload buffer");
        return false;
    }
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type;
    FORZA_VK(vkAllocateMemory(device_, &ma, nullptr, &staging_mem_[flight]));
    FORZA_VK(vkBindBufferMemory(device_, staging_[flight], staging_mem_[flight], 0));
    FORZA_VK(vkMapMemory(device_, staging_mem_[flight], 0, bytes, 0, (void**)&staging_map_[flight]));
    staging_size_[flight] = bytes;
    VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    im.imageType = VK_IMAGE_TYPE_2D;
    im.format = swap_format_;
    im.extent = {w, h, 1};
    im.mipLevels = 1;
    im.arrayLayers = 1;
    im.samples = VK_SAMPLE_COUNT_1_BIT;
    im.tiling = VK_IMAGE_TILING_OPTIMAL;
    im.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    im.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    FORZA_VK(vkCreateImage(device_, &im, nullptr, &frame_[flight]));
    vkGetImageMemoryRequirements(device_, frame_[flight], &mr);
    type = FindMemoryType(physical_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        LOGE("gpu: no device memory for the frame image");
        return false;
    }
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type;
    FORZA_VK(vkAllocateMemory(device_, &ma, nullptr, &frame_mem_[flight]));
    FORZA_VK(vkBindImageMemory(device_, frame_[flight], frame_mem_[flight], 0));
    frame_w_[flight] = w;
    frame_h_[flight] = h;
    return true;
}

void VulkanBackend::ApplyGamma(std::vector<uint8_t>& px) const {
    // BGRA8: B, G, R, A; the ramp is R, G, B
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        px[i] = gamma_[512 + px[i]];
        px[i + 1] = gamma_[256 + px[i + 1]];
        px[i + 2] = gamma_[px[i + 2]];
    }
}

// ---- draw side, milestone 3-b ---------------------------------------------

// where an image sits decides the stages and accesses its layout transitions move
void LayoutStages(VkImageLayout l, VkPipelineStageFlags& stage, VkAccessFlags& access) {
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            access = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            access = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            access = VK_ACCESS_SHADER_READ_BIT;
            break;
        default:
            stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            access = 0;
            break;
    }
}

void VulkanBackend::ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                                 VkPipelineStageFlags, VkAccessFlags, VkPipelineStageFlags, VkAccessFlags) {
    // the stage masks come from the layouts; the explicit parameters stay for the
    // few calls that want something special, the rest pass anything
    VkPipelineStageFlags src_stage, dst_stage;
    VkAccessFlags src_access, dst_access;
    LayoutStages(from, src_stage, src_access);
    LayoutStages(to, dst_stage, dst_access);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

Ring& VulkanBackend::NewRing(uint32_t flight, VkDeviceSize bytes) {
    static Ring broken;  // returned on failure, size 0
    Ring r{};
    r.size = std::max<VkDeviceSize>(bytes, kRingBytes);
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = r.size;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (vkCreateBuffer(device_, &bc, nullptr, &r.buf) != VK_SUCCESS) {
        LOGE("gpu: upload ring buffer failed");
        ok_ = false;
        return broken;
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, r.buf, &mr);
    uint32_t type = FindMemoryType(physical_, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type != UINT32_MAX ? type : 0;
    if (type == UINT32_MAX || vkAllocateMemory(device_, &ma, nullptr, &r.mem) != VK_SUCCESS ||
        vkBindBufferMemory(device_, r.buf, r.mem, 0) != VK_SUCCESS ||
        vkMapMemory(device_, r.mem, 0, r.size, 0, (void**)&r.map) != VK_SUCCESS) {
        LOGE("gpu: upload ring memory failed");
        ok_ = false;
        return broken;
    }
    rings_[flight].push_back(r);
    return rings_[flight].back();
}

bool VulkanBackend::RingAlloc(uint32_t flight, VkDeviceSize bytes, VkDeviceSize align, Ring& ring,
                              VkDeviceSize& offset) {
    if (!rings_[flight].empty()) {
        Ring& r = rings_[flight].back();
        VkDeviceSize at = (r.used + align - 1) & ~(align - 1);
        if (at + bytes <= r.size) {
            r.used = at + bytes;
            ring = r;
            offset = at;
            return true;
        }
    }
    Ring& r = NewRing(flight, bytes);
    if (!r.size) return false;
    r.used = bytes;
    ring = r;
    offset = 0;
    return true;
}

bool VulkanBackend::BeginDrawCmd() {
    uint32_t f = flight_;
    if (draw_open_[f]) return true;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(draw_cmd_[f], &bi) != VK_SUCCESS) {
        LOGE("gpu: draw command begin failed");
        return false;
    }
    draw_open_[f] = true;
    return true;
}

bool VulkanBackend::OpenPass(Target* t) {
    if (t && pass_target_ == t) return true;
    ClosePass();
    VkCommandBuffer cmd = draw_cmd_[flight_];
    if (!draw_pass_) {
        // one shape for every target: the format guard keeps them BGRA8
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_B8G8R8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // draws accumulate, the title clears
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        if (vkCreateRenderPass(device_, &rp, nullptr, &draw_pass_) != VK_SUCCESS) {
            LOGE("gpu: draw render pass failed");
            return false;
        }
    }
    if (!t) return true;  // the pass object only, EnsureTarget's framebuffers need it
    if (!BeginDrawCmd()) return false;
    cmd = draw_cmd_[flight_];
    if (t->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        ImageBarrier(cmd, t->image, t->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0, 0, 0);
        t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = draw_pass_;
    rb.framebuffer = t->pass_fb;
    rb.renderArea = {0, 0, t->w, t->h};
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    pass_target_ = t;
    return true;
}

void VulkanBackend::ClosePass() {
    if (!pass_target_) return;
    vkCmdEndRenderPass(draw_cmd_[flight_]);
    pass_target_ = nullptr;
}

// the frame's recorded draws to the queue, wait, then copy every target the GPU
// drew back into guest memory; Present, Flush, Sync and Copy all drain here
bool VulkanBackend::SubmitDrawsAndReadback() {
    uint32_t f = flight_;
    if (draw_open_[f]) {
        ClosePass();
        if (vkEndCommandBuffer(draw_cmd_[f]) != VK_SUCCESS) {
            LOGE("gpu: draw command end failed");
            return false;
        }
        draw_open_[f] = false;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &draw_cmd_[f];
        if (vkQueueSubmit(queue_, 1, &si, draw_fence_[f]) != VK_SUCCESS ||
            vkWaitForFences(device_, 1, &draw_fence_[f], VK_TRUE, ~0ull) != VK_SUCCESS) {
            LOGE("gpu: draw submit or wait failed");
            return false;
        }
        vkResetFences(device_, 1, &draw_fence_[f]);
        vkResetCommandBuffer(draw_cmd_[f], 0);
        vkResetDescriptorPool(device_, desc_pool_[f], 0);
        for (Ring& r : rings_[f]) r.used = 0;
    }
    std::vector<Target*> dirty;
    for (auto& [data, t] : targets_)
        if (t.gpu_drawn) dirty.push_back(&t);
    if (dirty.empty()) return true;
    return ReadbackTargets(dirty);
}

// GPU targets back to guest memory: linear 32 bit rows at the guest's 64 byte
// pitch, exactly what the milestones 1..3-a CPU paths read
bool VulkanBackend::ReadbackTargets(std::vector<Target*>& list) {
    VkCommandBuffer cmd = draw_cmd_[flight_];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        LOGE("gpu: readback begin failed");
        return false;
    }
    for (Target* t : list) {
        ImageBarrier(cmd, t->image, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, 0, 0);
        VkBufferImageCopy cpy{};
        cpy.bufferRowLength = (t->w + 15) & ~15u;  // the 64 byte guest pitch, in texels
        cpy.bufferImageHeight = t->h;
        cpy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cpy.imageExtent = {t->w, t->h, 1};
        vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->readback, 1, &cpy);
        ImageBarrier(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                     0, 0, 0);
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS || vkQueueSubmit(queue_, 1, &si, readback_fence_) != VK_SUCCESS ||
        vkWaitForFences(device_, 1, &readback_fence_, VK_TRUE, ~0ull) != VK_SUCCESS) {
        LOGE("gpu: readback submit or wait failed");
        return false;
    }
    vkResetFences(device_, 1, &readback_fence_);
    vkResetCommandBuffer(cmd, 0);
    for (Target* t : list) {
        uint32_t pitch = GuestPitch(t->w);
        auto* guest = static_cast<uint8_t*>(kernel::HostPtr(t->data));
        if (!guest) continue;
        // the CPU may have written the surface since the GPU drew (a clear or a
        // lock between the draws and this drain): its bytes win, like the
        // reference's SyncBytes, and the next bind uploads them
        uint64_t now = MemSig(t->data, pitch * t->h);
        if (now != t->sig) {
            t->gpu_drawn = false;
            t->uploaded = false;
            t->sig = 0;
            continue;
        }
        for (uint32_t y = 0; y < t->h; y++)
            std::memcpy(guest + size_t(y) * pitch, t->readback_map + size_t(y) * pitch, size_t(t->w) * 4);
        t->gpu_drawn = false;
        t->uploaded = true;
        t->sig = now;
    }
    return true;
}

Target* VulkanBackend::EnsureTarget(uint32_t data, uint32_t w, uint32_t h) {
    auto it = targets_.find(data);
    if (it != targets_.end()) {
        if (it->second.w == w && it->second.h == h) return &it->second;
        DropTarget(data);
    }
    Target t{};
    t.data = data;
    t.w = w;
    t.h = h;
    // every failure past the image must unwind what already exists (N5)
    auto fail = [&]() {
        LOGE("gpu: target creation failed");
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.mem) vkFreeMemory(device_, t.mem, nullptr);
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.pass_fb) vkDestroyFramebuffer(device_, t.pass_fb, nullptr);
        if (t.fb) vkDestroyImage(device_, t.fb, nullptr);
        if (t.fb_mem) vkFreeMemory(device_, t.fb_mem, nullptr);
        if (t.fb_view) vkDestroyImageView(device_, t.fb_view, nullptr);
        if (t.readback) vkDestroyBuffer(device_, t.readback, nullptr);
        if (t.readback_mem) vkFreeMemory(device_, t.readback_mem, nullptr);
        return (Target*)nullptr;
    };
    VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    im.imageType = VK_IMAGE_TYPE_2D;
    im.format = VK_FORMAT_B8G8R8A8_UNORM;
    im.extent = {w, h, 1};
    im.mipLevels = 1;
    im.arrayLayers = 1;
    im.samples = VK_SAMPLE_COUNT_1_BIT;
    im.tiling = VK_IMAGE_TILING_OPTIMAL;
    im.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    im.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &im, nullptr, &t.image) != VK_SUCCESS) return fail();
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, t.image, &mr);
    uint32_t type = FindMemoryType(physical_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type != UINT32_MAX ? type : 0;
    if (type == UINT32_MAX || vkAllocateMemory(device_, &ma, nullptr, &t.mem) != VK_SUCCESS ||
        vkBindImageMemory(device_, t.image, t.mem, 0) != VK_SUCCESS)
        return fail();
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t.image;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vc.format = VK_FORMAT_B8G8R8A8_UNORM;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &vc, nullptr, &t.view) != VK_SUCCESS) return fail();
    if (!draw_pass_ && !OpenPass(nullptr)) {
        // EnsureTarget runs first in Draw, so the render pass is made here
        return fail();
    }
    VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fc.renderPass = draw_pass_;
    fc.attachmentCount = 1;
    fc.pAttachments = &t.view;
    fc.width = w;
    fc.height = h;
    fc.layers = 1;
    if (vkCreateFramebuffer(device_, &fc, nullptr, &t.pass_fb) != VK_SUCCESS) return fail();
    // the sampled copy and the readback land, both guest sized
    VkImageCreateInfo fim = im;
    fim.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(device_, &fim, nullptr, &t.fb) != VK_SUCCESS) return fail();
    vkGetImageMemoryRequirements(device_, t.fb, &mr);
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = FindMemoryType(physical_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ma, nullptr, &t.fb_mem) != VK_SUCCESS ||
        vkBindImageMemory(device_, t.fb, t.fb_mem, 0) != VK_SUCCESS)
        return fail();
    VkImageViewCreateInfo fvc = vc;
    fvc.image = t.fb;
    if (vkCreateImageView(device_, &fvc, nullptr, &t.fb_view) != VK_SUCCESS) return fail();
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = VkDeviceSize(GuestPitch(w)) * h;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device_, &bc, nullptr, &t.readback) != VK_SUCCESS) return fail();
    vkGetBufferMemoryRequirements(device_, t.readback, &mr);
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = FindMemoryType(physical_, mr.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ma, nullptr, &t.readback_mem) != VK_SUCCESS ||
        vkBindBufferMemory(device_, t.readback, t.readback_mem, 0) != VK_SUCCESS ||
        vkMapMemory(device_, t.readback_mem, 0, bc.size, 0, (void**)&t.readback_map) != VK_SUCCESS)
        return fail();
    targets_[data] = t;
    return &targets_[data];
}

void VulkanBackend::DropTarget(uint32_t data) {
    auto it = targets_.find(data);
    if (it == targets_.end()) return;
    SubmitDrawsAndReadback();  // the recorded commands may still reference it
    Target& t = it->second;
    if (t.image) vkDestroyImage(device_, t.image, nullptr);
    if (t.mem) vkFreeMemory(device_, t.mem, nullptr);
    if (t.view) vkDestroyImageView(device_, t.view, nullptr);
    if (t.pass_fb) vkDestroyFramebuffer(device_, t.pass_fb, nullptr);
    if (t.fb) vkDestroyImage(device_, t.fb, nullptr);
    if (t.fb_mem) vkFreeMemory(device_, t.fb_mem, nullptr);
    if (t.fb_view) vkDestroyImageView(device_, t.fb_view, nullptr);
    if (t.readback) vkDestroyBuffer(device_, t.readback, nullptr);
    if (t.readback_mem) vkFreeMemory(device_, t.readback_mem, nullptr);
    targets_.erase(it);
}

// the title wrote the target's memory on the CPU: up to the host image it goes
bool VulkanBackend::UploadTargetImage(Target& t, const TextureDesc& d) {
    if (!BeginDrawCmd()) return false;
    ClosePass();
    VkCommandBuffer cmd = draw_cmd_[flight_];
    uint32_t pitch = GuestPitch(d.width);
    VkDeviceSize total = VkDeviceSize(pitch) * d.height;
    Ring r;
    VkDeviceSize off;
    if (!RingAlloc(flight_, total, 4, r, off)) {
        LOGE("gpu: target upload staging failed");
        return false;
    }
    auto* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    if (!guest) return false;
    for (uint32_t y = 0; y < d.height; y++)
        std::memcpy(r.map + off + size_t(y) * pitch, guest + size_t(y) * pitch, size_t(d.width) * 4);
    ImageBarrier(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0, 0, 0);
    t.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkBufferImageCopy cpy{};
    cpy.bufferRowLength = pitch / 4;
    cpy.bufferImageHeight = d.height;
    cpy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cpy.imageExtent = {d.width, d.height, 1};
    vkCmdCopyBufferToImage(cmd, r.buf, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
    ImageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0, 0,
                 0);
    t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    t.uploaded = true;
    t.sig = MemSig(d.data, uint32_t(total));
    t.gpu_drawn = false;
    return true;
}

// a draw samples a target it is about to draw into: the sampled copy takes the
// pixels as they were, the console reads the ones it is overwriting too
bool VulkanBackend::CopyTargetToFb(Target& t) {
    if (!BeginDrawCmd()) return false;
    ClosePass();
    VkCommandBuffer cmd = draw_cmd_[flight_];
    ImageBarrier(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0, 0, 0);
    ImageBarrier(cmd, t.fb, t.fb_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0, 0, 0);
    VkImageCopy cpy{};
    cpy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cpy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cpy.extent = {t.w, t.h, 1};
    vkCmdCopyImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &cpy);
    ImageBarrier(cmd, t.fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, 0, 0);
    t.fb_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ImageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0, 0,
                 0);
    t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return true;
}

Texture* VulkanBackend::EnsureTexture(uint32_t data, const TextureDesc& d, const FormatInfo& fi,
                                      const uint32_t* palette) {
    uint64_t epoch = DirtyEpoch(data);
    HostVkFormat hv = HostVk(fi.host);
    auto it = textures_.find(data);
    if (it != textures_.end() && it->second.epoch == epoch && it->second.fmt == hv.format && it->second.w == d.width &&
        it->second.h == d.height)
        return &it->second;
    SubmitDrawsAndReadback();  // the current commands may sample the old image
    if (it != textures_.end()) {
        Texture& old = it->second;
        if (old.image) vkDestroyImage(device_, old.image, nullptr);
        if (old.mem) vkFreeMemory(device_, old.mem, nullptr);
        if (old.view) vkDestroyImageView(device_, old.view, nullptr);
        textures_.erase(it);
    }
    Texture tx{};
    tx.w = d.width;
    tx.h = d.height;
    tx.levels = d.levels;
    tx.fmt = hv.format;
    tx.compressed = hv.compressed;
    tx.epoch = epoch;
    uint32_t w = d.width, h = d.height;
    if (hv.compressed && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    im.imageType = VK_IMAGE_TYPE_2D;
    im.format = hv.format;
    im.extent = {w, h, 1};
    im.mipLevels = tx.levels;
    im.arrayLayers = 1;
    im.samples = VK_SAMPLE_COUNT_1_BIT;
    im.tiling = VK_IMAGE_TILING_OPTIMAL;
    im.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    im.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &im, nullptr, &tx.image) != VK_SUCCESS) {
        LOGE("gpu: texture image failed");
        return nullptr;
    }
    textures_[data] = tx;
    Texture& stored = textures_[data];
    // every failure past this point must drop the entry, or the next draw's
    // cache hit hands a half made texture to the descriptors (A2)
    auto drop = [&]() {
        if (stored.image) vkDestroyImage(device_, stored.image, nullptr);
        if (stored.mem) vkFreeMemory(device_, stored.mem, nullptr);
        if (stored.view) vkDestroyImageView(device_, stored.view, nullptr);
        textures_.erase(data);
    };
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, stored.image, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = FindMemoryType(physical_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ma, nullptr, &stored.mem) != VK_SUCCESS ||
        vkBindImageMemory(device_, stored.image, stored.mem, 0) != VK_SUCCESS) {
        LOGE("gpu: texture memory failed");
        drop();
        return nullptr;
    }
    if (!UploadTexture(stored, d, fi, palette)) {
        drop();
        return nullptr;
    }
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = stored.image;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vc.format = hv.format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tx.levels, 0, 1};
    if (vkCreateImageView(device_, &vc, nullptr, &stored.view) != VK_SUCCESS) {
        LOGE("gpu: texture view failed");
        drop();
        return nullptr;
    }
    return &stored;
}

// every level decoded by the SDK goes up through the frame's staging ring
bool VulkanBackend::UploadTexture(Texture& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* palette) {
    if (!BeginDrawCmd()) return false;
    VkCommandBuffer cmd = draw_cmd_[flight_];
    ImageBarrier(cmd, tx.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0, 0, 0);
    uint32_t w = tx.w, h = tx.h;
    if (tx.compressed && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
    }
    for (uint32_t l = 0; l < tx.levels; l++) {
        std::vector<uint8_t> px;
        uint32_t row_pitch = 0;
        // DecodeLevel walks the level and face offsets itself
        DecodeLevel(d, l, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), palette, px, row_pitch);
        if (px.empty()) {
            LOGE("gpu: texture level %u decode failed", l);
            return false;
        }
        Ring r;
        VkDeviceSize off;
        if (!RingAlloc(flight_, VkDeviceSize(px.size()), 4, r, off)) {
            LOGE("gpu: texture staging failed");
            return false;
        }
        std::memcpy(r.map + off, px.data(), px.size());
        VkBufferImageCopy cpy{};
        cpy.bufferOffset = off;
        cpy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        cpy.imageExtent = {std::max(1u, w >> l), std::max(1u, h >> l), 1};
        vkCmdCopyBufferToImage(cmd, r.buf, tx.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
    }
    ImageBarrier(cmd, tx.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, 0,
                 0);
    return true;
}

VkSampler VulkanBackend::EnsureSampler(const State& s, int stage) {
    const auto& t = s.tss[size_t(stage)];
    struct SamKey {
        uint32_t v[9];
    } key{t[kTssAddressU],   t[kTssAddressV],    t[kTssMagFilter], t[kTssMinFilter],  t[kTssMipFilter],
          t[kTssMipLodBias], t[kTssMaxMipLevel], t[kTssAddressW],  t[kTssBorderColor]};
    uint64_t h = Hash(&key, sizeof(key));
    auto it = samplers_.find(h);
    if (it != samplers_.end()) return it->second;
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // D3DTEXF: 1 point, 2 linear, 3 anisotropic; anisotropy stays off in 3-b
    sc.magFilter = t[kTssMagFilter] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.minFilter = t[kTssMinFilter] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.mipmapMode = t[kTssMipFilter] == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = Address(t[kTssAddressU]);
    sc.addressModeV = Address(t[kTssAddressV]);
    sc.addressModeW = Address(t[kTssAddressW]);
    sc.mipLodBias = Bits(t[kTssMipLodBias]);
    sc.minLod = float(t[kTssMaxMipLevel]);
    sc.maxLod = t[kTssMipFilter] ? VK_LOD_CLAMP_NONE : sc.minLod;
    sc.anisotropyEnable = VK_FALSE;
    sc.maxAnisotropy = 1;
    // Vulkan only knows the fixed border colours; the games use black or white
    float bc[4];
    Color(t[kTssBorderColor], bc);
    bool borders = t[kTssAddressU] == 4 || t[kTssAddressV] == 4 || t[kTssAddressW] == 4;
    bool white = std::all_of(bc, bc + 4, [](float c) { return c >= 0.999f; });
    bool black = std::all_of(bc, bc + 4, [](float c) { return c <= 0.001f; });
    if (borders && white) sc.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else {
        if (borders && !black) LOG_FIRST(8, "gpu: custom border colours sample black until 3-c");
        sc.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    }
    VkSampler smp = VK_NULL_HANDLE;
    if (vkCreateSampler(device_, &sc, nullptr, &smp) != VK_SUCCESS) {
        LOGE("gpu: sampler creation failed");
        return VK_NULL_HANDLE;
    }
    samplers_[h] = smp;
    return smp;
}

VkShaderModule VulkanBackend::EnsureShader(uint64_t key, const std::string& hlsl, shaderc_shader_kind kind) {
    auto it = shaders_.find(key);
    if (it != shaders_.end()) return it->second;
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_source_language(opt, shaderc_source_language_hlsl);
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    shaderc_compile_options_set_optimization_level(opt, shaderc_optimization_level_performance);
    shaderc_compilation_result_t res =
        shaderc_compile_into_spv(compiler_, hlsl.data(), hlsl.size(), kind, "forza", "main", opt);
    shaderc_compile_options_release(opt);
    bool ok = res && shaderc_result_get_compilation_status(res) == shaderc_compilation_status_success;
    if (!ok) {
        const char* err = res ? shaderc_result_get_error_message(res) : "no result";
        LOGE("gpu: shader compile failed: %.600s", err);
        if (res) shaderc_result_release(res);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mc.codeSize = shaderc_result_get_length(res) & ~size_t(3);
    mc.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(res));
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device_, &mc, nullptr, &mod);
    shaderc_result_release(res);
    if (r != VK_SUCCESS) {
        LOGE("gpu: shader module failed (%d)", int(r));
        return VK_NULL_HANDLE;
    }
    shaders_[key] = mod;
    return mod;
}

bool VulkanBackend::EnsureShaders(const State& s, const Assembled& a, uint64_t vkey, uint64_t pkey, const PixelKey& pk,
                                  VkShaderModule& vs, VkShaderModule& ps) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    std::string src = prog ? VertexProgramHlsl(s.vs->program, a.regs, a.decode)
                           : FixedVertexHlsl(a.regs, a.pretransformed, a.decode, MakeFixedKey(s));
    if (pk.quad) src = QuadHlsl(std::move(src));
    vs = EnsureShader(vkey, BindHlslResources(std::move(src)), shaderc_vertex_shader);
    if (!vs) return false;
    ps = EnsureShader(pkey ^ kPixelKeySalt, BindHlslResources(PixelHlsl(pk)), shaderc_fragment_shader);
    return ps != VK_NULL_HANDLE;
}

// everything a pipeline variant depends on; viewport, scissor and the blend
// constants are dynamic state and stay out
struct PipeKey {
    uint64_t vs, ps;
    uint32_t topo;
    uint32_t regs;
    uint32_t elements[16];
    uint32_t strides[16];
    uint32_t blend, src, dst, op, wmask, cull, fill;
};

VkPipeline VulkanBackend::EnsurePipeline(const State& s, const Assembled& a, VkShaderModule vs, VkShaderModule ps) {
    PipeKey k{};
    k.vs = uint64_t(vs);
    k.ps = uint64_t(ps);
    k.topo = a.topology == Topology::Lines ? (a.strip ? 1 : 2) : (a.strip ? 3 : 4);
    k.regs = a.regs;
    for (uint32_t r = 0; r < 16; r++)
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[r];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    for (uint32_t i = 0; i < a.buffer_count && i < 16; i++) k.strides[i] = a.buffers[i].stride;
    bool ben = SimpleState(s, kMBlendEnable) != 0;
    k.blend = ben;
    k.src = ben ? SimpleState(s, kMSrcBlend) : 1;
    k.dst = ben ? SimpleState(s, kMDestBlend) : 0;
    k.op = ben ? SimpleState(s, kMBlendOp) : 1;
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
    k.cull = s.rs[kRsCullMode];
    k.fill = s.rs[kRsFillMode];
    uint64_t h = Hash(&k, sizeof(k));
    auto it = pipelines_.find(h);
    if (it != pipelines_.end()) return it->second;
    VkVertexInputAttributeDescription attr[16];
    uint32_t na = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        VkFormat f = VertexVkFormat(k.elements[r] & 0xFF);
        if (f == VK_FORMAT_UNDEFINED) {
            LOG_FIRST(16, "gpu: vertex element format %u waits for 3-c, draw skipped", k.elements[r] & 0xFF);
            return VK_NULL_HANDLE;
        }
        attr[na++] = {r, k.elements[r] >> 8 & 0xFF, f, k.elements[r] >> 16};
    }
    VkVertexInputBindingDescription bind[16];
    uint32_t nb = 0;
    for (uint32_t i = 0; i < a.buffer_count && i < 16; i++)
        bind[nb++] = {i, a.buffers[i].stride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = bind;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k.topo == 0   ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : k.topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                  : k.topo == 2 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                  : k.topo == 3 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    bool triangles = a.topology == Topology::Triangles;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == 0x1B01 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull && triangles ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.cull == 0x900 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1;
    // depth and stencil stay out until 3-c
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = k.blend ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = Blend(k.src, false);
    cb.dstColorBlendFactor = Blend(k.dst, false);
    cb.colorBlendOp = BlendOp(k.op);
    cb.srcAlphaBlendFactor = Blend(k.src, true);
    cb.dstAlphaBlendFactor = Blend(k.dst, true);
    cb.alphaBlendOp = BlendOp(k.op);
    cb.colorWriteMask = k.wmask & 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dyni{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyni.dynamicStateCount = 3;
    dyni.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                               VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", nullptr}};
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dyni;
    gp.layout = pipe_layout_;
    gp.renderPass = draw_pass_;
    gp.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, pipe_cache_, 1, &gp, nullptr, &pipe) != VK_SUCCESS) {
        LOGE("gpu: pipeline creation failed");
        return VK_NULL_HANDLE;
    }
    pipelines_[h] = pipe;
    return pipe;
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    if (!ok_) return;
    if (flags & kClearTarget) {
        gpu::TextureDesc d;
        if (!DescribeSurface(s.rt, d)) return;
        // the D3DCOLOR word only lands byte-correct in the A8R8G8B8 linear
        // formats; anything else waits for the renderer of 3-b
        if (d.format != kFmtArgb && d.format != kFmtLinArgb) {
            LOG_FIRST(4, "gpu: clear of a %ux%u format %u target is pending 3-c", d.width, d.height, d.format);
            return;
        }
        uint32_t* base = (uint32_t*)kernel::HostPtr(d.data);
        uint32_t stride = d.pitch / 4;
        if (count && rects) {
            for (uint32_t r = 0; r < count; r++) {
                // D3DRECT: x1, y1, x2, y2, x2/y2 exclusive
                const int32_t* q = (const int32_t*)(rects + size_t(r) * 16);
                uint32_t ax = uint32_t(std::clamp(q[0], 0, int32_t(d.width)));
                uint32_t ay = uint32_t(std::clamp(q[1], 0, int32_t(d.height)));
                uint32_t bx = uint32_t(std::clamp(q[2], 0, int32_t(d.width)));
                uint32_t by = uint32_t(std::clamp(q[3], 0, int32_t(d.height)));
                for (uint32_t y = ay; y < by; y++)
                    for (uint32_t x = ax; x < bx; x++) base[size_t(y) * stride + x] = color;
            }
        } else {
            for (uint32_t y = 0; y < d.height; y++)
                for (uint32_t x = 0; x < d.width; x++) base[size_t(y) * stride + x] = color;
        }
        // the host image is stale now, the next bind uploads the cleared memory
        auto it = targets_.find(d.data);
        if (it != targets_.end()) {
            it->second.uploaded = false;
            it->second.sig = 0;
        }
    }
    if ((flags & (kClearZ | kClearStencil)) && s.zs.header) {
        gpu::TextureDesc d;
        if (!DescribeSurface(s.zs, d) || !d.linear) return;
        uint8_t* base = (uint8_t*)kernel::HostPtr(d.data);
        uint32_t f = d.format;
        if (f == 0x30) {  // LIN_D16
            uint16_t v = uint16_t(z * 65535.0f + 0.5f);
            for (uint32_t y = 0; y < d.height; y++) {
                uint16_t* row = (uint16_t*)(base + size_t(y) * d.pitch);
                for (uint32_t x = 0; x < d.width; x++) row[x] = v;
            }
        } else if (f == 0x31) {  // LIN_F16
            uint16_t v = Half(z);
            for (uint32_t y = 0; y < d.height; y++) {
                uint16_t* row = (uint16_t*)(base + size_t(y) * d.pitch);
                for (uint32_t x = 0; x < d.width; x++) row[x] = v;
            }
        } else {  // 24-bit depth in 32-bit words, stencil in the top byte
            uint32_t z24 = uint32_t(z * 16777215.0f + 0.5f) & 0xFFFFFF;
            bool with_stencil = flags & kClearStencil;
            for (uint32_t y = 0; y < d.height; y++) {
                uint32_t* row = (uint32_t*)(base + size_t(y) * d.pitch);
                for (uint32_t x = 0; x < d.width; x++)
                    row[x] = (with_stencil ? stencil << 24 : row[x] & 0xFF000000u) | z24;
            }
        }
    }
}

// The milestone 3-b draw: assemble, textures, shaders, pipeline, constants,
// record. Skipped features report once and drop the draw, so the field log
// names exactly what 3-c still needs.
void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (!ok_) return;
    if (!s.rt.header) return;
    gpu::TextureDesc td;
    if (!DescribeSurface(s.rt, td)) return;
    if (td.format != kFmtArgb && td.format != kFmtLinArgb) {
        LOG_FIRST(16, "gpu: draws into a format %u target wait for 3-c", td.format);
        return;
    }
    if (!BeginDrawCmd()) {
        ok_ = false;
        return;
    }
    if (s.rs[kRsZEnable] || s.rs[kRsStencilEnable])
        LOG_FIRST(64, "gpu: depth/stencil draws are 3-c, drawing without depth");
    Assembled a;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    if (a.topology == Topology::Points) {
        LOG_FIRST(16, "gpu: point draws are 3-c, draw skipped");
        return;
    }
    Target* rt = EnsureTarget(td.data, td.width, td.height);
    if (!rt) {
        ok_ = false;
        return;
    }
    // the stages: which textures, which are targets themselves, what needs recording
    TexInfo infos[4]{};
    TextureDesc tds[4]{};
    bool rt_stage[4]{};
    uint64_t sig = MemSig(td.data, GuestPitch(td.width) * td.height);
    bool stale = !rt->uploaded || sig != rt->sig;
    bool setup = stale;
    for (int i = 0; i < 4; i++) {
        if (!s.textures[i]) continue;
        TextureDesc d;
        if (!Describe(s.textures[i], d)) continue;
        const FormatInfo& fi = Format(d.format);
        if (d.cube || d.depth > 1 || fi.depth) {
            LOG_FIRST(16, "gpu: cube/3D/depth stage textures are 3-c, draw skipped");
            return;
        }
        tds[i] = d;
        infos[i].kind = kTex2D;
        infos[i].rect = d.linear;
        if (targets_.count(d.data) && !fi.compressed) {
            infos[i].flags |= kTexTarget;
            rt_stage[i] = true;
            setup = true;
        } else {
            auto tx = textures_.find(d.data);
            if (tx == textures_.end() || tx->second.epoch != DirtyEpoch(d.data)) setup = true;
        }
    }
    // everything recorded before the render pass, so the image barriers stay legal
    if (setup) ClosePass();
    if (stale) {
        if (!UploadTargetImage(*rt, td)) {
            ok_ = false;
            return;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (rt_stage[i]) {
            if (!CopyTargetToFb(targets_.at(tds[i].data))) {
                ok_ = false;
                return;
            }
        } else if (tds[i].data) {
            const FormatInfo& fi = Format(tds[i].format);
            const uint32_t* pal = s.palettes[i] ? (const uint32_t*)kernel::HostPtr(s.palettes[i]) : nullptr;
            if (!EnsureTexture(tds[i].data, tds[i], fi, pal)) {
                LOG_FIRST(8, "gpu: stage texture %08X upload failed, draw skipped", tds[i].data);
                return;
            }
        }
    }
    if (!OpenPass(rt)) {
        ok_ = false;
        return;
    }
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    FixedKey fk;
    if (!prog) fk = MakeFixedKey(s);
    // a quad without depth test may be a pass over the whole target: the
    // far edge variant of the shaders, as d3d12.cpp's QuadHlsl choice
    bool quad = count <= 6 && !s.rs[kRsZEnable];
    uint64_t vkey = VertexKey(s, a, prog, fk) ^ (quad ? kQuadSalt : 0);
    PixelKey pk = MakePixelKey(s, infos);
    pk.quad = quad;
    uint64_t pkey = Hash(&pk, sizeof(pk));
    VkShaderModule vs, ps;
    if (!EnsureShaders(s, a, vkey, pkey, pk, vs, ps)) return;
    VkPipeline pipe = EnsurePipeline(s, a, vs, ps);
    if (!pipe) return;
    VkCommandBuffer cmd = draw_cmd_[flight_];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    // constants: the vertex shader's c[192] and the shared draw constants
    Ring r;
    VkDeviceSize off;
    if (!RingAlloc(flight_, sizeof(s.vsc), 256, r, off)) {
        ok_ = false;
        return;
    }
    std::memcpy(r.map + off, s.vsc.data(), sizeof(s.vsc));
    VkDescriptorBufferInfo vcbi{r.buf, off, sizeof(s.vsc)};
    DrawConstants c{};
    GuestExtent size{td.width, td.height};
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, prog, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, !prog || ReadsViewportOffset(s.vs->program), rt_stage, tds, c);
    if (!prog && !a.pretransformed) FillTransformConstants(s, size, c);
    if (!prog && (fk.light & 1)) FillLightConstants(s, fk, c);
    if (!RingAlloc(flight_, sizeof(DrawConstants), 256, r, off)) {
        ok_ = false;
        return;
    }
    std::memcpy(r.map + off, &c, sizeof(c));
    VkDescriptorBufferInfo dcbi{r.buf, off, sizeof(DrawConstants)};
    // vertices and indices through the ring
    VkBuffer vbuf[16] = {};
    VkDeviceSize vboff[16] = {};
    if (a.buffer_count > 16) {
        LOG_FIRST(16, "gpu: draw with %u vertex buffers is 3-c territory, skipped", a.buffer_count);
        return;
    }
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        if (!b.src || !b.bytes || !b.stride) {
            LOG_FIRST(16, "gpu: draw with an empty vertex buffer, skipped");
            return;
        }
        if (!RingAlloc(flight_, b.bytes, 16, r, vboff[i])) {
            ok_ = false;
            return;
        }
        std::memcpy(r.map + vboff[i], b.src, b.bytes);
        vbuf[i] = r.buf;
    }
    VkBuffer ibuf = VK_NULL_HANDLE;
    VkDeviceSize iboff = 0;
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        if (!RingAlloc(flight_, ibytes, 4, r, iboff)) {
            ok_ = false;
            return;
        }
        std::memcpy(r.map + iboff, a.index_src, ibytes);
        ibuf = r.buf;
    }
    // one descriptor set per draw out of the frame's pool
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = desc_pool_[flight_];
    da.descriptorSetCount = 1;
    da.pSetLayouts = &set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &da, &set) != VK_SUCCESS) {
        LOG_FIRST(8, "gpu: descriptor set allocation failed");
        return;
    }
    VkDescriptorImageInfo img[4]{};
    for (int i = 0; i < 4; i++) {
        VkImageView v = null_view_;
        if (rt_stage[i]) v = targets_.at(tds[i].data).fb_view;
        else if (tds[i].data) {
            auto tx = textures_.find(tds[i].data);
            if (tx != textures_.end()) v = tx->second.view;
        }
        img[i] = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkDescriptorImageInfo smps[4]{};
    for (int i = 0; i < 4; i++) smps[i] = {EnsureSampler(s, i), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    for (int i = 0; i < 4; i++)
        if (!smps[i].sampler) {
            // a null sampler in a descriptor is invalid use; the draw waits for 3-c
            LOG_FIRST(8, "gpu: stage %d sampler failed, draw skipped", i);
            return;
        }
    VkWriteDescriptorSet w[10]{};
    for (int i = 0; i < 10; i++) w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = w[1].dstSet = set;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &vcbi;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &dcbi;
    for (int i = 0; i < 4; i++) {
        w[2 + i].dstSet = set;
        w[2 + i].dstBinding = uint32_t(2 + i);
        w[2 + i].descriptorCount = 1;
        w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[2 + i].pImageInfo = &img[i];
        w[6 + i].dstSet = set;
        w[6 + i].dstBinding = uint32_t(6 + i);
        w[6 + i].descriptorCount = 1;
        w[6 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[6 + i].pImageInfo = &smps[i];
    }
    vkUpdateDescriptorSets(device_, 10, w, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1, &set, 0, nullptr);
    // the whole target is the viewport, the guest viewport becomes the scissor
    VkViewport vp{0, 0, float(td.width), float(td.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    int32_t x0 = std::clamp<int32_t>(int32_t(s.viewport.x), 0, int32_t(td.width));
    int32_t y0 = std::clamp<int32_t>(int32_t(s.viewport.y), 0, int32_t(td.height));
    int32_t x1 = std::clamp<int32_t>(int32_t(s.viewport.x + s.viewport.w), x0, int32_t(td.width));
    int32_t y1 = std::clamp<int32_t>(int32_t(s.viewport.y + s.viewport.h), y0, int32_t(td.height));
    if (s.scissor_count) {
        x0 = std::max(x0, std::clamp<int32_t>(int32_t(s.scissors[0]), 0, int32_t(td.width)));
        y0 = std::max(y0, std::clamp<int32_t>(int32_t(s.scissors[1]), 0, int32_t(td.height)));
        x1 = std::min(x1, std::clamp<int32_t>(int32_t(s.scissors[2]), x0, int32_t(td.width)));
        y1 = std::min(y1, std::clamp<int32_t>(int32_t(s.scissors[3]), y0, int32_t(td.height)));
    }
    VkRect2D sc{x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0)};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(cmd, bf);
    vkCmdBindVertexBuffers(cmd, 0, a.buffer_count, vbuf, vboff);
    if (ibuf) vkCmdBindIndexBuffer(cmd, ibuf, iboff, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    if (a.index_count) vkCmdDrawIndexed(cmd, a.index_count, 1, 0, a.base_vertex, 0);
    else
        vkCmdDraw(cmd, a.draw_count, 1, 0, 0);
    rt->gpu_drawn = true;
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                         uint32_t dst_points_va) {
    if (!ok_) return;
    if (!SubmitDrawsAndReadback()) {
        ok_ = false;
        return;
    }
    if (!CopySurfaceMemory(src, dst, count, src_rects_va, dst_points_va))
        LOG_FIRST(4, "gpu: unsupported CopyRects is pending 3-c");
}

void VulkanBackend::Sync(uint32_t data, uint32_t) {
    if (!ok_) return;
    if (!SubmitDrawsAndReadback()) ok_ = false;
}

void VulkanBackend::Flush() {
    if (!ok_) return;
    if (!SubmitDrawsAndReadback()) ok_ = false;
}

void VulkanBackend::Present(const Surface& front) {
    if (!ok_ || !front.header) {
        // setup failed: behave like the null backend, the game thread must not spin
        if (!ok_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    // the Android surface is gone while the app is in the background; park the
    // frame here until it is back, then rebuild around the new surface
    if (!host::Foreground()) host::WaitForeground();
    if (host::SurfaceEpoch() != epoch_ || recreate_next_) {
        epoch_ = host::SurfaceEpoch();
        recreate_next_ = false;
        SubmitDrawsAndReadback();  // the draws of the frame land before the rebuild
        DestroySwapchain();
        if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface((SDL_Window*)host::Window(), instance_, nullptr, &surface_)) {
            LOGE("gpu: Vulkan surface re-creation failed: %s", SDL_GetError());
            ok_ = false;
            return;
        }
        if (!CreateSurfaceAndSwapchain()) {
            ok_ = false;
            return;
        }
    }
    // the GPU results back into guest memory, then the front surface decodes
    // just like it did in the milestones 1..3-a
    if (!SubmitDrawsAndReadback()) {
        ok_ = false;
        return;
    }
    gpu::TextureDesc d;
    if (!DescribeSurface(front, d)) {
        LOG_FIRST(4, "gpu: present of an undecodable surface %08X", front.header);
        return;
    }
    if (!d.linear || Format(d.format).bits != 32) {
        LOG_FIRST(4, "gpu: present of %ux%u format %u is pending 3-c", d.width, d.height, d.format);
        return;
    }
    if (d.width != guest_w_ || d.height != guest_h_) {
        guest_w_ = d.width;
        guest_h_ = d.height;
    }
    uint32_t flight = flight_;
    flight_ = (flight_ + 1) % kFramesInFlight;
    if (!EnsureGuestBuffers(guest_w_, guest_h_, flight)) {
        ok_ = false;
        return;
    }
    uint32_t pitch = 0;
    std::vector<uint8_t> px;
    DecodeLevel(d, 0, 0, (const uint8_t*)kernel::HostPtr(d.data), nullptr, px, pitch);
    if (px.empty()) {
        LOG_FIRST(4, "gpu: present decode failed for %08X", front.header);
        return;
    }
    if (!gamma_identity_) ApplyGamma(px);
    // the decoder makes BGRA8; a swapchain that came up as RGBA8 needs the red
    // and blue swapped before the upload
    if (swap_format_ == VK_FORMAT_R8G8B8A8_UNORM)
        for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    if (SubmitFrame(px, flight)) forza::ui::PresentTick();
}

// One frame: upload the decoded guest surface, blit it aspect-fitted onto the
// swapchain image, draw the overlay pass on top, present. True when the frame
// was actually submitted (the FPS counter only counts shown frames).
bool VulkanBackend::SubmitFrame(const std::vector<uint8_t>& px, uint32_t flight) {
    VkFence fence = fences_[flight];
    FORZA_VK_SOFT(vkWaitForFences(device_, 1, &fence, VK_TRUE, ~0ull), "gpu: fence wait failed (%d)");
    uint32_t idx = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, ~0ull, acquired_[flight], VK_NULL_HANDLE, &idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_ERROR_SURFACE_LOST_KHR) {
        recreate_next_ = true;
        return false;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOG_FIRST(8, "gpu: acquire failed (%d)", int(acq));
        return false;
    }
    recreate_next_ |= acq == VK_SUBOPTIMAL_KHR;  // the image is still good, show it and rebuild later
    if (idx >= cmds_.size()) return false;
    VkCommandBuffer cmd = cmds_[idx];
    FORZA_VK_SOFT(vkResetFences(device_, 1, &fence), "gpu: fence reset failed (%d)");
    FORZA_VK_SOFT(vkResetCommandBuffer(cmd, 0), "gpu: command reset failed (%d)");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        LOG_FIRST(8, "gpu: command buffer begin failed");
        return false;
    }
    // the upload: the decoded guest frame straight into the mapped staging buffer
    VkDeviceSize bytes = VkDeviceSize(guest_w_) * guest_h_ * 4;
    if (staging_size_[flight] >= bytes && px.size())
        std::memcpy(staging_map_[flight], px.data(), std::min(VkDeviceSize(px.size()), bytes));
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.subresourceRange = all;
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageMemoryBarrier swap_bar = to_transfer;
    swap_bar.image = images_[idx];
    VkImageMemoryBarrier frame_bar = to_transfer;
    frame_bar.image = frame_[flight];
    VkImageMemoryBarrier bars[] = {frame_bar, swap_bar};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, bars);
    VkBufferImageCopy cpy{};
    cpy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cpy.imageExtent = {guest_w_, guest_h_, 1};
    vkCmdCopyBufferToImage(cmd, staging_[flight], frame_[flight], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
    VkImageMemoryBarrier to_src = frame_bar;
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_src);
    // the letterbox: the guest aspect kept inside the screen, black around it
    VkClearColorValue black{};
    vkCmdClearColorImage(cmd, images_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    float scale = std::min(extent_.width / float(guest_w_), extent_.height / float(guest_h_));
    uint32_t dw = std::min(uint32_t(guest_w_ * scale + 0.5f), extent_.width);
    uint32_t dh = std::min(uint32_t(guest_h_ * scale + 0.5f), extent_.height);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {int32_t(guest_w_), int32_t(guest_h_), 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[0] = {int32_t((extent_.width - dw) / 2), int32_t((extent_.height - dh) / 2), 0};
    bl.dstOffsets[1] = {int32_t((extent_.width - dw) / 2 + dw), int32_t((extent_.height - dh) / 2 + dh), 1};
    vkCmdBlitImage(cmd, frame_[flight], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[idx],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    VkImageMemoryBarrier to_color = swap_bar;
    to_color.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_color);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = overlay_pass_;
    rb.framebuffer = framebuffers_[idx];
    rb.renderArea = {0, 0, extent_.width, extent_.height};
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    forza::ui::Record(cmd, float(extent_.width), float(extent_.height));
    vkCmdEndRenderPass(cmd);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG_FIRST(8, "gpu: command buffer end failed");
        return false;
    }
    // the first touch of the taken image is the transfer below, so that is the
    // stage the acquire semaphore waits in
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_[flight];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[idx];
    FORZA_VK_SOFT(vkQueueSubmit(queue_, 1, &si, fence), "gpu: submit failed (%d)");
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &idx;
    VkResult shown = vkQueuePresentKHR(queue_, &pi);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_ERROR_SURFACE_LOST_KHR || shown == VK_SUBOPTIMAL_KHR)
        recreate_next_ = true;
    else if (shown != VK_SUCCESS)
        LOG_FIRST(8, "gpu: present failed (%d)", int(shown));
    return true;
}

void VulkanBackend::SetGamma(const uint8_t* ramp) {
    if (!ramp) return;
    std::memcpy(gamma_, ramp, sizeof(gamma_));
    gamma_identity_ = true;
    for (int i = 0; i < 256; i++)
        if (gamma_[i] != i || gamma_[256 + i] != i || gamma_[512 + i] != i) {
            gamma_identity_ = false;
            break;
        }
}

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
