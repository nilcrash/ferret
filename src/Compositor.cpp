/**
 * Ferret Window Manager
 * A modern C++23 compositing window manager based on X11 and OpenGL
 *
 * Compositor.cpp - Implementation of OpenGL-based window compositing
 */

#include "Compositor.h"
#include <spdlog/spdlog.h>

namespace ferret {

// GLX extension constants
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092

// Default vertex and fragment shaders for window rendering
static constexpr const char *DEFAULT_VERTEX_SHADER = R"(#version 330
    layout(location = 0) in vec2 vertex_position;
    out vec2 tex_coord;

    uniform float depth;
    uniform vec2 position;
    uniform vec2 size;

    void main(void) {
        // Map vertex position to texture coordinates in 0-1 range
        tex_coord = vertex_position + 0.5;
        
        // Use vertex position directly with size and position
        // Since vertices are now in -0.5 to 0.5 range, this works correctly
        gl_Position = vec4(vertex_position * size + position, depth, 1.0);
    })";

static constexpr const char *DEFAULT_FRAGMENT_SHADER = R"(#version 330
    in vec2 tex_coord;
    out vec4 fragment_colour;

    uniform float opacity;
    uniform sampler2D texture_sampler;

    void main(void) {
        // Fix texture coordinates - ensure proper mapping by flipping Y
        vec2 corrected_coord = vec2(tex_coord.x, 1.0 - tex_coord.y);
        
        // Sample texture and apply opacity
        fragment_colour = texture(texture_sampler, corrected_coord);
    })";

// Shadow effect shaders
static constexpr const char *SHADOW_VERTEX_SHADER = R"(#version 330
    layout(location = 0) in vec2 vertex_position;
    out vec2 map_position;

    uniform float depth;
    uniform vec2 position;
    uniform vec2 size;
    uniform vec2 spread;

    void main(void) {
        map_position = vertex_position * (size + spread);
        gl_Position = vec4(map_position + position, depth, 1.0);
    })";

static constexpr const char *SHADOW_FRAGMENT_SHADER = R"(#version 330
    in vec2 map_position;
    out vec4 fragment_colour;

    uniform float strength;
    uniform vec2 size;
    uniform vec2 spread;

    void main(void) {
        float dx = (2 * abs(map_position.x) - size.x + spread.x / 8) / spread.x;
        float dy = (2 * abs(map_position.y) - size.y + spread.y / 8) / spread.y;

        if (map_position.y > 0) dy *= 1.5;
        if (map_position.y < 0) dy /= 1.2;

        dx = clamp(dx, 0, 1);
        dy = clamp(dy, 0, 1);

        float value = 1.0 - clamp(length(vec2(dx, dy)), 0, 1);
        fragment_colour = vec4(0.0, 0.0, 0.0, value * value) * strength;
    })";

Compositor::Compositor(WindowManager &windowManager)
    : m_windowManager(windowManager), m_lastFrameTime(std::chrono::steady_clock::now()) {

    SPDLOG_DEBUG("Initializing Compositor");

    // Set up X11 compositing overlay
    setupCompositeOverlay();

    // Set up GLX and OpenGL context
    setupGLXContext();

    // Create window shader program
    m_windowShader = createShaderProgram(DEFAULT_VERTEX_SHADER, DEFAULT_FRAGMENT_SHADER);
    m_textureUniform = glGetUniformLocation(m_windowShader, "texture_sampler");
    m_opacityUniform = glGetUniformLocation(m_windowShader, "opacity");
    m_depthUniform = glGetUniformLocation(m_windowShader, "depth");
    m_positionUniform = glGetUniformLocation(m_windowShader, "position");
    m_sizeUniform = glGetUniformLocation(m_windowShader, "size");

    // Create shadow shader program
    m_shadowShader = createShaderProgram(SHADOW_VERTEX_SHADER, SHADOW_FRAGMENT_SHADER);
    m_shadowStrengthUniform = glGetUniformLocation(m_shadowShader, "strength");
    m_shadowDepthUniform = glGetUniformLocation(m_shadowShader, "depth");
    m_shadowPositionUniform = glGetUniformLocation(m_shadowShader, "position");
    m_shadowSizeUniform = glGetUniformLocation(m_shadowShader, "size");
    m_shadowSpreadUniform = glGetUniformLocation(m_shadowShader, "spread");

    // Create shadow geometry
    createGeometryBuffers(m_shadowVAO, m_shadowVBO, m_shadowIBO);

    const GLubyte shadowIndices[] = {0, 1, 2, 0, 2, 3};
    const GLfloat shadowVertexPositions[] = {-0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f};

    m_shadowIndexCount = sizeof(shadowIndices) / sizeof(shadowIndices[0]);
    updateBufferData(m_shadowVAO, m_shadowVBO, sizeof(shadowVertexPositions), shadowVertexPositions,
                     m_shadowIBO, sizeof(shadowIndices), shadowIndices);

    // Enable OpenGL features
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    SPDLOG_INFO("Compositor initialized");
}

Compositor::~Compositor() {
    SPDLOG_DEBUG("Shutting down Compositor");

    // Clean up window textures
    for (auto const& [window, texture] : m_windowTextures) {
        freeWindowTextureResources(window); // Free buffers/pixmaps
        deleteOpenGLTexture(window);      // Free texture ID
    }
    m_windowTextures.clear();

    // Delete shadow buffers
    if (m_shadowVAO) {
        glDeleteVertexArrays(1, &m_shadowVAO);
        m_shadowVAO = 0;
    }
    if (m_shadowVBO) {
        glDeleteBuffers(1, &m_shadowVBO);
        m_shadowVBO = 0;
    }
    if (m_shadowIBO) {
        glDeleteBuffers(1, &m_shadowIBO);
        m_shadowIBO = 0;
    }

    // Delete shader programs
    if (m_windowShader) {
        glDeleteProgram(m_windowShader);
        m_windowShader = 0;
    }
    if (m_shadowShader) {
        glDeleteProgram(m_shadowShader);
        m_shadowShader = 0;
    }

    // Destroy GLX context and resources
    if (m_glxContext) {
        Display *display = m_windowManager.getDisplay();
        glXMakeCurrent(display, None, nullptr);
        glXDestroyContext(display, m_glxContext);
        m_glxContext = nullptr;
    }

    if (m_glxConfigs) {
        XFree(m_glxConfigs);
        m_glxConfigs = nullptr;
    }

    // Free X11 resources
    if (m_outputWindow) {
        XDestroyWindow(m_windowManager.getDisplay(), m_outputWindow);
        m_outputWindow = 0;
    }

    if (m_overlayWindow) {
        XCompositeReleaseOverlayWindow(m_windowManager.getDisplay(), m_overlayWindow);
        m_overlayWindow = 0;
    }

    if (m_screenOwner) {
        XDestroyWindow(m_windowManager.getDisplay(), m_screenOwner);
        m_screenOwner = 0;
    }

    SPDLOG_INFO("Compositor shut down");
}

