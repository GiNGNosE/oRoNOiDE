#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    uint debugMode;
    uint _pad0;
    uint _pad1;
    uint _pad2;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in uint inMaterialId;

layout(set = 0, binding = 1) uniform SceneUBO {
    vec3 cameraPosition;
    float _pad0;
    vec3 lightDirection;
    float _pad1;
    vec3 lightColor;
    float lightIntensity;
    vec3 ambientColor;
    float ambientIntensity;
    mat4 lightSpaceMatrix;
} scene;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) flat out uint fragMaterialId;
layout(location = 2) out vec3 fragWorldPosition;
layout(location = 3) out vec4 fragLightSpacePosition;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragNormal = inNormal;
    fragMaterialId = inMaterialId;
    fragWorldPosition = inPosition;
    fragLightSpacePosition = scene.lightSpaceMatrix * vec4(inPosition, 1.0);
}
