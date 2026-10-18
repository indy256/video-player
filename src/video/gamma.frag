#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float gamma;
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    vec4 pixel = texture(source, qt_TexCoord0);
    vec3 rgb = pixel.a > 0.0 ? pixel.rgb / pixel.a : vec3(0.0);
    fragColor = vec4(pow(clamp(rgb, 0.0, 1.0), vec3(1.0 / gamma)) * pixel.a, pixel.a) * qt_Opacity;
}
