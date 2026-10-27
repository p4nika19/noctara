#version 440
// Dual Kawase downsample pass (Bjørge, SIGGRAPH 2015): the centre plus four diagonal taps, written into a
// texture half the size of the source. Bilinear filtering makes every tap average four texels.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 texel;    // 1 / source texture size
    float offset;  // tap distance in source texels
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    vec2 o = texel * offset;
    vec4 sum = texture(source, qt_TexCoord0) * 4.0;
    sum += texture(source, qt_TexCoord0 - o);
    sum += texture(source, qt_TexCoord0 + o);
    sum += texture(source, qt_TexCoord0 + vec2(o.x, -o.y));
    sum += texture(source, qt_TexCoord0 - vec2(o.x, -o.y));
    fragColor = sum * 0.125;
}