void Compositor::setupCompositeOverlay() {
    Display *display = m_windowManager.getDisplay();
    Window rootWindow = m_windowManager.getRootWindow();

    // Register as a compositing window manager with proper window name
    m_screenOwner = XCreateSimpleWindow(display, rootWindow, 0, 0, 1, 1, 0, 0, 0);
    Xutf8SetWMProperties(display, m_screenOwner, "xcompmgr", "xcompmgr", nullptr, 0, nullptr,
                         nullptr, nullptr);

    // Create atom name specific to the screen
    char cmAtomName[32];
    snprintf(cmAtomName, sizeof(cmAtomName), "_NET_WM_CM_S%d", DefaultScreen(display));

    Atom cmAtom = XInternAtom(display, cmAtomName, False);
    XSetSelectionOwner(display, cmAtom, m_screenOwner, 0);
    
    // Make sure it worked
    Window ownerWindow = XGetSelectionOwner(display, cmAtom);
    if (ownerWindow != m_screenOwner) {
        SPDLOG_ERROR("Failed to set compositor selection owner: expected 0x{:x}, got 0x{:x}", 
                    m_screenOwner, ownerWindow);
    } else {
        SPDLOG_INFO("Successfully set window 0x{:x} as compositor selection owner for atom {}", 
                   m_screenOwner, cmAtomName);
    }

    // Redirect subwindows - CRITICAL: using CompositeRedirectAutomatic for reliable compositing
    SPDLOG_INFO("Setting window redirection mode: CompositeRedirectAutomatic");
    XCompositeRedirectSubwindows(display, rootWindow, CompositeRedirectAutomatic);
    
    // Flush X requests to ensure redirection takes effect
    XSync(display, False);
    SPDLOG_INFO("Applied compositing redirection to root window 0x{:x}", rootWindow);

    // Get overlay window
    m_overlayWindow = XCompositeGetOverlayWindow(display, rootWindow);
    SPDLOG_INFO("Created composite overlay window: 0x{:x}", m_overlayWindow);

    // Make overlay transparent to input events - exactly like simplewm
    XserverRegion region = XFixesCreateRegion(display, nullptr, 0);
    XFixesSetWindowShapeRegion(display, m_overlayWindow, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(display, region);
    SPDLOG_INFO("Made overlay window transparent to input events");

    // Add overlay and screen owner to blacklist for event handling
    m_windowManager.addEventBlacklistedWindow(m_overlayWindow);
    m_windowManager.addEventBlacklistedWindow(m_screenOwner);
}

void Compositor::setupGLXContext() {
    Display *display = m_windowManager.getDisplay();
    Window rootWindow = m_windowManager.getRootWindow();
    int screen = DefaultScreen(display);

    // Get GLX frame buffer configurations
    SPDLOG_DEBUG("Selecting GLX frame buffer configurations");
    const int configAttributes[] = {
        GLX_BIND_TO_TEXTURE_RGBA_EXT, 1,
        GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_X_RENDERABLE, 1,
        GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT, static_cast<int>(GLX_DONT_CARE),
        GLX_BUFFER_SIZE, 32,
        GLX_DOUBLEBUFFER, 1,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_STENCIL_SIZE, 0,
        GLX_DEPTH_SIZE, 16,
        None
    };

    m_glxConfigs = glXChooseFBConfig(display, screen, configAttributes, &m_glxConfigCount);
    if (!m_glxConfigs || m_glxConfigCount == 0) {
        throw std::runtime_error("Failed to get GLX frame buffer configurations");
    }
    SPDLOG_INFO("Found {} GLX frame buffer configurations", m_glxConfigCount);

    // Get visual info from the first FB config
    XVisualInfo *visualInfo = glXGetVisualFromFBConfig(display, m_glxConfigs[0]);
    if (!visualInfo) {
        XFree(m_glxConfigs);
        throw std::runtime_error("Failed to get visual from FB config");
    }
    SPDLOG_INFO("Using visual ID 0x{:x} with depth {}", visualInfo->visualid, visualInfo->depth);

    // Create output window
    XSetWindowAttributes windowAttributes;
    windowAttributes.colormap = XCreateColormap(display, rootWindow, visualInfo->visual, AllocNone);
    windowAttributes.border_pixel = 0;

    int width = m_windowManager.getScreenWidth();
    int height = m_windowManager.getScreenHeight();

    SPDLOG_DEBUG("Creating output window with dimensions {}x{}", width, height);
    m_outputWindow =
        XCreateWindow(display, rootWindow, 0, 0, width, height, 0, visualInfo->depth, InputOutput,
                      visualInfo->visual, CWBorderPixel | CWColormap, &windowAttributes);

    if (!m_outputWindow) {
        XFree(visualInfo);
        XFree(m_glxConfigs);
        throw std::runtime_error("Failed to create output window");
    }

    XReparentWindow(display, m_outputWindow, m_overlayWindow, 0, 0);
    XMapRaised(display, m_outputWindow);

    // Add output window to event blacklist
    m_windowManager.addEventBlacklistedWindow(m_outputWindow);

    // Create OpenGL 3.3 context
    SPDLOG_DEBUG("Creating OpenGL 3.3 core profile context");
    const int contextAttributes[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
        None
    };

    // Get extension functions
    glXCreateContextAttribsARBProc glXCreateContextAttribsARB =
        reinterpret_cast<glXCreateContextAttribsARBProc>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte *>("glXCreateContextAttribsARB")));

    if (!glXCreateContextAttribsARB) {
        XFree(visualInfo);
        XFree(m_glxConfigs);
        throw std::runtime_error("Failed to get glXCreateContextAttribsARB function");
    }

    // Try to create OpenGL 3.3 context
    m_glxContext =
        glXCreateContextAttribsARB(display, m_glxConfigs[0], nullptr, True, contextAttributes);
    if (!m_glxContext) {
        SPDLOG_WARN("Failed to create OpenGL 3.3 context, falling back to default context");
        m_glxContext = glXCreateNewContext(display, m_glxConfigs[0], GLX_RGBA_TYPE, nullptr, True);
        if (!m_glxContext) {
            XFree(visualInfo);
            XFree(m_glxConfigs);
            throw std::runtime_error("Failed to create any OpenGL context");
        }
    }

    // Get texture functions
    m_glXBindTexImageEXT = reinterpret_cast<glXBindTexImageEXTProc>(
        glXGetProcAddress(reinterpret_cast<const GLubyte *>("glXBindTexImageEXT")));

    m_glXReleaseTexImageEXT = reinterpret_cast<glXReleaseTexImageEXTProc>(
        glXGetProcAddress(reinterpret_cast<const GLubyte *>("glXReleaseTexImageEXT")));

    if (!m_glXBindTexImageEXT || !m_glXReleaseTexImageEXT) {
        XFree(visualInfo);
        XFree(m_glxConfigs);
        glXDestroyContext(display, m_glxContext);
        throw std::runtime_error("Failed to get GLX extension functions");
    }
    SPDLOG_INFO("Successfully loaded GLX texture extension functions");

    // Make context current
    if (!glXMakeCurrent(display, m_outputWindow, m_glxContext)) {
        XFree(visualInfo);
        XFree(m_glxConfigs);
        glXDestroyContext(display, m_glxContext);
        throw std::runtime_error("Failed to make GLX context current");
    }

    // Initialize GLEW
    SPDLOG_DEBUG("Initializing GLEW");
    glewExperimental = GL_TRUE;
    GLenum glewResult = glewInit();
    if (glewResult != GLEW_OK) {
        XFree(visualInfo);
        XFree(m_glxConfigs);
        glXDestroyContext(display, m_glxContext);
        throw std::runtime_error("Failed to initialize GLEW");
    }
    
    // Clear any GLEW initialization errors
    while (glGetError() != GL_NO_ERROR) {
        // GLEW initialization can sometimes produce an GL_INVALID_ENUM error which can be safely ignored
    }

    // Print OpenGL context info
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* glslVersion = glGetString(GL_SHADING_LANGUAGE_VERSION);
    SPDLOG_INFO("OpenGL Context: Version: {}, Vendor: {}, Renderer: {}, GLSL: {}", 
                version ? (const char*)version : "unknown",
                vendor ? (const char*)vendor : "unknown", 
                renderer ? (const char*)renderer : "unknown",
                glslVersion ? (const char*)glslVersion : "unknown");

    // Check for any OpenGL errors after initialization
    GLenum err;
    if ((err = glGetError()) != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error after context initialization: {}", err);
    }

    // Initialize Damage extension
    int damageError;
    if (!XDamageQueryExtension(m_windowManager.getDisplay(), &m_damageEventBase, &damageError)) {
        SPDLOG_ERROR("XDamage extension not available");
    } else {
        SPDLOG_INFO("XDamage extension initialized with event base {}", m_damageEventBase);
    }

    // Clean up
    XFree(visualInfo);
}

