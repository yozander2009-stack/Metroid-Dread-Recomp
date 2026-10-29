/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* texconv.comp's conversions, by its mode push constant: C (vk/texconv.c) and GLSL (texconv.glsl) include this */
#ifndef SNX_TEXCONV_MODES
#define SNX_TEXCONV_MODES

/* 2 byte texels to RGBA8, a pair an invocation */
#define TC_RGBA4 0u    /* R in the low 4 bits */
#define TC_RGB5A1 1u   /* R in the low 5 bits, A the top bit */
#define TC_A5B5G5R1 2u /* R the low bit, then G, B and A of 5 */
/* RGB32F to RGBA32F with A 1.0, a texel an invocation */
#define TC_RGB32F 3u
/* packed depth and stencil to a depth aspect of 4 bytes a texel and a stencil aspect of 1, four texels an
   invocation: the depth in 24 bits (as they are, or as a float when the push constant zfloat says so) */
#define TC_D24S8 4u /* depth in the low 24 bits, stencil the top 8 */
#define TC_S8D24 5u /* stencil in the low 8 bits, depth the top 24 */
#define TC_D24X8 6u /* depth in the low 24 bits, stencil 0 */
#define TC_D32S8 7u /* 8 bytes: float depth, then the stencil byte */
/* ETC2 and EAC 4x4 blocks to pixels, a block an invocation (tests/support/etc2.c is the reference) */
#define TC_ETC2_RGB 8u
#define TC_ETC2_PTA 9u
#define TC_ETC2_RGBA 10u
#define TC_EAC_R 11u
#define TC_EAC_R_SNORM 12u
#define TC_EAC_RG 13u
#define TC_EAC_RG_SNORM 14u
/* the pixel formats of frames vi presents from memory (Android's, high bits first) to RGBA8: 2 byte ones a pair an
   invocation, 4 byte ones a pixel */
#define TC_VI_RGB565 15u
#define TC_VI_RGBA5551 16u
#define TC_VI_RGBA4444 17u
#define TC_VI_RGBX8888 18u
#define TC_VI_BGRA8888 19u
/* NVDEC's picture: FFmpeg's U and V planes (U from in_bias, V from st_bias, rows in_pitch and st_pitch apart)
   interleaved into NV12's chroma rows, U first, four pairs an invocation */
#define TC_NV12_UV 20u
/* the DMA copy engine's remap (maxwell/dma.c): each output word of a line from its bytes' sources, a word an
   invocation. remap packs the component size (cs, bits 0-3), the source's and the destination's components (ns,
   nd: 4-7, 8-11) and each destination component's source (12-15, 16-19, 20-23, 24-27: 0-3 a source component, 4
   constant A, 5 constant B, 6 and 7 the destination's own byte, which out holds already). in_pitch and in_bias count
   bytes here: lines can start anywhere in a word */
#define TC_DMA_REMAP 21u
/* the 2D engine's blit (maxwell/fermi2d.c): a row of the destination rectangle's pixels, line_out bytes, each pixel's
   bpp bytes (remap) from the source pixel its column and row name in two tables in the input (words from const_a: a
   column's source column, from const_b: a row's source row). Source rows are in_pitch bytes apart from in_bias */
#define TC_BLIT 22u
/* BC1 to BC7 4x4 blocks to pixels, a block an invocation (tests/support/bcn.c is the reference): RGBA8 for BC1 to BC3
   and BC7, 16 bit unorm or snorm channels for BC4 and BC5, RGBA16F for BC6H */
#define TC_BC1 23u
#define TC_BC2 24u
#define TC_BC3 25u
#define TC_BC4 26u
#define TC_BC4_SNORM 27u
#define TC_BC5 28u
#define TC_BC5_SNORM 29u
#define TC_BC6H_UF 30u
#define TC_BC6H_SF 31u
#define TC_BC7 32u
/* TC_BLIT between float formats (gpu.h's GPU_BLIT_FLOAT, in remap): the source pixel's components as floats, 0 where it
   has none and its alpha 1, each into the destination's, rounded to the nearest half (ties to even) for a half. An
   invocation owns an output word (line_out bytes a row, the last word's bytes past it left) */
#define TC_BLIT_FLOAT 33u

#endif
