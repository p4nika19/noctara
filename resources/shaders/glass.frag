#version 440
// Frosted glass (Dual Kawase and Gaussian styles): the shared blurred backdrop behind the panel, with a
// saturation and brightness lift, cut to the panel's rounded rectangle with a one-pixel anti-aliased edge.
// The veil, sheen and edge are drawn on top by GlassPanel.qml.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 1) in vec2 windowUv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float ndcYSign;
    vec2 itemSize;
    vec2 windowSize;
    float radius;
    float saturation;
    float brightness;
    float refraction;
    float thickness;
    float dispersion;
    float specular;
    float fresnel;
    float rimDarkening;
    vec2 lightDir;
    vec4 rimTint;
    float grain;
};
layout(binding = 1) uniform sampler2D source;

// Cheap per-pixel hash noise in [-1, 1] (stable: depends on the pixel position only).
float noise(vec2 p)
{
    return fract(sin(dot(floor(p), vec2(12.9898, 78.233))) * 43758.5453) * 2.0 - 1.0;
}

float roundedBoxDistance(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + vec2(r);
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 halfSize = itemSize * 0.5;
    float r = min(radius, min(halfSize.x, halfSize.y));
    float d = roundedBoxDistance(qt_TexCoord0 * itemSize - halfSize, halfSize, r);
    float coverage = clamp(0.5 - d, 0.0, 1.0);

    vec3 c = texture(source, windowUv).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, saturation) + vec3(brightness + grain * noise(qt_TexCoord0 * itemSize));
    c = clamp(c, 0.0, 1.0);
    fragColor = vec4(c, 1.0) * (coverage * qt_Opacity);
}
