#version 440
// Soft shadow / glow of a rounded rectangle, evaluated analytically from its signed distance field.
// One quad, no offscreen pass, no texture: the cost is a few ALU ops per covered pixel.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 color;      // shadow colour, premultiplied (Qt premultiplies colour uniforms of a ShaderEffect)
    vec2 itemSize;   // size of this quad in pixels
    vec4 box;        // casting rectangle inside the quad: x, y, width, height (pixels)
    float radius;    // corner radius of the casting rectangle
    float softness;  // blur extent in pixels
};

float roundedBoxDistance(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + vec2(r);
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 p = qt_TexCoord0 * itemSize;
    vec2 halfSize = box.zw * 0.5;
    float r = min(radius, min(halfSize.x, halfSize.y));
    float d = roundedBoxDistance(p - (box.xy + halfSize), halfSize, r);
    // Smooth, Gaussian-like falloff across the blur extent (tanh approximates erf).
    float s = max(softness, 0.5) * 0.5;
    float a = 0.5 - 0.5 * tanh(d / s);
    fragColor = color * (a * qt_Opacity);
}
