#version 440
// Shared by the glass fragment shaders: besides the item's own texture coordinates it passes on where the
// fragment lies in the window, so a panel samples the shared blurred backdrop exactly behind itself. Being
// derived from qt_Matrix, this stays correct while the panel scrolls, moves or animates.

layout(location = 0) in vec4 qt_Vertex;
layout(location = 1) in vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 qt_TexCoord0;
layout(location = 1) out vec2 windowUv;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float ndcYSign;     // -1 when NDC has Y up (OpenGL), +1 when it points down (Vulkan)
    vec2 itemSize;      // panel size in pixels
    vec2 windowSize;    // window size in pixels
    float radius;       // corner radius
    float saturation;   // 1 = unchanged
    float brightness;   // added to every channel
    float refraction;   // liquid: how far the rim bends the background, in pixels
    float thickness;    // liquid: width of the bending rim, in pixels
    float dispersion;   // liquid: colour separation of the bend (0..1)
    float specular;     // liquid: highlight on the rim facing the light
    float fresnel;      // liquid: brightening along the whole rim
    float rimDarkening; // liquid: shade on the rim facing away from the light
    vec2 lightDir;      // liquid: unit vector towards the light, y down
    vec4 rimTint;       // liquid: colour of the light caught at the edge (premultiplied by its strength)
    float grain;        // amplitude of the fine noise added to the glass
};

void main()
{
    qt_TexCoord0 = qt_MultiTexCoord0;
    gl_Position = qt_Matrix * qt_Vertex;
    vec2 ndc = gl_Position.xy / gl_Position.w;
    windowUv = vec2(ndc.x * 0.5 + 0.5, ndcYSign * ndc.y * 0.5 + 0.5);
}
