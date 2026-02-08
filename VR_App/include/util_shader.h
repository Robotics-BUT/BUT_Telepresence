/**
 * util_shader.h - OpenGL ES shader compilation and linking
 *
 * Provides a simple shader_obj_t that bundles a linked program with
 * its common uniform/attribute locations, plus helpers for compilation
 * and error checking.
 */
#pragma once

/** Compiled shader program with cached uniform/attribute locations. */
struct shader_obj_t {
    GLuint program;
    GLint loc_mvp;        /* uniform: model-view-projection matrix */
    GLint loc_texture;    /* uniform: texture sampler */
    GLint loc_position;   /* attribute: vertex position */
    GLint loc_tex_coord;  /* attribute: texture coordinate */
};

/** Compile vertex + fragment shaders, link, and resolve locations. */
int generate_shader(shader_obj_t *shader_obj, const char* vertex_shader,
                    const char* &fragment_shader);

/** Log compilation errors for a shader object. */
void check_shader(GLuint shader);

/** Link vertex and fragment shaders into a program. */
GLuint link_shaders(GLuint vertex_shader, GLuint fragment_shader);

/** Log linking errors for a program object. */
void check_program(GLuint program);