float Compositor::renderFrame() {
    SPDLOG_DEBUG("Starting frame render");

    // Make sure we have a current OpenGL context
    Display *display = m_windowManager.getDisplay();
    if (!glXGetCurrentContext()) {
        if (!glXMakeCurrent(display, m_outputWindow, m_glxContext)) {
            SPDLOG_ERROR("Failed to make OpenGL context current");
            return 0.0f;
        }
    }

    // Clear the frame
    glClearColor(0.16f, 0.16f, 0.16f, 1.0f); // Dark gray background
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Set viewport
    int width = m_windowManager.getScreenWidth();
    int height = m_windowManager.getScreenHeight();
    glViewport(0, 0, width, height);
    SPDLOG_DEBUG("Viewport set to {}x{}", width, height);

    // Calculate time delta
    auto currentTime = std::chrono::steady_clock::now();
    float deltaTime = std::chrono::duration<float>(currentTime - m_lastFrameTime).count();
    m_lastFrameTime = currentTime;

    // Get all windows to render
    const auto &managedWindows = m_windowManager.getManagedWindows();
    int windowCount = managedWindows.size();
    
    // Log visible window count
    int visibleCount = 0;
    for (const auto &window : managedWindows) {
        if (window.isVisible()) {
            visibleCount++;
        }
    }
    SPDLOG_DEBUG("Rendering frame with {} windows ({} visible)", windowCount, visibleCount);

    // Check for OpenGL errors before rendering
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error before rendering: {}", error);
    }

    // Enable blending and disable depth testing for 2D compositing
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Render each window
    int index = 0;
    for (const auto &window : managedWindows) {
        if (window.isVisible()) {
            Window xWindow = window.getXWindow();
            
            // Double check window existence and validity
            XWindowAttributes attribs;
            Status status = XGetWindowAttributes(m_windowManager.getDisplay(), xWindow, &attribs);
            if (status == 0) {
                SPDLOG_WARN("Window 0x{:x} appears to be invalid, skipping render", xWindow);
                continue;
            }
            
            if (attribs.map_state != IsViewable) {
                SPDLOG_WARN("Window 0x{:x} is not viewable (map_state={}), marking as requiring update", 
                          xWindow, attribs.map_state);
                // Force an update of the window next time around
                handleWindowModified(xWindow);
                continue;
            }
            
            SPDLOG_DEBUG("Rendering window 0x{:x} at ({}, {}) size {}x{}", xWindow,
                         window.getX(), window.getY(), window.getWidth(), window.getHeight());

            // Skip windows with invalid dimensions
            if (window.getWidth() <= 0 || window.getHeight() <= 0) {
                SPDLOG_WARN("Window 0x{:x} has invalid dimensions: {}x{}, skipping render", 
                           xWindow, window.getWidth(), window.getHeight());
                continue;
            }
            
            renderWindow(xWindow, index, windowCount, deltaTime);
            
            // Check error immediately after rendering this window
            GLenum error = glGetError();
            if (error != GL_NO_ERROR) {
                SPDLOG_ERROR("OpenGL error after rendering window 0x{:x}: {}", xWindow, error);
            }
            
            index++;
        } else {
            SPDLOG_TRACE("Window 0x{:x} is not visible, skipping render", window.getXWindow());
        }
    }

    // Draw a debug marker in a corner to indicate renderer is working
    static bool debugMarkerInitialized = false;
    static GLuint debugVAO = 0, debugVBO = 0, debugIBO = 0;
    static GLuint debugShader = 0;
    
    if (!debugMarkerInitialized) {
        // Create a small marker in the top-right corner
        glGenVertexArrays(1, &debugVAO);
        glBindVertexArray(debugVAO);
        
        glGenBuffers(1, &debugVBO);
        glBindBuffer(GL_ARRAY_BUFFER, debugVBO);
        
        const GLfloat markerVertices[] = {
            0.95f, 0.95f,  // Top-right
            0.99f, 0.95f,  // Top-right
            0.99f, 0.99f,  // Bottom-right 
            0.95f, 0.99f   // Bottom-right
        };
        
        glBufferData(GL_ARRAY_BUFFER, sizeof(markerVertices), markerVertices, GL_STATIC_DRAW);
        
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (void *)0);
        glEnableVertexAttribArray(0);
        
        glGenBuffers(1, &debugIBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, debugIBO);
        
        const GLuint markerIndices[] = {
            0, 1, 2,  // First triangle
            0, 2, 3   // Second triangle
        };
        
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(markerIndices), markerIndices, GL_STATIC_DRAW);
        
        // Create a simple shader for the marker
        const char* markerVertSrc = R"(#version 330 core
            layout(location = 0) in vec2 aPos;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
            })";
            
        const char* markerFragSrc = R"(#version 330 core
            out vec4 FragColor;
            void main() {
                FragColor = vec4(0.0, 1.0, 0.0, 1.0); // Bright green
            })";
            
        debugShader = createShaderProgram(markerVertSrc, markerFragSrc);
        debugMarkerInitialized = true;
    }
    
    // Draw the debug marker
    glUseProgram(debugShader);
    glBindVertexArray(debugVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Unbind VAO
    glBindVertexArray(0);

    // Swap buffers
    glXSwapBuffers(display, m_outputWindow);
    SPDLOG_DEBUG("Frame rendered and buffers swapped");

    return deltaTime;
}

