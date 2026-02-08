#include <string>
#include <GLES3/gl3.h>
#include "pch.h"
#include "check.h"

#include "util_shader.h"


int generate_shader(shader_obj_t *shader_obj, const char* vertex_shader,
                    const char* &fragment_shader) {

    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertex_shader, nullptr);
    glCompileShader(vertexShader);
    check_shader(vertexShader);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragment_shader, nullptr);
    glCompileShader(fragmentShader);
    check_shader(fragmentShader);

    shader_obj->program = link_shaders(vertexShader, fragmentShader);
    shader_obj->loc_position = glGetAttribLocation(shader_obj->program, "position");
    shader_obj->loc_tex_coord = glGetAttribLocation(shader_obj->program, "texCoord");

    shader_obj->loc_mvp = glGetUniformLocation(shader_obj->program, "u_ModelViewProjection");
    shader_obj->loc_texture = glGetUniformLocation(shader_obj->program, "u_Texture");

    return 0;
}

void check_shader(GLuint shader) {
    GLint r = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &r);
    if (r == GL_FALSE) {
        GLchar msg[4096] = {};
        GLsizei length;
        glGetShaderInfoLog(shader, sizeof(msg), &length, msg);
        THROW(Fmt("Compile shader failed: %s", msg))
    }
}

GLuint link_shaders(GLuint vertex_shader, GLuint fragment_shader) {
    GLuint program = glCreateProgram();

    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    check_program(program);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    return program;
}

void check_program(GLuint program) {
    GLint r = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &r);
    if (r == GL_FALSE) {
        GLchar msg[4096];
        GLsizei length;
        glGetProgramInfoLog(program, sizeof(msg), &length, msg);
        THROW(Fmt("Link program failed: %s", msg))
    }
}