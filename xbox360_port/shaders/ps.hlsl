//-----------------------------------------------------------------------------
// ps.hlsl - Silent Hill Xbox 360 pixel shader.
//
// PSX texel combine, which is NOT the usual modulate:
//   out.rgb = texel * colour + fog
// The colour arrives already doubled (PutVertUV scales by 2 because PSX colour
// 0x80 means 1.0), and fog is ADDED after the modulate, matching the NV2A final
// combiner and PsyCross's mix().
//-----------------------------------------------------------------------------

// .x = the alpha this PSX blend mode needs. libXenon exposes no blend-factor
// setter, so the 0.5 of ABR "0.5*B + 0.5*F" and the 0.25 of "B + 0.25*F" have to
// reach the blender as SOURCE ALPHA. gpu_xenos.c sets this alongside every
// Xe_SetBlendControl, so the two can never drift apart.
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

    // PSX texel 0x0000 is fully transparent, and psx_vram decodes it to alpha 0.
    // Killing those here rather than through the blender keeps the semi-
    // transparent modes free to use alpha for their blend weight instead.
    clip(texel.a - 0.5f);

    float4 result;
    result.rgb = texel.rgb * input.col.rgb + input.spec.rgb;
    result.a   = gBlend.x;
    return result;
}
