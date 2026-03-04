#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    uint debugMode;
    uint _pad0;
    uint _pad1;
    uint _pad2;
} pc;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) flat in uint fragMaterialId;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec4 fragLightSpacePosition;

layout(location = 0) out vec4 outColor;

struct GpuMaterialEntry {
    vec4 albedoRoughness;
    vec4 emissiveMetallic;
};

layout(set = 0, binding = 0) readonly buffer MaterialBuffer {
    GpuMaterialEntry materials[];
};

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

layout(set = 0, binding = 2) uniform sampler2D shadowMap;

const float PI = 3.14159265359;
const float kEpsilon = 1e-5;

GpuMaterialEntry fetchMaterial(uint materialId) {
    uint count = uint(materials.length());
    if (count == 0u) {
        GpuMaterialEntry fallback;
        fallback.albedoRoughness = vec4(0.7, 0.7, 0.7, 0.8);
        fallback.emissiveMetallic = vec4(0.0);
        return fallback;
    }
    uint clampedId = min(materialId, count - 1u);
    return materials[clampedId];
}

vec3 safeNormalize(vec3 value) {
    float lenSq = dot(value, value);
    if (lenSq <= kEpsilon) {
        return vec3(0.0, 0.0, 1.0);
    }
    return value * inversesqrt(lenSq);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(N, H), 0.0);
    float ndoth2 = ndoth * ndoth;
    float denom = ndoth2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, kEpsilon);
}

float geometrySchlickGGX(float ndotx, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotx / max(ndotx * (1.0 - k) + k, kEpsilon);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float ndotv = max(dot(N, V), 0.0);
    float ndotl = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(ndotv, roughness);
    float ggx1 = geometrySchlickGGX(ndotl, roughness);
    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    float factor = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return F0 + (1.0 - F0) * factor;
}

float sampleShadow(vec4 lightSpacePosition, vec3 normal, vec3 lightDir) {
    if (lightSpacePosition.w <= kEpsilon) {
        return 1.0;
    }

    vec3 projCoords = lightSpacePosition.xyz / lightSpacePosition.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 1.0;
    }

    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float ndotl = max(dot(normal, lightDir), 0.0);
    float bias = max(0.00075 * (1.0 - ndotl), 0.0001);
    float visibility = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            float shadowDepth = texture(shadowMap, projCoords.xy + offset).r;
            visibility += ((projCoords.z - bias) <= shadowDepth) ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

void main() {
    GpuMaterialEntry material = fetchMaterial(fragMaterialId);
    vec3 albedo = material.albedoRoughness.rgb;
    vec3 N = safeNormalize(fragNormal);
    if (dot(fragNormal, fragNormal) <= kEpsilon) {
        vec3 dpdx = dFdx(fragWorldPosition);
        vec3 dpdy = dFdy(fragWorldPosition);
        N = safeNormalize(cross(dpdx, dpdy));
    }

    if (pc.debugMode == 3u) {
        outColor = vec4(abs(N), 1.0);
        return;
    }

    if (pc.debugMode == 4u) {
        vec3 dpdx = dFdx(fragWorldPosition);
        vec3 dpdy = dFdy(fragWorldPosition);
        vec3 flatNormal = safeNormalize(cross(dpdx, dpdy));
        outColor = vec4(abs(flatNormal), 1.0);
        return;
    }

    if (pc.debugMode == 5u) {
        outColor = vec4(albedo, 1.0);
        return;
    }

    float roughness = clamp(material.albedoRoughness.a, 0.045, 1.0);
    float metallic = clamp(material.emissiveMetallic.a, 0.0, 1.0);
    vec3 emissive = material.emissiveMetallic.rgb;

    vec3 V = safeNormalize(scene.cameraPosition - fragWorldPosition);
    vec3 L = safeNormalize(-scene.lightDirection);
    vec3 H = safeNormalize(V + L);

    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (D * G * F) / max(4.0 * ndotv * ndotl, kEpsilon);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kD * (albedo / PI);

    vec3 radiance = scene.lightColor * scene.lightIntensity;
    float shadowFactor = sampleShadow(fragLightSpacePosition, N, L);
    vec3 lighting = (diffuse + specular) * radiance * ndotl * shadowFactor;
    vec3 ambient = scene.ambientColor * scene.ambientIntensity * albedo;
    vec3 linearColor = lighting + ambient + emissive;
    vec3 gammaColor = pow(max(linearColor, vec3(0.0)), vec3(1.0 / 2.2));
    outColor = vec4(gammaColor, 1.0);
}
