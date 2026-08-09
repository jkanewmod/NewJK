/*[Vertex]*/
out vec2 var_TexCoords;

void main()
{
	vec2 position = vec2(2.0 * float(gl_VertexID & 2) - 1.0, 4.0 * float(gl_VertexID & 1) - 1.0);
	gl_Position = vec4(position, 0.0, 1.0);
	var_TexCoords = position * 0.5 + vec2(0.5);
}

/*[Fragment]*/
uniform sampler2D u_TextureMap;
uniform float u_InvGamma;

in vec2 var_TexCoords;

out vec4 out_Color;

void main()
{
	vec3 color = texture(u_TextureMap, var_TexCoords).rgb;
	color = pow(max(color, vec3(0.0)), vec3(u_InvGamma));
	out_Color = vec4(color, 1.0);
}
