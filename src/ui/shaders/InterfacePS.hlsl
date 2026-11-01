// Overlay menu shader, adapted from Zelda64Recomp (GPL-3.0).
//
// RmlUi's vertex colours are premultiplied but its generated textures (font
// glyphs) are not, so the texture is premultiplied here and the pipeline
// blends with ONE / INV_SRC_ALPHA.

SamplerState gSampler : register(s1, space0);
Texture2D<float4> gTexture : register(t2, space1);

void PSMain(
    in float4 iColor : COLOR,
    in float2 iUV : TEXCOORD,
    out float4 oColor : SV_TARGET
)
{
    float4 texel = gTexture.SampleLevel(gSampler, iUV, 0);
    texel.rgb *= texel.a;
    oColor = texel * iColor;
}
