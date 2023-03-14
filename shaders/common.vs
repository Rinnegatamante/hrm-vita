
uniform float4x4 projMtx;

void main(
	float3 a_position,
	float4 a_color,
	float2 a_texCoord0,
	float4 out v_color : COLOR,
	float2 out v_texCoord0: TEXCOORD0,
	float4 out gl_Position : POSITION
) {
	gl_Position = mul(projMtx, float4( a_position, 1.0 ));
	v_color = a_color.bgra;
	v_texCoord0= a_texCoord0;
}
