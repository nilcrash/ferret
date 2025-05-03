/**
 * Ferret Window Manager
 * A modern C++23 compositing window manager based on X11 and OpenGL
 */

// X11 includes
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/shape.h>

// OpenGL includes
#include <GL/glew.h>
#include <GL/glx.h>

// System includes
#include <csignal>
#include <iostream>
#include <sys/time.h>
#include <unistd.h>

// Ferret includes
#include "Compositor.h"
#include "WindowManager.h"

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

// Debugging flag
#ifndef DEBUGGING
#define DEBUGGING 0
#endif

namespace ferret {

// Global flag to control the main loop
static bool g_running = true;

// Global compositor reference for callbacks
static Compositor *g_compositor = nullptr;

// Signal handler to handle Ctrl+C and other termination signals
void signalHandler(int signal) {
    SPDLOG_INFO("Received signal {}, shutting down...", signal);
    g_running = false;
}

// Handle window creation with proper setup
void onWindowCreate(Window window) {
    SPDLOG_INFO("Window created: 0x{:x}", window);
    
    // Get window attributes to see if it's already mapped
    XWindowAttributes attribs;
    if (XGetWindowAttributes(XOpenDisplay(nullptr), window, &attribs)) {
        SPDLOG_INFO("New window 0x{:x} initial state: size={}x{}, depth={}, visual=0x{:x}, mapped={}",
                  window, attribs.width, attribs.height, attribs.depth,
                  XVisualIDFromVisual(attribs.visual),
                  (attribs.map_state == IsViewable ? "yes" : "no"));
    }
    
    if (g_compositor) {
        g_compositor->handleWindowCreated(window);
        
        // Force immediate rendering to make the window visible faster
        g_compositor->renderFrame();
    }
}

// Handle window modifications with proper texture management
void onWindowModify(Window window, bool visible, float x, float y, float width, float height) {
    SPDLOG_INFO("Window modified: 0x{:x}, visible={}, pos=({},{}), size={}x{}", 
               window, visible, x, y, width, height);
    
    if (g_compositor) {
        g_compositor->handleWindowModified(window);

        // Force an immediate rendering update when a window becomes visible
        // or changes position/size
        g_compositor->renderFrame();
    }
}

// Handle window destruction
void onWindowDestroy(Window window) {
    SPDLOG_DEBUG("Window destroyed: 0x{:x}", window);
    if (g_compositor)
        g_compositor->handleWindowDestroyed(window);
}

/**
 * Main window manager class that combines window management and compositing
 */
class FerretWindowManager {
  public:
    FerretWindowManager() : m_windowManager(), m_compositor(m_windowManager) {

        SPDLOG_INFO("FerretWindowManager initialized");
        setupCallbacks();
    }

    ~FerretWindowManager() {
        SPDLOG_INFO("FerretWindowManager destroyed");
    }

    void run() {
        SPDLOG_INFO("Entering main event loop");

        while (g_running) {
            // Process window events
            m_windowManager.processEvents();

            // Render a frame with the compositor
            float deltaTime = m_compositor.renderFrame();

            // Throttle the loop if not using VSync
            if (!m_compositor.isVSyncEnabled()) {
                usleep(10000); // 10ms
            }
        }

        SPDLOG_INFO("Main event loop terminated");
    }

  private:
    void setupCallbacks() {
        // Setup keyboard event handler
        m_windowManager.setKeyboardEventCallback([this](unsigned window, bool pressed,
                                                        unsigned modifiers, unsigned key) {
            // Only handle key presses, not releases
            if (!pressed)
                return;

            // Super+Q to quit the window manager
            if (modifiers == Mod4Mask &&
                key == XKeysymToKeycode(m_windowManager.getDisplay(), XStringToKeysym("q"))) {
                SPDLOG_INFO("Detected quit key combination (Super+Q), shutting down...");
                g_running = false;
            }
            // Super+F1 to list all windows (for debugging)
            else if (modifiers == Mod4Mask &&
                     key == XKeysymToKeycode(m_windowManager.getDisplay(), XStringToKeysym("F1"))) {
                SPDLOG_INFO("Listing all windows (Super+F1):");
                listAllWindows();
            }
            // Super+V to toggle VSync
            else if (modifiers == Mod4Mask &&
                     key == XKeysymToKeycode(m_windowManager.getDisplay(), XStringToKeysym("v"))) {
                bool newVSync = !m_compositor.isVSyncEnabled();
                SPDLOG_INFO("Toggling VSync: {}", newVSync ? "enabled" : "disabled");
                m_compositor.setVSync(newVSync);
            }
        });

        // Setup window create/modify/destroy callbacks
        m_windowManager.setCreateEventCallback([this](unsigned window) { onWindowCreate(window); });

        m_windowManager.setModifyEventCallback(
            [this](unsigned window, bool visible, float x, float y, float width, float height) {
                onWindowModify(window, visible, x, y, width, height);
            });

        m_windowManager.setDestroyEventCallback(
            [this](unsigned window) { onWindowDestroy(window); });
    }

    void listAllWindows() {
        const auto &windows = m_windowManager.getManagedWindows();
        SPDLOG_INFO("Total managed windows: {}", windows.size());

        for (const auto &win : windows) {
            SPDLOG_INFO("Window 0x{:x}: visible={} pos=({},{}) size={}x{}", win.getXWindow(),
                        win.isVisible(), win.getX(), win.getY(), win.getWidth(), win.getHeight());
        }
    }

    WindowManager m_windowManager;
    Compositor m_compositor;
};

} // namespace ferret

/**
 * Main function - entry point of the Ferret window manager
 */
int main(int argc, char *argv[]) {
    (void)argc; // Silence unused parameter warning
    (void)argv; // Silence unused parameter warning

    // Initialize logging
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_level(spdlog::level::debug);

    SPDLOG_INFO("Ferret window manager starting up...");

    // Set up signal handling for clean shutdown
    std::signal(SIGINT, ferret::signalHandler);  // Ctrl+C
    std::signal(SIGTERM, ferret::signalHandler); // Termination request

    try {
        // Create and run window manager
        ferret::FerretWindowManager windowManager;
        windowManager.run();
    } catch (const std::exception &e) {
        SPDLOG_CRITICAL("Fatal error: {}", e.what());
        return 1;
    }

    SPDLOG_INFO("Ferret window manager shutting down gracefully");
    return 0;
}
