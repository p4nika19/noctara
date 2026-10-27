#version 440
// One direction of a separable Gaussian blur (run horizontally, then vertically). 2 x 12 taps plus the
// centre; the tap spacing widens with sigma so the kernel always covers 3 sigma.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 direction;  // one source texel along the blur axis, in texture coordinates
    float sigma;     // standard deviation in source texels
};
layout(binding = 1) uniform sampler2D source;

const int kTaps = 12;

void main()
{
    float s = max(sigma, 0.5);
    float spacing = max(1.0, 3.0 * s / float(kTaps));
    vec4 sum = texture(source, qt_TexCoord0);
    float total = 1.0;
    for (int i = 1; i <= kTaps; ++i) {
        float x = float(i) * spacing;
        float w = exp(-0.5 * x * x / (s * s));
        vec2 d = direction * x;
        sum += (texture(source, qt_TexCoord0 + d) + texture(source, qt_TexCoord0 - d)) * w;
        total += 2.0 * w;
    }
    fragColor = sum / total;
}
