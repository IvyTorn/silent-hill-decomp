//-----------------------------------------------------------------------------
// ps.hlsl - Silent Hill Xbox 360 pixel shader.
//
// PSX texel combine, which is NOT the usual modulate:
//   out.rgb = texel * colour + fog
// The colour arrives already doubled (PutVertUV scales by 2 because PSX colour
// 0x80 means 1.0), and fog is ADDED after the modulate, matching the NV2A final
// combiner and PsyCross's mix().
//-----------------------------------------------------------------------------

// Output alpha is  texel.a * gBlend.x + gBlend.y , and gpu_xenos.c picks the
// pair. libXenon exposes no blend-factor setter, so the 0.5 of ABR "0.5*B+0.5*F"
// and the 0.25 of "B+0.25*F" have to reach the blender as SOURCE ALPHA.
//
// Two cases, which is why this is a multiply-add rather than a constant:
//
//   TEXTURED, ABR0  -> (1, 0): use the TEXEL's own alpha. psx_vram encodes the
//     PSX STP bit as 0x80 vs 0xFF, so src-alpha blending reproduces PSX
//     semi-transparency PER TEXEL -- an STP texel blends at 50%, a non-STP texel
//     in the same primitive stays opaque. A constant 0.5 would wash out the
//     opaque texels too.
//   UNTEXTURED, ABR0 -> (0, 0.5): the white texture's alpha is 1.0, so the
//     weight has to come from the constant instead.
float4 gBlend : register(c0);

sampler gTex : register(s0);

struct Input
{
    float4 col  : NORMAL;
    float2 uv   : TEXCOORD0;
    float4 spec : TEXCOORD1;
};

float4 main(Input input) : COLOR
{
    float4 texel = tex2D(gTex, input.uv);

    // Only TRUE-transparent texels (PSX 0x0000 -> alpha 0) are killed. The
    // threshold has to sit just above zero, NOT at 0.5: psx_vram encodes an STP
    // texel as alpha 0x80, which is exactly 0.5, so a 0.5 threshold would sit on
    // the boundary and discard every semi-transparent texel in the game.
    clip(texel.a - (0.5f / 255.0f));

    float4 result;
    result.rgb = texel.rgb * input.col.rgb + input.spec.rgb;
    result.a   = texel.a * gBlend.x + gBlend.y;
    return result;
}
