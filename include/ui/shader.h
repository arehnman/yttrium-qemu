#ifndef QEMU_SHADER_H
#define QEMU_SHADER_H

typedef struct QemuGLShader QemuGLShader;

void qemu_gl_run_texture_blit(QemuGLShader *gls, bool flip);
bool qemu_gl_run_external_texture_blit(QemuGLShader *gls, uint32_t texture,
                                      const float crop[4]);

QemuGLShader *qemu_gl_init_shader(void);
void qemu_gl_fini_shader(QemuGLShader *gls);

#endif /* QEMU_SHADER_H */
