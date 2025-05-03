/**
 * Ferret Window Manager
 * A modern C++23 compositing window manager based on X11 and OpenGL
 *
 * WindowManager.h - Main window manager class header
 */

#pragma once

// X11 includes
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/shape.h>

// Standard includes
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifndef DEBUGGING
#define DEBUGGING 0
#endif

namespace ferret {

// Forward declarations
class Compositor;

/**
 * ManagedWindow - Represents a window managed by the window manager
 */
class ManagedWindow {
  public:
    explicit ManagedWindow(Window xWindow)
        : m_window(xWindow), m_visible(false), m_x(0), m_y(0), m_width(0), m_height(0) {
    }

    // Getters
    Window getXWindow() const {
        return m_window;
    }
    bool isVisible() const {
        return m_visible;
    }
    int getX() const {
        return m_x;
    }
    int getY() const {
        return m_y;
    }
    int getWidth() const {
        return m_width;
    }
    int getHeight() const {
        return m_height;
    }
    void *getInternal() const {
        return m_internal;
    }

    // Setters
    void setVisibility(bool visible) {
        m_visible = visible;
    }
    void setPosition(int x, int y) {
        m_x = x;
        m_y = y;
    }
    void setSize(int width, int height) {
        m_width = width;
        m_height = height;
    }
    void setInternal(void *data) {
        m_internal = data;
    }

  private:
    Window m_window;
    bool m_visible;
    int m_x, m_y;
    int m_width, m_height;

    // Optional data for extensions like the compositor
    void *m_internal = nullptr;
};

/**
 * WindowManager - Main class for managing X11 windows
 */
class WindowManager {
  public:
    // Event callback types using modern C++ function objects
    using KeyboardEventCallback =
        std::function<void(unsigned window, bool pressed, unsigned modifiers, unsigned key)>;
    using ClickEventCallback = std::function<bool(unsigned window, bool pressed, unsigned modifiers,
                                                  unsigned button, float x, float y)>;
    using MoveEventCallback =
        std::function<void(unsigned window, unsigned modifiers, float x, float y)>;
    using CreateEventCallback = std::function<void(unsigned window)>;
    using ModifyEventCallback = std::function<void(unsigned window, bool visible, float x, float y,
                                                   float width, float height)>;
    using DestroyEventCallback = std::function<void(unsigned window)>;

    /**
     * Constructor - Initializes the window manager
     */
    WindowManager();

    /**
     * Destructor - Cleans up resources
     */
    ~WindowManager();

    // Prevent copying and assignment
    WindowManager(const WindowManager &) = delete;
    WindowManager &operator=(const WindowManager &) = delete;

    // Basic window manager functions
    void processEvents();
    void closeWindow(unsigned windowId);
    void killWindow(unsigned windowId);
    void moveWindow(unsigned windowId, float x, float y, float width, float height);
    void focusWindow(unsigned windowId);

    // Monitor information queries
    int getMonitorCount() const {
        return m_monitorCount;
    }
    float getMonitorX(int monitorIndex) const;
    float getMonitorY(int monitorIndex) const;
    float getMonitorWidth(int monitorIndex) const;
    float getMonitorHeight(int monitorIndex) const;

    // Resolution queries
    int getScreenWidth() const {
        return m_width;
    }
    int getScreenHeight() const {
        return m_height;
    }

    // Coordinate conversion utilities
    float widthDimensionToFloat(int pixels) const;
    float heightDimensionToFloat(int pixels) const;
    float xCoordinateToFloat(int pixels) const;
    float yCoordinateToFloat(int pixels) const;
    int floatToWidthDimension(float x) const;
    int floatToHeightDimension(float x) const;
    int floatToXCoordinate(float x) const;
    int floatToYCoordinate(float x) const;

    // Event callback setters
    void setKeyboardEventCallback(KeyboardEventCallback callback) {
        m_keyboardEventCallback = std::move(callback);
    }
    void setClickEventCallback(ClickEventCallback callback) {
        m_clickEventCallback = std::move(callback);
    }
    void setMoveEventCallback(MoveEventCallback callback) {
        m_moveEventCallback = std::move(callback);
    }
    void setCreateEventCallback(CreateEventCallback callback) {
        m_createEventCallback = std::move(callback);
    }
    void setModifyEventCallback(ModifyEventCallback callback) {
        m_modifyEventCallback = std::move(callback);
    }
    void setDestroyEventCallback(DestroyEventCallback callback) {
        m_destroyEventCallback = std::move(callback);
    }

    // Raw X11 access (for advanced usage)
    Display *getDisplay() const {
        return m_display;
    }
    Window getRootWindow() const {
        return m_rootWindow;
    }

    // Window management methods
    const std::vector<ManagedWindow> &getManagedWindows() const {
        return m_windows;
    }
    const ManagedWindow *findWindowByXID(Window xid) const;

    // Event blacklisting
    void addEventBlacklistedWindow(Window window) {
        m_eventBlacklistedWindows.insert(window);
    }
    void removeEventBlacklistedWindow(Window window) {
        m_eventBlacklistedWindows.erase(window);
    }

    // Compositor integration
    void setCompositor(Compositor* compositor) {
        m_compositor = compositor;
    }
    
    Compositor* getCompositor() const {
        return m_compositor;
    }

  private:
    // Error handling
    void error(const std::string_view &message) const;
    static int errorHandler(Display *display, XErrorEvent *event);

    // Window management internals
    bool isWindowBlacklisted(Window window) const;
    int findWindowById(Window xid) const;
    void syncWindow(ManagedWindow &window);
    void updateClientList();

    // X11 connection
    Display *m_display = nullptr;
    int m_screen = 0;
    Window m_rootWindow = 0;

    // Screen dimensions
    unsigned m_width = 0;
    unsigned m_height = 0;

    // Window storage
    std::vector<ManagedWindow> m_windows;

    // Monitor information
    int m_monitorCount = 0;
    XineramaScreenInfo *m_monitorInfos = nullptr;

    // X11 atoms for communication
    Atom m_clientListAtom = 0;

    // Blacklisted windows (for filtering events)
    std::unordered_set<Window> m_eventBlacklistedWindows;

    // Event callbacks
    KeyboardEventCallback m_keyboardEventCallback;
    ClickEventCallback m_clickEventCallback;
    MoveEventCallback m_moveEventCallback;
    CreateEventCallback m_createEventCallback;
    ModifyEventCallback m_modifyEventCallback;
    DestroyEventCallback m_destroyEventCallback;

    // Constants
    static constexpr const char *WM_NAME = "ferret";

    // Compositor reference
    Compositor* m_compositor = nullptr;
};

} // namespace ferret
