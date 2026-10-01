#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <QImage>

#include <memory>

namespace iridium::engine::webkit {

// Imports the EGL images exported by wpebackend-fdo and reads them back into
// QImages. The web process only gets a usable hardware EGL display when the FDO
// renderer is initialized with that display, so this also gates hardware
// rasterization.
//
// The display is process-wide and is acquired once, never terminated; each
// presenter owns only its context, surface and GL objects. Terminating the
// display per view leaked a few megabytes of driver state per tab.
class EglPresenter final {
public:
    // Returns null when no usable EGL display/context could be created.
    static std::unique_ptr<EglPresenter> create();
    ~EglPresenter();

    EGLDisplay display() const { return m_display; }

    // Imports the image into a texture and reads it back. The image stays
    // owned by the caller and must be released afterwards.
    QImage readImage(EGLImageKHR image, int width, int height);

private:
    EglPresenter() = default;

    EGLDisplay m_display { EGL_NO_DISPLAY };
    EGLContext m_context { EGL_NO_CONTEXT };
    EGLSurface m_surface { EGL_NO_SURFACE };
    GLuint m_texture { 0 };
    GLuint m_framebuffer { 0 };
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_imageTargetTexture { nullptr };
};

} // namespace iridium::engine::webkit
