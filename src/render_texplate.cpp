#include <GLES3/gl31.h>
#include "util_shader.h"
#include "render_texplate.h"

static shader_obj_t s_obj;

static float varray[] =
        {-0.5, 0.5, 0.0,
         -0.5, -0.5, 0.0,
         0.5, 0.5, 0.0,
         0.5, -0.5, 0.0};


/* ------------------------------------------------------ *
 *  Shader for Texture
 * ------------------------------------------------------ */
static const char *TexplateVertexShaderGlsl = R"_(#version 320 es

    in vec3 position;
    in lowp vec2 texCoord;

    out lowp vec2 v_TexCoord;

    uniform mat4 u_ModelViewProjection;

    void main() {
       gl_Position = u_ModelViewProjection * vec4(position, 1.0);
       v_TexCoord = texCoord;
    }
    )_";

static const char *TexplateFragmentShaderGlsl = R"_(#version 320 es
    in lowp vec2 v_TexCoord;

    out lowp vec4 color;

    uniform sampler2D u_Texture;

    void main() {
        color = texture(u_Texture, v_TexCoord);
    }
    )_";


int init_texplate() {
    generate_shader(&s_obj, TexplateVertexShaderGlsl, TexplateFragmentShaderGlsl);

    return 0;
}


typedef struct _texparam {
    int texid;
    XrMatrix4x4f matPVM;
} texparam_t;


static void flip_texcoord(float *uv) {
    uv[1] = 1.0f - uv[1];
    uv[3] = 1.0f - uv[3];
    uv[5] = 1.0f - uv[5];
    uv[7] = 1.0f - uv[7];
}


static int draw_texture_in(texparam_t *tparam) {
    float tarray[] = {
            0.0, 0.0,
            0.0, 1.0,
            1.0, 0.0,
            1.0, 1.0};
    float *uv = tarray;

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glUseProgram(s_obj.program);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(s_obj.loc_texture, 0);

    glBindTexture(GL_TEXTURE_2D, tparam->texid);

    flip_texcoord(uv);

    if (s_obj.loc_tex_coord >= 0) {
        glEnableVertexAttribArray(s_obj.loc_tex_coord);
        glVertexAttribPointer(s_obj.loc_tex_coord, 2, GL_FLOAT, GL_FALSE, 0, uv);
    }

    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);

    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUniformMatrix4fv(s_obj.loc_mvp, 1, GL_FALSE,
                       reinterpret_cast<const GLfloat *>(&tparam->matPVM));

    if (s_obj.loc_position >= 0) {
        glEnableVertexAttribArray(s_obj.loc_position);
        glVertexAttribPointer(s_obj.loc_position, 3, GL_FLOAT, GL_FALSE, 0, varray);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);

    return 0;
}


int draw_tex_plate(int texid, const XrMatrix4x4f &matPVM) {
    texparam_t tparam = {0};
    tparam.texid = texid;
    tparam.matPVM = matPVM;
    draw_texture_in(&tparam);

    return 0;
}

