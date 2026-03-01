#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in uint inMaterialId;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) flat out uint fragMaterialId;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragNormal = inNormal;
    fragMaterialId = inMaterialId;
}
