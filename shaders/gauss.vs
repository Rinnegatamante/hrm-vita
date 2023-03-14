

void main(
	float2 a_position,
	float2 a_texCoord0,
	float2 out v_texCoord0 : TEXCOORD0,
	float4 out gl_Position : POSITION
) {
	gl_Position = float4( a_position, 0.5, 1.0 );
	v_texCoord0 = a_texCoord0;
}
