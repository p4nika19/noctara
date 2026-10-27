#version 440
// Liquid Glass: like frosted glass, but the panel behaves like a thick lens. Near the rim the background is
// pulled inwards along the surface normal (refraction), the three colour channels bend by slightly different
// amounts (dispersion), the rim facing the light catches a specular highlight, the whole rim brightens with
// the viewing angle (Fresnel), and the side facing away from the light darkens.

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
    vec2 p = qt_TexCoord0 * itemSize - halfSize;
    float d = roundedBoxDistance(p, halfSize, r);
    float coverage = clamp(0.5 - d, 0.0, 1.0);

    // Outward surface normal from the distance field's gradient.
    const float e = 0.75;
    vec2 grad = vec2(roundedBoxDistance(p + vec2(e, 0.0), halfSize, r) - roundedBoxDistance(p - vec2(e, 0.0), halfSize, r),
                     roundedBoxDistance(p + vec2(0.0, e), halfSize, r) - roundedBoxDistance(p - vec2(0.0, e), halfSize, r));
    vec2 n = length(grad) > 1e-4 ? normalize(grad) : vec2(0.0);

    // 1 at the rim, falling to 0 at `thickness` pixels inside; squared for a lens-like profile.
    float rim = clamp(1.0 + d / max(thickness, 1.0), 0.0, 1.0);
    float bend = rim * rim;

    // Pull the sample inwards (towards the centre) proportionally to the bend; channels bend differently.
    vec2 shift = -n * (refraction * bend) / windowSize;
    vec3 c;
    c.r = texture(source, windowUv + shift * (1.0 + dispersion)).r;
    c.g = texture(source, windowUv + shift).g;
    c.b = texture(source, windowUv + shift * (1.0 - dispersion)).b;

    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, saturation) + vec3(brightness + grain * noise(qt_TexCoord0 * itemSize));

    float facing = dot(n, lightDir);
    float band = pow(rim, 3.0);
    // The highlight is a thin line along the rim, not a patch: on large corner radii the corner faces the
    // light over a long arc, and a wide band there reads as glare.
    c += vec3(1.0) * specular * pow(max(facing, 0.0), 8.0) * pow(rim, 8.0);
    c += vec3(1.0) * fresnel * band * 0.5;
    c += rimTint.rgb * pow(max(-facing, 0.0), 2.0) * band;
    c *= 1.0 - rimDarkening * max(-facing, 0.0) * rim;

    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0) * (coverage * qt_Opacity);
}
