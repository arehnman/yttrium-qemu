/*
 * QEMU opengl shader helper functions
 *
 * Copyright (c) 2014 Red Hat
 *
 * Authors:
 *    Gerd Hoffmann <kraxel@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include <epoxy/gl.h>
#include "ui/shader.h"

#include "ui/shader/texture-blit-vert.h"
#include "ui/shader/texture-blit-flip-vert.h"
#include "ui/shader/texture-blit-frag.h"

struct QemuGLShader {
    GLint texture_blit_prog;
    GLint texture_blit_flip_prog;
    GLint texture_blit_vao;
    GLuint external_blit_prog;
    GLuint external_blit_vao;
    GLint external_blit_crop;
    bool external_blit_initialized;
    bool external_blit_validated;
};

/* ---------------------------------------------------------------------- */

static GLuint qemu_gl_init_texture_blit(GLint texture_blit_prog)
{
    static const GLfloat in_position[] = {
        -1, -1,
        1,  -1,
        -1,  1,
        1,   1,
    };
    GLint l_position;
    GLuint vao, buffer;

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    /* this is the VBO that holds the vertex data */
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(in_position), in_position,
                 GL_STATIC_DRAW);

    l_position = glGetAttribLocation(texture_blit_prog, "in_position");
    glVertexAttribPointer(l_position, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(l_position);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    return vao;
}

void qemu_gl_run_texture_blit(QemuGLShader *gls, bool flip)
{
    glUseProgram(flip
                 ? gls->texture_blit_flip_prog
                 : gls->texture_blit_prog);
    glBindVertexArray(gls->texture_blit_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ---------------------------------------------------------------------- */

static GLuint qemu_gl_create_compile_shader(GLenum type, const GLchar *src)
{
    GLuint shader;
    GLint status, length;
    char *errmsg;

    shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, 0);
    glCompileShader(shader);

    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        errmsg = g_malloc(length);
        glGetShaderInfoLog(shader, length, &length, errmsg);
        fprintf(stderr, "%s: compile %s error\n%s\n", __func__,
                (type == GL_VERTEX_SHADER) ? "vertex" : "fragment",
                errmsg);
        g_free(errmsg);
        return 0;
    }
    return shader;
}

static GLuint qemu_gl_create_link_program(GLuint vert, GLuint frag)
{
    GLuint program;
    GLint status, length;
    char *errmsg;

    program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        errmsg = g_malloc(length);
        glGetProgramInfoLog(program, length, &length, errmsg);
        fprintf(stderr, "%s: link program: %s\n", __func__, errmsg);
        g_free(errmsg);
        return 0;
    }
    return program;
}

static GLuint qemu_gl_create_compile_link_program(const GLchar *vert_src,
                                                  const GLchar *frag_src)
{
    GLuint vert_shader, frag_shader, program = 0;

    vert_shader = qemu_gl_create_compile_shader(GL_VERTEX_SHADER, vert_src);
    frag_shader = qemu_gl_create_compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vert_shader || !frag_shader) {
        goto end;
    }

    program = qemu_gl_create_link_program(vert_shader, frag_shader);

end:
    glDeleteShader(vert_shader);
    glDeleteShader(frag_shader);

    return program;
}

