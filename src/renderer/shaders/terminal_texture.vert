#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
layout(location = 2) in vec4 color;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColor;

void main()
{
    vTexCoord = texCoord;
    vColor = color;
    gl_Position = vec4(position, 0.0, 1.0);
}