void Compositor::renderWindow(Window xWindow, int zIndex, int windowCount, float deltaTime) {
    SPDLOG_DEBUG("Rendering window 0x{:x}", xWindow);

    // First check if the window is still valid
    const ManagedWindow *window = m_windowManager.findWindowByXID(xWindow);
    if (!window || !window->isVisible()) {
        SPDLOG_DEBUG("Window 0x{:x} not found or not visible, skipping render", xWindow);
        return;
    }

    // Get window texture
    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture) {
        SPDLOG_ERROR("No texture found for window 0x{:x}", xWindow);
        return;
    }

    // Check and create geometry if missing
    if (!texture->vao || !texture->vbo || !texture->ibo) {
        SPDLOG_WARN("Window 0x{:x} geometry buffers missing or invalid (VAO={}, VBO={}, IBO={}). Creating now.", 
                    xWindow, texture->vao, texture->vbo, texture->ibo);
        createWindowGeometry(texture, window->getWidth(), window->getHeight());
        
        // Check again after creation attempt
        if (!texture->vao || !texture->vbo || !texture->ibo) {
            SPDLOG_ERROR("Failed to create geometry buffers for window 0x{:x} during render. Skipping.", xWindow);
            return; // Cannot render without geometry
        }
        SPDLOG_INFO("Successfully created geometry buffers for window 0x{:x} during render.", xWindow);
    }
    
    // Log the current state of the window texture (should be valid now)
    SPDLOG_DEBUG("Window 0x{:x} texture state: textureId={}, xPixmap=0x{:x}, glxPixmap=0x{:x}, VAO={}, VBO={}, IBO={}",
               xWindow, texture->textureId, texture->xPixmap, texture->glxPixmap, 
               texture->vao, texture->vbo, texture->ibo);

    // Log more detailed checking of texture state -> This check is now handled above
    /* if (!texture->vao || !texture->vbo || !texture->ibo) { ... } */

    // Calculate window properties in normalized device coordinates (-1 to 1)
    float x = m_windowManager.xCoordinateToFloat(window->getX() + window->getWidth() / 2);
    float y = m_windowManager.yCoordinateToFloat(window->getY() + window->getHeight() / 2);
    float width = m_windowManager.widthDimensionToFloat(window->getWidth());
    float height = m_windowManager.heightDimensionToFloat(window->getHeight());

    SPDLOG_DEBUG("Window 0x{:x} properties: x={:.4f}, y={:.4f}, width={:.4f}, height={:.4f}",
                xWindow, x, y, width, height);
    SPDLOG_DEBUG("Window 0x{:x} original pixel dimensions: {}x{} at ({},{})",
                xWindow, window->getWidth(), window->getHeight(),
                window->getX(), window->getY());

    // Calculate window depth (using zIndex for z-ordering)
    float depth = 1.0f - static_cast<float>(zIndex) / windowCount;

    // Clear errors before starting render sequence
    GLenum prevError = glGetError();
    if (prevError != GL_NO_ERROR) {
        SPDLOG_WARN("Existing OpenGL error before rendering window 0x{:x}: {}", xWindow, prevError);
    }

    // IMPORTANT: Render shadow first (behind the window)
    renderWindowShadow(zIndex, windowCount, x, y, width, height, depth, deltaTime);

    // **** CRITICAL SECTION: Render window content ****
    SPDLOG_DEBUG("Starting critical window render section for window 0x{:x}", xWindow);
    
    // 1. Activate texture unit 0 for the window texture
    glActiveTexture(GL_TEXTURE0);
    SPDLOG_DEBUG("Activated texture unit GL_TEXTURE0");
    
    // 2. Reset the binding to nothing to avoid state conflicts
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // 3. Bind the window texture via GLX
    bindWindowTexture(xWindow);
    
    // 4. Check if texture binding succeeded
    GLint boundTextureID = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTextureID);
    SPDLOG_DEBUG("Current GL_TEXTURE_BINDING_2D after bindWindowTexture = {}", boundTextureID);
    
    if (boundTextureID == 0) {
        SPDLOG_ERROR("No texture was bound after bindWindowTexture for window 0x{:x}", xWindow);
    } else if (boundTextureID != texture->textureId) {
        SPDLOG_ERROR("Wrong texture was bound: expected {}, got {}", texture->textureId, boundTextureID);
    }

    // 5. Use window shader program
    glUseProgram(m_windowShader);
    SPDLOG_DEBUG("Using window shader program {}", m_windowShader);

    // 6. Set blend mode for solid rendering regardless of alpha
    glBlendFunc(GL_ONE, GL_ZERO);  // Replace destination with source
    
    // Set texture sampling to nearest for crisp text
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    
    // 7. Set all uniforms explicitly
    glUniform1i(m_textureUniform, 0);  // Texture unit 0
    glUniform1f(m_opacityUniform, 1.0f);
    glUniform1f(m_depthUniform, depth);
    
    // CRITICAL: Set position and size uniforms correctly
    glUniform2f(m_positionUniform, x, y);
    glUniform2f(m_sizeUniform, width, height);
    
    SPDLOG_DEBUG("Set window 0x{:x} uniforms: depth={:.3f}, position=({:.3f},{:.3f}), size=({:.3f},{:.3f})",
                xWindow, depth, x, y, width, height);
    
    // Check for errors before drawing
    GLenum preDrawError = glGetError();
    if (preDrawError != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error before drawing window 0x{:x}: {}", xWindow, preDrawError);
    }

    // 8. Bind the VAO for geometry
    glBindVertexArray(texture->vao);
    SPDLOG_DEBUG("Bound VAO {} for window 0x{:x}", texture->vao, xWindow);
    
    // 9. Draw the window
    glDrawElements(GL_TRIANGLES, texture->indexCount, GL_UNSIGNED_INT, 0);
    SPDLOG_DEBUG("Drew window 0x{:x} with {} indices", xWindow, texture->indexCount);
    
    // 10. Unbind VAO
    glBindVertexArray(0);

    // Check for errors after drawing
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error after drawing window 0x{:x}: {}", xWindow, error);
    } else {
        SPDLOG_DEBUG("Successfully drew window 0x{:x}", xWindow);
    }

    // 11. Unbind window texture
    unbindWindowTexture(xWindow);
    SPDLOG_DEBUG("Completed critical window render section for window 0x{:x}", xWindow);
    // **** END CRITICAL SECTION ****

    // Add fallback drawing - draw with explicit patterns if texture isn't working
    // Set up fallback shader program
    static GLuint fallbackShader = 0;
    if (!fallbackShader) {
        static const char* fallbackVertSrc = R"(#version 330 core
            layout(location = 0) in vec2 aPos;
            out vec2 localPos;
            uniform vec2 uPosition;
            uniform vec2 uSize;
            uniform float uDepth;
            void main() {
                localPos = aPos;
                gl_Position = vec4(aPos * uSize + uPosition, uDepth + 0.01, 1.0);
            })";
            
        static const char* fallbackFragSrc = R"(#version 330 core
            in vec2 localPos;
            out vec4 FragColor;
            void main() {
                // Create a checkerboard pattern
                float freq = 10.0;
                float checker = mod(floor(localPos.x * freq) + floor(localPos.y * freq), 2.0);
                vec3 color = (checker < 0.5) ? vec3(0.9, 0.1, 0.1) : vec3(0.1, 0.1, 0.9);
                // Add a border
                vec2 d = abs(localPos) - vec2(0.95);
                bool isBorder = max(d.x, d.y) > 0.0;
                if (isBorder) color = vec3(1.0, 1.0, 0.0);
                FragColor = vec4(color, 0.75);
            })";
            
        fallbackShader = createShaderProgram(fallbackVertSrc, fallbackFragSrc);
        SPDLOG_INFO("Created fallback pattern shader {}", fallbackShader);
    }
    
    // Draw the fallback pattern a bit offset from the main window
    glUseProgram(fallbackShader);
    
    // Get uniform locations only once
    static GLint fallbackPosLoc = glGetUniformLocation(fallbackShader, "uPosition");
    static GLint fallbackSizeLoc = glGetUniformLocation(fallbackShader, "uSize");
    static GLint fallbackDepthLoc = glGetUniformLocation(fallbackShader, "uDepth");
    
    // Offset position by a fraction
    glUniform2f(fallbackPosLoc, x + width*0.05f, y - height*0.05f);
    glUniform2f(fallbackSizeLoc, width * 0.3f, height * 0.3f);
    glUniform1f(fallbackDepthLoc, depth + 0.02f);
    
    // Use same geometry
    glBindVertexArray(texture->vao);
    glDrawElements(GL_TRIANGLES, texture->indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    
    // Restore window shader for next window
    glUseProgram(m_windowShader);
}

