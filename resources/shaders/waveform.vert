#version 440
// Waveform bars: one geometry for the whole waveform; each bar is a quad carrying its own local frame.

layout(location = 0) in vec4 vertex;
layout(location = 1) in vec4 box;    // position inside the bar relative to its centre, and its half size (pixels)
layout(location = 2) in vec2 extra;  // the bar's position along the track (0…1), stored (1) or not (0)

layout(location = 0) out vec4 vBox;
layout(location = 1) out vec2 vExtra;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float progress;
    float hover;  // < 0: no hover
    float pad;
    vec4 played;
    vec4 unplayed;
    vec4 stored;
    vec4 hoverColor;
};

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    vBox = box;
    vExtra = extra;
    gl_Position = qt_Matrix * vertex;
}
