#pragma once
struct shader_obj_t {
    GLuint program;
    GLint loc_mvp;
    GLint loc_texture;
    GLint loc_position;
    GLint loc_tex_coord;
};

int generate_shader(shader_obj_t *shader_obj, const char* vertex_shader,
                    const char* &fragment_shader);

void check_shader(GLuint shader);

GLuint link_shaders(GLuint vertex_shader, GLuint fragment_shader);

void check_program(GLuint program);