static bool qemu_gl_init_external_texture_blit(QemuGLShader *gls)
{
    static const GLchar vert_src[] =
        "#version 300 es\n"
        "uniform highp vec4 crop;\n"
        "out highp vec2 ex_tex_coord;\n"
        "const vec2 positions[4] = vec2[4](vec2(-1.0, -1.0),\n"
        "    vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));\n"
        "void main() {\n"
        "    vec2 position = positions[gl_VertexID];\n"
        "    gl_Position = vec4(position, 0.0, 1.0);\n"
        "    ex_tex_coord = crop.xy + (position + 1.0) * 0.5 * crop.zw;\n"
        "}\n";
    static const GLchar frag_src[] =
        "#version 300 es\n"
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "uniform lowp samplerExternalOES image;\n"
        "in highp vec2 ex_tex_coord;\n"
        "out mediump vec4 out_frag_color;\n"
        "void main() {\n"
        "    out_frag_color = texture(image, ex_tex_coord);\n"
        "}\n";
    GLint old_prog, image;

    if (gls->external_blit_initialized) {
        return gls->external_blit_prog != 0;
    }
    gls->external_blit_initialized = true;
    /*
     * Desktop GL can accept the ES shader extension without advertising the
     * GLES extension strings.  Compile and validate against the actual source
     * below; GLES contexts must advertise the external ES3 shader capability.
     */
    if (epoxy_is_desktop_gl() ?
        (epoxy_gl_version() < 43 &&
         !epoxy_has_gl_extension("GL_ARB_ES3_compatibility")) :
        (!epoxy_has_gl_extension("GL_OES_EGL_image_external") ||
         !epoxy_has_gl_extension("GL_OES_EGL_image_external_essl3"))) {
        error_report("opengl: external DMA-BUF sampling requires "
                     "ES3 external shader support in the current GL context");
        return false;
    }
    gls->external_blit_prog = qemu_gl_create_compile_link_program(vert_src,
                                                                frag_src);
    if (!gls->external_blit_prog) {
        error_report("opengl: external DMA-BUF copy shader creation failed");
        return false;
    }
    gls->external_blit_crop = glGetUniformLocation(gls->external_blit_prog,
                                                  "crop");
    image = glGetUniformLocation(gls->external_blit_prog, "image");
    glGenVertexArrays(1, &gls->external_blit_vao);
    if (gls->external_blit_crop < 0 || image < 0 ||
        !gls->external_blit_vao) {
        error_report("opengl: external DMA-BUF copy shader has invalid "
                     "uniforms or vertex array");
        glDeleteProgram(gls->external_blit_prog);
        gls->external_blit_prog = 0;
        return false;
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
    glUseProgram(gls->external_blit_prog);
    glUniform1i(image, 0);
    glUseProgram(old_prog);
    return true;
}

/* The caller owns the destination framebuffer and viewport. */
bool qemu_gl_run_external_texture_blit(QemuGLShader *gls, uint32_t texture,
                                      const float crop[4])
{
    static const GLenum tests[] = {
        GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
        GL_CULL_FACE, GL_RASTERIZER_DISCARD,
    };
    GLboolean enabled[G_N_ELEMENTS(tests)], color_mask[4];
    GLint program, vao, active, binding, sampler;
    GLenum gl_error;
    bool ok = true;
    int i;

    if (!qemu_gl_init_external_texture_blit(gls)) {
        return false;
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &binding);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    for (i = 0; i < G_N_ELEMENTS(tests); i++) {
        enabled[i] = glIsEnabled(tests[i]);
        glDisable(tests[i]);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glUseProgram(gls->external_blit_prog);
    glUniform4fv(gls->external_blit_crop, 1, crop);
    glBindVertexArray(gls->external_blit_vao);
    if (!gls->external_blit_validated) {
        GLint status, length;
        g_autofree char *message = NULL;

        glValidateProgram(gls->external_blit_prog);
        glGetProgramiv(gls->external_blit_prog, GL_VALIDATE_STATUS, &status);
        if (!status) {
            glGetProgramiv(gls->external_blit_prog, GL_INFO_LOG_LENGTH, &length);
            message = g_malloc0(MAX(length, 1));
            glGetProgramInfoLog(gls->external_blit_prog, length, NULL, message);
            error_report("opengl: external DMA-BUF copy shader validation "
                         "failed with the source bound: %s", message);
            ok = false;
        } else {
            gls->external_blit_validated = true;
        }
    }
    if (ok) {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glBindVertexArray(vao);
    glUseProgram(program);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, binding);
    glBindSampler(0, sampler);
    glActiveTexture(active);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    for (i = 0; i < G_N_ELEMENTS(tests); i++) {
        if (enabled[i]) {
            glEnable(tests[i]);
        }
    }
    while ((gl_error = glGetError()) != GL_NO_ERROR) {
        error_report("opengl: external DMA-BUF mailbox copy GL error 0x%x",
                     gl_error);
        ok = false;
    }
    if (!ok && !gls->external_blit_validated) {
        /* Cache a failed initialization/validation instead of retrying it. */
        glDeleteProgram(gls->external_blit_prog);
        gls->external_blit_prog = 0;
    }
    return ok;
}

/* ---------------------------------------------------------------------- */

QemuGLShader *qemu_gl_init_shader(void)
{
    QemuGLShader *gls = g_new0(QemuGLShader, 1);

    gls->texture_blit_prog = qemu_gl_create_compile_link_program
        (texture_blit_vert_src, texture_blit_frag_src);
    gls->texture_blit_flip_prog = qemu_gl_create_compile_link_program
        (texture_blit_flip_vert_src, texture_blit_frag_src);
    if (!gls->texture_blit_prog || !gls->texture_blit_flip_prog) {
        exit(1);
    }

    gls->texture_blit_vao =
        qemu_gl_init_texture_blit(gls->texture_blit_prog);

    return gls;
}

void qemu_gl_fini_shader(QemuGLShader *gls)
{
    if (!gls) {
        return;
    }
    glDeleteProgram(gls->texture_blit_prog);
    glDeleteProgram(gls->texture_blit_flip_prog);
    glDeleteProgram(gls->texture_blit_vao);
    glDeleteProgram(gls->external_blit_prog);
    glDeleteVertexArrays(1, &gls->external_blit_vao);
    g_free(gls);
}
