/*
 * php-vio - Default embedded shaders
 * OpenGL 4.1 Core Profile GLSL 410
 */

#ifndef VIO_DEFAULT_SHADERS_H
#define VIO_DEFAULT_SHADERS_H

static const char *vio_default_vertex_shader =
    "#version 410 core\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec4 aColor;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPosition, 1.0);\n"
    "    vColor = aColor;\n"
    "}\n";

static const char *vio_default_vertex_shader_pos_only =
    "#version 410 core\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPosition, 1.0);\n"
    "    vColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
    "}\n";

static const char *vio_default_fragment_shader =
    "#version 410 core\n"
    "in vec4 vColor;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vColor;\n"
    "}\n";

#endif /* VIO_DEFAULT_SHADERS_H */