void Compositor::renderWindowShadow(int zIndex, int windowCount, float x, float y, float width,
                                    float height, float depth, float deltaTime) {
    // Make shadow stronger for focused window (zIndex == 0)
    const float shadowOpacity = 0.25f + (zIndex == 0 ? 0.15f : 0.0f);

    // Use shadow shader program
    glUseProgram(m_shadowShader);

    // Shadow spread - higher for focused windows
    const float shadowRadius = 128.0f + (zIndex == 0 ? 64.0f : 0.0f);
    float spreadX = 4.0f * shadowRadius / m_windowManager.getScreenWidth();
    float spreadY = 4.0f * shadowRadius / m_windowManager.getScreenHeight();

    // Shadow is drawn slightly behind the window
    float shadowDepth = depth - 0.01f;
    
    // Add a slight y offset for better visual effect
    float yOffset = -spreadY / 32.0f - (zIndex == 0 ? spreadY / 16.0f : 0.0f);

    // Set shadow uniforms
    glUniform1f(m_shadowStrengthUniform, shadowOpacity);
    glUniform2f(m_shadowSpreadUniform, spreadX, spreadY);
    glUniform1f(m_shadowDepthUniform, shadowDepth);
    glUniform2f(m_shadowPositionUniform, x, y + yOffset);
    glUniform2f(m_shadowSizeUniform, width, height);

    // Draw shadow
    glBindVertexArray(m_shadowVAO);
    glDrawElements(GL_TRIANGLES, m_shadowIndexCount, GL_UNSIGNED_BYTE, nullptr);
    glBindVertexArray(0); // Unbind VAO
    
    // Check for errors after drawing shadow
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error after shadow drawing: {}", error);
    }
}

