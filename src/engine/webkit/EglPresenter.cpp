#include "EglPresenter.hpp"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace iridium::engine::webkit {

namespace {

// The EGL display is process-wide state, not per-view state: every
// eglGetPlatformDisplay/eglGetDisplay call for the same platform returns the
// same handle, and wpe_fdo_initialize_for_egl_display keeps using it for the
// lifetime of the process. Acquiring and terminating it once per view made
// every tab open/close re-initialize driver state that was never reclaimed,
// so it is created once here and deliberately never terminated. Only the
// context, surface and GL objects below are per-presenter.
struct SharedDisplay {
    EGLDisplay display { EGL_NO_DISPLAY };
    EGLConfig config { nullptr };
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture { nullptr };
    bool attempted { false };
};

SharedDisplay& sharedDisplay()
{
    static SharedDisplay shared;
    if (shared.attempted)
        return shared;
    shared.attempted = true;

    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (getPlatformDisplay)
        shared.display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
            EGL_DEFAULT_DISPLAY, nullptr);
    if (shared.display == EGL_NO_DISPLAY)
        shared.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (shared.display == EGL_NO_DISPLAY)
        return shared;

    if (!eglInitialize(shared.display, nullptr, nullptr))
        return shared;
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        return shared;

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint configCount = 0;
    if (!eglChooseConfig(shared.display, configAttribs, &shared.config, 1, &configCount)
        || configCount < 1)
        return shared;

    shared.imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    return shared;
}

} // namespace

std::unique_ptr<EglPresenter> EglPresenter::create()
{
    SharedDisplay& shared = sharedDisplay();
    if (shared.display == EGL_NO_DISPLAY || !shared.config || !shared.imageTargetTexture)
        return nullptr;

    auto presenter = std::unique_ptr<EglPresenter>(new EglPresenter());
    presenter->m_display = shared.display;

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    presenter->m_context = eglCreateContext(shared.display, shared.config,
        EGL_NO_CONTEXT, contextAttribs);
    if (presenter->m_context == EGL_NO_CONTEXT)
        return nullptr;

    if (!eglMakeCurrent(shared.display, EGL_NO_SURFACE, EGL_NO_SURFACE, presenter->m_context)) {
        const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        presenter->m_surface = eglCreatePbufferSurface(shared.display, shared.config,
            pbufferAttribs);
        if (presenter->m_surface == EGL_NO_SURFACE
            || !eglMakeCurrent(shared.display, presenter->m_surface, presenter->m_surface,
                presenter->m_context))
            return nullptr;
    }

    presenter->m_imageTargetTexture = shared.imageTargetTexture;
    return presenter;
}

EglPresenter::~EglPresenter()
{
    // Only per-view resources are released. The display belongs to the process
    // and stays initialized for the web process to keep using.
    if (m_display == EGL_NO_DISPLAY)
        return;
    if (eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
        if (m_texture)
            glDeleteTextures(1, &m_texture);
        if (m_framebuffer)
            glDeleteFramebuffers(1, &m_framebuffer);
    }
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_context != EGL_NO_CONTEXT)
        eglDestroyContext(m_display, m_context);
    if (m_surface != EGL_NO_SURFACE)
        eglDestroySurface(m_display, m_surface);
}

QImage EglPresenter::readImage(EGLImageKHR image, int width, int height)
{
    if (m_display == EGL_NO_DISPLAY || image == EGL_NO_IMAGE_KHR || width <= 0 || height <= 0)
        return {};
    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
        return {};

    if (!m_texture)
        glGenTextures(1, &m_texture);
    if (!m_framebuffer)
        glGenFramebuffers(1, &m_framebuffer);

    glBindTexture(GL_TEXTURE_2D, m_texture);
    m_imageTargetTexture(GL_TEXTURE_2D, image);

    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return {};

    // The exported image already matches QImage's top-down row order, so no
    // vertical flip is needed; RGBA8888 matches GL_RGBA byte order.
    QImage frame(width, height, QImage::Format_RGBA8888_Premultiplied);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.bits());
    return frame;
}

} // namespace iridium::engine::webkit
