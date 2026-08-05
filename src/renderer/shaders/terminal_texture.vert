#version 440

layout(location = 0) in vec4 instanceRect;
layout(location = 1) in vec4 instanceUv;
layout(location = 2) in vec4 instanceColor;
layout(location = 3) in vec4 instanceMeta;

layout(std140, binding = 1) uniform PlacementBlock {
    vec4 viewport; // physical width, physical height, DPR, row count
    vec4 rowPlacement[256];
};

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out float vAtlasPage;
layout(location = 3) flat out float vFlags;

void main()
{
    const vec2 corners[4] = vec2[4](
        vec2(0.0, 0.0), vec2(0.0, 1.0),
        vec2(1.0, 0.0), vec2(1.0, 1.0));
    vec2 corner = corners[gl_VertexIndex];
    // The CPU caps interactive terminal rows to this uniform-array capacity,
    // but imported/programmatic models may still be larger. Reject an invalid
    // slot before indexing the array; mapping it to the zero-initialized first
    // row would stack unrelated character rows at y=0.
    if (instanceMeta.y >= viewport.w) {
        vTexCoord = vec2(0.0);
        vColor = vec4(0.0);
        vAtlasPage = 0.0;
        vFlags = 0.0;
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    float rowY = instanceMeta.y >= 0.0
        ? rowPlacement[int(instanceMeta.y)].x : 0.0;
    vec2 logical = mix(instanceRect.xy, instanceRect.zw, corner)
        + vec2(0.0, rowY);
    vec2 physical = logical * viewport.z;
    vec2 ndc = vec2(physical.x / viewport.x * 2.0 - 1.0,
                    1.0 - physical.y / viewport.y * 2.0);
    vTexCoord = mix(instanceUv.xy, instanceUv.zw, corner);
    vColor = instanceColor;
    vAtlasPage = instanceMeta.x;
    vFlags = instanceMeta.z;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
