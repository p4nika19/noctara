#version 440
// Colours each bar by where it lies relative to the play position and the pointer (uniforms only, so playing
// and hovering never rebuild the geometry), with rounded, antialiased ends from the bar's distance field.

layout(location = 0) in vec4 vBox;
layout(location = 1) in vec2 vExtra;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float progress;
    float hover;
    float pad;
    vec4 played;      // straight (non-premultiplied) colours
    vec4 unplayed;
    vec4 stored;      // not played yet, already stored on this computer
    vec4 hoverColor;  // between the play position and the pointer
};

float roundedBoxDistance(vec2 p, vec2 halfSize, float r)
{
    vec2 q = abs(p) - halfSize + vec2(r);
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 halfSize = vBox.zw;
    float d = roundedBoxDistance(vBox.xy, halfSize, min(halfSize.x, halfSize.y));
    float coverage = clamp(0.5 - d / max(fwidth(d), 1e-3), 0.0, 1.0);

    float at = vExtra.x;
    vec4 color = at <= progress ? played : (vExtra.y > 0.5 ? stored : unplayed);
    if (hover >= 0.0) {
        if (hover > progress && at > progress && at <= hover) {
            color = hoverColor;
        } else if (hover < progress && at > hover && at <= progress) {
            color = mix(played, unplayed, 0.5);
        }
    }
    float alpha = color.a * coverage * qt_Opacity;
    fragColor = vec4(color.rgb * alpha, alpha);
}