void Compositor::handleWindowCreated(Window xWindow) {
    SPDLOG_DEBUG("Creating compositor resources for window 0x{:x}", xWindow);

    // Get window attributes to log information
    XWindowAttributes attribs;
    if (XGetWindowAttributes(m_windowManager.getDisplay(), xWindow, &attribs)) {
        SPDLOG_INFO("Window created 0x{:x}: size={}x{}, depth={}, visual=0x{:x}, mapped={}",
                   xWindow, attribs.width, attribs.height, attribs.depth,
                   XVisualIDFromVisual(attribs.visual),
                   (attribs.map_state == IsViewable ? "yes" : "no"));
    }

    // Make sure we have a current OpenGL context before doing any GL operations
    if (!glXGetCurrentContext()) {
        if (!glXMakeCurrent(m_windowManager.getDisplay(), m_outputWindow, m_glxContext)) {
            SPDLOG_ERROR("Failed to make OpenGL context current in handleWindowCreated");
            return;
        }
    }

    // Create window texture entry - initially empty
    WindowTexture texture = {};
    m_windowTextures[xWindow] = texture;
    SPDLOG_INFO("Created initial texture entry for window 0x{:x} (geometry will be created on modify/render)", xWindow);

    // Geometry will be created by renderWindow when first needed.
    // No need to force renderFrame here.

    // Set up damage tracking
    setupDamageTracking(xWindow);
}

void Compositor::handleWindowModified(Window xWindow) {
    SPDLOG_DEBUG("Handling modification for window 0x{:x}. Invalidating pixmaps.", xWindow);

    // Ensure context is current before GL operations in freeWindowPixmaps might occur indirectly
    if (!glXGetCurrentContext()) {
        if (!glXMakeCurrent(m_windowManager.getDisplay(), m_outputWindow, m_glxContext)) {
            SPDLOG_ERROR("Failed to make OpenGL context current in handleWindowModified");
            // Attempt to continue to free X resources if possible
        }
    }

    // Free only the pixmaps. Texture ID and geometry remain.
    // Geometry will be recreated/updated by renderWindow if necessary.
    freeWindowPixmaps(xWindow);

    // No need to force renderFrame here, main loop handles rendering based on events.
}

void Compositor::handleWindowDestroyed(Window xWindow) {
    SPDLOG_INFO("Destroying compositor resources for window 0x{:x}", xWindow);

    // Clean up damage tracking
    cleanupDamageTracking(xWindow);
    
    // Free window GL buffers and pixmaps
    freeWindowTextureResources(xWindow);
    // Explicitly delete the OpenGL texture object
    deleteOpenGLTexture(xWindow);

    // Remove from texture map
    m_windowTextures.erase(xWindow);
    
    // Force a render frame to update the display
    renderFrame();
}

void Compositor::setVSync(bool enabled) {
    m_vsync = enabled;

    // Try to set VSync mode if supported
    glXSwapIntervalEXTProc glXSwapIntervalEXT = reinterpret_cast<glXSwapIntervalEXTProc>(
        glXGetProcAddress(reinterpret_cast<const GLubyte *>("glXSwapIntervalEXT")));

    if (glXSwapIntervalEXT) {
        glXSwapIntervalEXT(m_windowManager.getDisplay(), m_outputWindow, enabled ? 1 : 0);
        SPDLOG_INFO("VSync {}", enabled ? "enabled" : "disabled");
    } else {
        SPDLOG_WARN("VSync control not supported by this driver");
    }
}

WindowTexture *Compositor::getWindowTexture(Window xWindow) {
    auto it = m_windowTextures.find(xWindow);
    if (it == m_windowTextures.end()) {
        // Create texture entry if it doesn't exist
        WindowTexture texture = {};
        m_windowTextures[xWindow] = texture;
        return &m_windowTextures[xWindow];
    }
    return &it->second;
}

