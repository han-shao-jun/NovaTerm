#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in float vAtlasPage;
layout(location = 3) flat in float vFlags;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2DArray glyphAtlas;

void main()
{
    vec4 sampleColor = texture(glyphAtlas, vec3(vTexCoord, vAtlasPage));
    fragColor = vFlags > 0.5
        ? vec4(sampleColor.rgb, sampleColor.a * vColor.a)
        : vec4(vColor.rgb, vColor.a * sampleColor.a);
}
