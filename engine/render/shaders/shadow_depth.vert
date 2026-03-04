#version 450

layout(push_constant) uniform ShadowPushConstants {
    mat4 lightViewProj;
} pc;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = pc.lightViewProj * vec4(inPosition, 1.0);
}
