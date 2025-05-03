/**
 * Ferret Window Manager
 * A modern C++23 compositing window manager based on X11 and OpenGL
 *
 * Compositor.h - Handles OpenGL-based window compositing
 */

#pragma once

// X11 includes
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

// OpenGL includes
#include <GL/glew.h>
#include <GL/glx.h>

// Standard includes
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Local includes
#include "WindowManager.h"

namespace ferret {

// Forward declarations
class WindowManager;

// GLX function pointer types
using glXCreateContextAttribsARBProc = GLXContext (*)(Display *, GLXFBConfig, GLXContext, Bool,
                                                      const int *);
using glXBindTexImageEXTProc = void (*)(Display *, GLXDrawable, int, const int *);
using glXReleaseTexImageEXTProc = void (*)(Display *, GLXDrawable, int);
using glXSwapIntervalEXTProc = void (*)(Display *, GLXDrawable, int);

/**
 * WindowTexture - Represents an OpenGL texture for a window
 */
class WindowTexture {
  public:
    WindowTexture() = default;
    ~WindowTexture() = default;

    // X11 pixmap data
    Pixmap xPixmap = 0;
    GLXPixmap glxPixmap = 0;

    // OpenGL data
    GLuint textureId = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    int indexCount = 0;

    // Damage tracking
    Damage damage = 0;
    bool needsUpdate = true;
};

/**
 * Compositor - Manages OpenGL-based window compositing
 */
class Compositor {
  public:
    /**
     * Constructor - Sets up the compositing environment
     */
    explicit Compositor(WindowManager &windowManager);

    /**
     * Destructor - Cleans up resources
     */
    ~Compositor();

    // Prevent copying and assignment
    Compositor(const Compositor &) = delete;
    Compositor &operator=(const Compositor &) = delete;

    /**
     * Render a frame with all visible windows
     * @return Time delta in seconds since the last frame
     */
    float renderFrame();

    /**
     * Enable/disable vertical sync
     */
    void setVSync(bool enabled);

    /**
     * Check if VSync is enabled
     */
    bool isVSyncEnabled() const {
        return m_vsync;
    }

    /**
     * Get the XDamage event base
     */
    int getDamageEventBase() const {
        return m_damageEventBase;
    }

    /**
     * Handle window events
     */
    void handleWindowCreated(Window xWindow);
    void handleWindowModified(Window xWindow);
    void handleWindowDestroyed(Window xWindow);

    /**
     * Handle damage events
     */
    void handleDamageEvent(XDamageNotifyEvent *event);

  private:
    // Window rendering methods
    void renderWindow(Window xWindow, int zIndex, int windowCount, float deltaTime);
    void renderWindowShadow(int zIndex, int windowCount, float x, float y, float width,
                            float height, float depth, float deltaTime);

    // OpenGL shader creation helpers
    GLuint createShaderProgram(const std::string_view &vertexSource,
                               const std::string_view &fragmentSource);
    GLuint compileShader(GLenum type, const std::string_view &source);
    void createGeometryBuffers(GLuint &vao, GLuint &vbo, GLuint &ibo);
    void updateBufferData(GLuint vao, GLuint vbo, GLsizeiptr vboSize, const void *vboData,
                          GLuint ibo, GLsizeiptr iboSize, const void *iboData);

    // Window texture management
    WindowTexture *getWindowTexture(Window xWindow);
    void bindWindowTexture(Window xWindow);
    void unbindWindowTexture(Window xWindow);
    void freeWindowPixmaps(Window xWindow);
    void freeWindowTextureResources(Window xWindow);
    void deleteOpenGLTexture(Window xWindow);
    void createWindowGeometry(WindowTexture *texture, int width, int height);

    // X11 compositing setup
    void setupCompositeOverlay();
    void setupGLXContext();

    // Damage tracking
    void setupDamageTracking(Window xWindow);
    void cleanupDamageTracking(Window xWindow);

    // Reference to the window manager
    WindowManager &m_windowManager;

    // X11 windows for compositing
    Window m_overlayWindow = 0;
    Window m_outputWindow = 0;
    Window m_screenOwner = 0;

    // OpenGL/GLX state
    GLXContext m_glxContext = nullptr;
    int m_glxConfigCount = 0;
    GLXFBConfig *m_glxConfigs = nullptr;

    // GLX function pointers
    glXBindTexImageEXTProc m_glXBindTexImageEXT = nullptr;
    glXReleaseTexImageEXTProc m_glXReleaseTexImageEXT = nullptr;

    // Window texture storage
    std::unordered_map<Window, WindowTexture> m_windowTextures;

    // Timing
    std::chrono::time_point<std::chrono::steady_clock> m_lastFrameTime;
    bool m_vsync = true;

    // Window shaders
    GLuint m_windowShader = 0;
    GLuint m_textureUniform = 0;
    GLuint m_opacityUniform = 0;
    GLuint m_depthUniform = 0;
    GLuint m_positionUniform = 0;
    GLuint m_sizeUniform = 0;

    // Shadow shaders
    GLuint m_shadowShader = 0;
    GLuint m_shadowVAO = 0;
    GLuint m_shadowVBO = 0;
    GLuint m_shadowIBO = 0;
    int m_shadowIndexCount = 0;
    GLuint m_shadowStrengthUniform = 0;
    GLuint m_shadowDepthUniform = 0;
    GLuint m_shadowPositionUniform = 0;
    GLuint m_shadowSizeUniform = 0;
    GLuint m_shadowSpreadUniform = 0;

    // Constants
    static constexpr int CORNER_RADIUS = 3; // pixels
    static constexpr int CORNER_RESOLUTION = 8;
    static constexpr double TAU = 6.283185307179586;

    // XDamage extension
    int m_damageEventBase = 0;
};

} // namespace ferret