void Compositor::bindWindowTexture(Window xWindow) {
    if (!m_glXBindTexImageEXT) {
        SPDLOG_ERROR("glXBindTexImageEXT extension not available");
        return;
    }

    // Ensure texture unit 0 is active
    glActiveTexture(GL_TEXTURE0);

    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture) {
        SPDLOG_ERROR("Failed to get texture for window 0x{:x}", xWindow);
        return;
    }

    // Synchronize X11 to ensure we have the latest window state
    XSync(m_windowManager.getDisplay(), False);
    
    // Use XGetWindowAttributes to check window validity
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(m_windowManager.getDisplay(), xWindow, &attrs)) {
        SPDLOG_ERROR("Window 0x{:x} is not a valid X window during texture binding", xWindow);
        return;
    }
    
    SPDLOG_DEBUG("Window 0x{:x} attributes: width={}, height={}, depth={}, map_state={}", 
                xWindow, attrs.width, attrs.height, attrs.depth, attrs.map_state);

    // Grab server to ensure atomic operations
    XGrabServer(m_windowManager.getDisplay());
    
    // Check if we need to update the pixmap
    bool needsNewPixmap = texture->needsUpdate || !texture->glxPixmap;
    
    if (needsNewPixmap) {
        SPDLOG_DEBUG("Window 0x{:x} needs texture update", xWindow);
        
        // Release previous texture binding
        if (texture->glxPixmap) {
            if (texture->textureId != 0) {
                glBindTexture(GL_TEXTURE_2D, texture->textureId);
                m_glXReleaseTexImageEXT(m_windowManager.getDisplay(), texture->glxPixmap, GLX_FRONT_LEFT_EXT);
            }
            
            // Free GLX pixmap
            glXDestroyPixmap(m_windowManager.getDisplay(), texture->glxPixmap);
            texture->glxPixmap = 0;
            
            // Free X pixmap
            XFreePixmap(m_windowManager.getDisplay(), texture->xPixmap);
            texture->xPixmap = 0;
        }
        
        // Try to find a matching FBConfig - use direct visual ID match
        GLXFBConfig config = nullptr;
        
        // IMPORTANT: Force RGB format, avoid RGBA which can cause sampling issues
        // Use GLX_TEXTURE_FORMAT_RGB_EXT for classic RGB format
        int format = GLX_TEXTURE_FORMAT_RGB_EXT;
        
        VisualID windowVisID = XVisualIDFromVisual(attrs.visual);
        
        // First try to find an exact visual match
        bool foundMatch = false;
        for (int i = 0; i < m_glxConfigCount; i++) {
            XVisualInfo* vInfo = glXGetVisualFromFBConfig(m_windowManager.getDisplay(), m_glxConfigs[i]);
            if (vInfo) {
                if (vInfo->visualid == windowVisID) {
                    config = m_glxConfigs[i];
                    foundMatch = true;
                    XFree(vInfo);
                    break;
                }
                XFree(vInfo);
            }
        }
        
        // If no exact match, try to find any compatible config
        if (!foundMatch) {
            for (int i = 0; i < m_glxConfigCount; i++) {
                int depth, visualId;
                
                glXGetFBConfigAttrib(m_windowManager.getDisplay(), m_glxConfigs[i], GLX_DEPTH_SIZE, &depth);
                glXGetFBConfigAttrib(m_windowManager.getDisplay(), m_glxConfigs[i], GLX_VISUAL_ID, &visualId);
                
                // Check for compatible depth
                if (depth == attrs.depth) {
                    config = m_glxConfigs[i];
                    break;
                }
            }
        }
        
        if (!config) {
            SPDLOG_ERROR("No matching FBConfig found for window 0x{:x}", xWindow);
            XUngrabServer(m_windowManager.getDisplay());
            return;
        }
        
        // Define pixmap attributes for RGB format
        const int pixmapAttribs[] = { 
            GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
            GLX_TEXTURE_FORMAT_EXT, format,
            None 
        };
        
        // Try to create a name pixmap
        texture->xPixmap = XCompositeNameWindowPixmap(m_windowManager.getDisplay(), xWindow);
        if (!texture->xPixmap) {
            SPDLOG_ERROR("Failed to create X pixmap for window 0x{:x}", xWindow);
            XUngrabServer(m_windowManager.getDisplay());
            return;
        }
        
        // Create GLX pixmap from X pixmap
        texture->glxPixmap = glXCreatePixmap(m_windowManager.getDisplay(), config, texture->xPixmap, pixmapAttribs);
        if (!texture->glxPixmap) {
            SPDLOG_ERROR("Failed to create GLX pixmap for window 0x{:x}", xWindow);
            XFreePixmap(m_windowManager.getDisplay(), texture->xPixmap);
            texture->xPixmap = 0;
            XUngrabServer(m_windowManager.getDisplay());
            return;
        }
        
        // Reset the update flag
        texture->needsUpdate = false;
    }
    
    // Generate texture if needed
    if (texture->textureId == 0) {
        glGenTextures(1, &texture->textureId);
        SPDLOG_INFO("Generated new texture ID {} for window 0x{:x}", texture->textureId, xWindow);
    }
    
    // Bind the texture
    glBindTexture(GL_TEXTURE_2D, texture->textureId);
    
    // Set minimal parameters - exactly like SimpWM
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    // Clear errors before binding
    while (glGetError() != GL_NO_ERROR) {}
    
    // CRITICAL CALL: Bind the pixmap content to the texture
    SPDLOG_DEBUG("Binding GLX pixmap 0x{:x} to texture {}", texture->glxPixmap, texture->textureId);
    m_glXBindTexImageEXT(m_windowManager.getDisplay(), texture->glxPixmap, GLX_FRONT_LEFT_EXT, NULL);
    
    // Check for errors immediately after
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error after binding texture: {}", error);
    } else {
        SPDLOG_DEBUG("Successfully bound GLX pixmap to texture");
    }
    
    // Keep server grabbed until unbindWindowTexture
}

void Compositor::unbindWindowTexture(Window xWindow) {
    if (!m_glXReleaseTexImageEXT)
        return;

    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture || !texture->glxPixmap) {
        XUngrabServer(m_windowManager.getDisplay());
        return;
    }

    // Unbind texture from GLX pixmap - exactly like SimpWM
    m_glXReleaseTexImageEXT(m_windowManager.getDisplay(), texture->glxPixmap, GLX_FRONT_LEFT_EXT);
    XUngrabServer(m_windowManager.getDisplay());
}

void Compositor::freeWindowTextureResources(Window xWindow) {
    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture)
        return;

    // Release GLX resources
    if (texture->glxPixmap) {
        glXDestroyPixmap(m_windowManager.getDisplay(), texture->glxPixmap);
        texture->glxPixmap = 0;
    }

    // Release X11 resources
    if (texture->xPixmap) {
        XFreePixmap(m_windowManager.getDisplay(), texture->xPixmap);
        texture->xPixmap = 0;
    }

    // Release OpenGL geometry buffers
    if (texture->vao) {
        glDeleteVertexArrays(1, &texture->vao);
        texture->vao = 0;
    }
    if (texture->vbo) {
        glDeleteBuffers(1, &texture->vbo);
        texture->vbo = 0;
    }
    if (texture->ibo) {
        glDeleteBuffers(1, &texture->ibo);
        texture->ibo = 0;
    }
}

void Compositor::freeWindowPixmaps(Window xWindow) {
    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture)
        return;

    // Release GLX resources
    if (texture->glxPixmap) {
        // We might need to release before destroying?
        // unbindWindowTexture(xWindow); // Careful with recursion/state
        glXDestroyPixmap(m_windowManager.getDisplay(), texture->glxPixmap);
        texture->glxPixmap = 0;
    }

    // Release X11 resources
    if (texture->xPixmap) {
        XFreePixmap(m_windowManager.getDisplay(), texture->xPixmap);
        texture->xPixmap = 0;
    }
}

void Compositor::deleteOpenGLTexture(Window xWindow) {
    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture)
        return;

    if (texture->textureId) {
        glDeleteTextures(1, &texture->textureId);
        texture->textureId = 0;
    }
}

