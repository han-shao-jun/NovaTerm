#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D glyphAtlas;

void main()
{
    float coverage = texture(glyphAtlas, vTexCoord).a;
    fragColor = vec4(vColor.rgb, vColor.a * coverage);
}
