#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

/* BC2/BC3 decode into an AFRC-compressed RGBA8 shadow: one fragment per
 * texel of the render area, the tile writeback does the compression. */

layout(location = 0) out vec4 o_color;

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer InBuf {
   uint data[];
};

layout(push_constant) uniform Registers
{
   uvec2 src;          /* block holding texel (x0, y0) of layer 0 */
   uint src_row_B;
   uint src_slice_B;
   int x0;
   int y0;
   int bc2;
} registers;

vec3 dec565(uint c)
{
   uint r = (c >> 11u) & 31u, g = (c >> 5u) & 63u, b = c & 31u;
   return vec3((r << 3u) | (r >> 2u), (g << 2u) | (g >> 4u), (b << 3u) | (b >> 2u));
}

/* BC3 alpha palette entry, D3D rounding (divisions as exact multiply-shifts). */
uint pal8(uint a0, uint a1, uint k)
{
   if (k == 0u)
      return a0;
   if (k == 1u)
      return a1;
   if (a0 > a1)
      return ((a0 * (8u - k) + a1 * (k - 1u) + 3u) * 9363u) >> 16u;
   if (k > 5u)
      return k == 6u ? 0u : 255u;
   return ((a0 * (6u - k) + a1 * (k - 1u) + 2u) * 13108u) >> 16u;
}

void main()
{
   const ivec2 p = ivec2(gl_FragCoord.xy) - ivec2(registers.x0, registers.y0);
   const uint word = (uint(gl_Layer) * registers.src_slice_B +
                      uint(p.y >> 2) * registers.src_row_B + uint(p.x >> 2) * 16u) / 4u;
   InBuf buf = InBuf(registers.src);
   const uvec4 w = uvec4(buf.data[word], buf.data[word + 1u], buf.data[word + 2u],
                         buf.data[word + 3u]);
   const uint i = uint((p.y & 3) * 4 + (p.x & 3));

   const vec3 e0 = dec565(w.z & 0xffffu), e1 = dec565(w.z >> 16u);
   const uint k = (w.w >> (2u * i)) & 3u;
   const vec3 c = k == 0u ? e0 : k == 1u ? e1 :
                  k == 2u ? (2.0 * e0 + e1) / 3.0 : (e0 + 2.0 * e1) / 3.0;

   uint a;
   if (registers.bc2 != 0) {
      a = ((w[i >> 3u] >> ((i & 7u) * 4u)) & 15u) * 17u;
   } else {
      const uint pos = 16u + 3u * i;
      uint idx;
      if (pos + 3u <= 32u)
         idx = (w.x >> pos) & 7u;
      else if (pos >= 32u)
         idx = (w.y >> (pos - 32u)) & 7u;
      else
         idx = ((w.x >> pos) | (w.y << (32u - pos))) & 7u;
      a = pal8(w.x & 0xffu, (w.x >> 8u) & 0xffu, idx);
   }

   o_color = vec4(c, float(a)) * (1.0 / 255.0);
}
