#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) flat in uint fragMaterialId;

layout(location = 0) out vec4 outColor;

vec3 materialTint(uint materialId) {
    uint idx = materialId % 5u;
    if (idx == 0u) return vec3(0.75, 0.74, 0.72);
    if (idx == 1u) return vec3(0.46, 0.42, 0.37);
    if (idx == 2u) return vec3(0.32, 0.47, 0.30);
    if (idx == 3u) return vec3(0.40, 0.42, 0.52);
    return vec3(0.68, 0.58, 0.42);
}

void main() {
    vec3 normal = normalize(fragNormal);
    vec3 lightDir = normalize(vec3(0.45, 0.8, 0.35));
    float ndotl = max(dot(normal, lightDir), 0.0);
    float ambient = 0.2;
    vec3 albedo = materialTint(fragMaterialId);
    vec3 lit = albedo * (ambient + ndotl);
    outColor = vec4(lit, 1.0);
}