void Compositor::createWindowGeometry(WindowTexture *texture, int width, int height) {
    if (!texture) {
        SPDLOG_ERROR("Null texture pointer passed to createWindowGeometry");
        return;
    }
    
    // Ensure valid dimensions
    width = std::max(1, width);
    height = std::max(1, height);
    
    SPDLOG_DEBUG("Creating window geometry with dimensions {}x{}", width, height);

    // Delete any existing geometry resources to prevent leaks
    if (texture->vao) {
        glDeleteVertexArrays(1, &texture->vao);
        texture->vao = 0;
    }
    
    if (texture->vbo) {
        glDeleteBuffers(1, &texture->vbo);
        texture->vbo = 0;
    }
    
    if (texture->ibo) {
        glDeleteBuffers(1, &texture->ibo);
        texture->ibo = 0;
    }

    // Generate VAO
    glGenVertexArrays(1, &texture->vao);
    if (!texture->vao) {
        SPDLOG_ERROR("Failed to generate VAO");
        return;
    }
    SPDLOG_DEBUG("Created VAO {}", texture->vao);
    
    // Bind VAO to record subsequent operations
    glBindVertexArray(texture->vao);

    // Generate VBO
    glGenBuffers(1, &texture->vbo);
    if (!texture->vbo) {
        SPDLOG_ERROR("Failed to generate VBO");
        glDeleteVertexArrays(1, &texture->vao);
        texture->vao = 0;
        return;
    }
    SPDLOG_DEBUG("Created VBO {}", texture->vbo);
    
    // Generate IBO
    glGenBuffers(1, &texture->ibo);
    if (!texture->ibo) {
        SPDLOG_ERROR("Failed to generate IBO");
        glDeleteVertexArrays(1, &texture->vao);
        glDeleteBuffers(1, &texture->vbo);
        texture->vao = 0;
        texture->vbo = 0;
        return;
    }
    SPDLOG_DEBUG("Created IBO {}", texture->ibo);

    // Simple quad vertices in normalized device coordinates using reduced scale
    // Use -0.5 to 0.5 range instead of -1.0 to 1.0 to address the 2x scaling issue
    const GLfloat vertices[] = {
        -0.5f,  0.5f,  // Top-left
        -0.5f, -0.5f,  // Bottom-left
         0.5f, -0.5f,  // Bottom-right
         0.5f,  0.5f   // Top-right
    };

    const GLuint indices[] = {
        0, 1, 2,  // First triangle (bottom-right)
        0, 2, 3   // Second triangle (top-left)
    };

    // Update buffer data
    glBindBuffer(GL_ARRAY_BUFFER, texture->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    // Check for errors after buffer data upload
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error after uploading vertex data: {}", error);
    }

    // Set up vertex attributes - position (XY)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (void *)0);
    glEnableVertexAttribArray(0);

    // Set up element buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, texture->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    // Check for errors after element buffer upload
    error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error after uploading index data: {}", error);
    }

    // Store the number of indices for drawing
    texture->indexCount = sizeof(indices) / sizeof(indices[0]);
    
    // Verify all was created successfully
    error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error during geometry creation: {}", error);
    } else {
        SPDLOG_INFO("Successfully created geometry with {} indices for VAO={}, VBO={}, IBO={}", 
                   texture->indexCount, texture->vao, texture->vbo, texture->ibo);
    }
    
    // Unbind VAO to prevent accidental modification
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

GLuint Compositor::createShaderProgram(const std::string_view &vertexSource,
                                       const std::string_view &fragmentSource) {
    GLuint program = glCreateProgram();

    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);

    glLinkProgram(program);

    // Check for linking errors
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[1024];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        SPDLOG_ERROR("Shader program linking error: {}", infoLog);
        throw std::runtime_error("Failed to link shader program");
    }

    // Clean up
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return program;
}

GLuint Compositor::compileShader(GLenum type, const std::string_view &source) {
    GLuint shader = glCreateShader(type);

    const GLchar *sourceCStr = source.data();
    glShaderSource(shader, 1, &sourceCStr, nullptr);
    glCompileShader(shader);

    // Check for compilation errors
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[1024];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        SPDLOG_ERROR("Shader compilation error: {}", infoLog);
        throw std::runtime_error("Failed to compile shader");
    }

    return shader;
}

void Compositor::createGeometryBuffers(GLuint &vao, GLuint &vbo, GLuint &ibo) {
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    glGenBuffers(1, &ibo);
}

void Compositor::updateBufferData(GLuint vao, GLuint vbo, GLsizeiptr vboSize, const void *vboData,
                                  GLuint ibo, GLsizeiptr iboSize, const void *iboData) {
    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vboSize, vboData, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, iboSize, iboData, GL_STATIC_DRAW);
}

// Implement damage tracking setup
void Compositor::setupDamageTracking(Window xWindow) {
    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture) {
        SPDLOG_ERROR("Cannot set up damage tracking for non-existent window texture 0x{:x}", xWindow);
        return;
    }
    
    if (texture->damage) {
        // Already has damage tracking
        return;
    }
    
    // Create damage object for this window
    texture->damage = XDamageCreate(m_windowManager.getDisplay(), xWindow, XDamageReportNonEmpty);
    SPDLOG_INFO("Set up damage tracking for window 0x{:x}, damage=0x{:x}", xWindow, texture->damage);
    
    // Mark as needing update initially
    texture->needsUpdate = true;
}

// Implement damage tracking cleanup
void Compositor::cleanupDamageTracking(Window xWindow) {
    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture || !texture->damage) {
        return;
    }
    
    // Destroy the damage object
    XDamageDestroy(m_windowManager.getDisplay(), texture->damage);
    texture->damage = 0;
    SPDLOG_DEBUG("Cleaned up damage tracking for window 0x{:x}", xWindow);
}

// Handle damage events
void Compositor::handleDamageEvent(XDamageNotifyEvent *event) {
    // Find the window by its drawable ID
    Window xWindow = event->drawable;
    
    WindowTexture *texture = getWindowTexture(xWindow);
    if (!texture) {
        SPDLOG_WARN("Received damage event for unknown window 0x{:x}", xWindow);
        return;
    }
    
    // Mark the window as needing update
    texture->needsUpdate = true;
    SPDLOG_DEBUG("Window 0x{:x} marked as damaged", xWindow);
    
    // Subtract the damage region to acknowledge we've seen it
    XDamageSubtract(m_windowManager.getDisplay(), event->damage, None, None);
}

} // namespace ferret
