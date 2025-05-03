/**
 * Ferret Window Manager
 * A modern C++23 compositing window manager based on X11 and OpenGL
 *
 * WindowManager.cpp - Main window manager class implementation
 */

#include "WindowManager.h"
#include "Compositor.h" // Add include for Compositor to resolve forward declaration

#include <cstring>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace ferret {

// Error handler function for X11 errors
int WindowManager::errorHandler(Display *display, XErrorEvent *event) {
    if (!event->resourceid)
        return 0; // Invalid window

    char buffer[1024];
    XGetErrorText(display, event->error_code, buffer, sizeof(buffer));

    SPDLOG_ERROR("X11 Error: code = {}, string = {}, resource ID = 0x{:x}", event->error_code,
                 buffer, event->resourceid);
    return 0;
}

// Error reporting method
void WindowManager::error(const std::string_view &message) const {
    SPDLOG_ERROR("[WindowManager] {}", message);
    throw std::runtime_error(std::string(message));
}

// Constructor - Initializes the window manager
WindowManager::WindowManager() {
    SPDLOG_DEBUG("Initializing WindowManager");

    // Open display connection to X server
    m_display = XOpenDisplay(nullptr); // Default to DISPLAY environment variable
    if (!m_display) {
        error("Failed to open display");
    }

    // For debugging - report errors synchronously as they occur
    XSynchronize(m_display, DEBUGGING);

    // Get screen and root window
    m_screen = DefaultScreen(m_display);
    m_rootWindow = DefaultRootWindow(m_display);

    // Get width/height of root window
    XWindowAttributes attributes;
    XGetWindowAttributes(m_display, m_rootWindow, &attributes);

    m_width = attributes.width;
    m_height = attributes.height;

    SPDLOG_INFO("Display resolution: {}x{}", m_width, m_height);

    // Tell X to send us all CreateNotify, ConfigureNotify, and DestroyNotify events
    // SubstructureNotifyMask also sends back some other events
    XSelectInput(m_display, m_rootWindow,
                 SubstructureNotifyMask | PointerMotionMask | ButtonMotionMask | ButtonPressMask |
                     ButtonReleaseMask);

    // Grab keyboard shortcuts
    XGrabKey(m_display, XKeysymToKeycode(m_display, XStringToKeysym("F1")), Mod4Mask, m_rootWindow,
             0, GrabModeAsync, GrabModeAsync);
    XGrabKey(m_display, XKeysymToKeycode(m_display, XStringToKeysym("q")), Mod4Mask, m_rootWindow,
             0, GrabModeAsync, GrabModeAsync);
    XGrabKey(m_display, XKeysymToKeycode(m_display, XStringToKeysym("f")), Mod4Mask | Mod1Mask,
             m_rootWindow, 0, GrabModeAsync, GrabModeAsync);
    XGrabKey(m_display, XKeysymToKeycode(m_display, XStringToKeysym("f")), Mod4Mask, m_rootWindow,
             0, GrabModeAsync, GrabModeAsync);
    XGrabKey(m_display, XKeysymToKeycode(m_display, XStringToKeysym("t")), Mod4Mask, m_rootWindow,
             0, GrabModeAsync, GrabModeAsync);
    XGrabKey(m_display, XKeysymToKeycode(m_display, XStringToKeysym("v")), Mod4Mask, m_rootWindow,
             0, GrabModeAsync, GrabModeAsync);
    XGrabKey(m_display, XKeysymToKeycode(m_display, XStringToKeysym("r")), Mod4Mask, m_rootWindow,
             0, GrabModeAsync, GrabModeAsync);
    XGrabKey(m_display, XKeysymToKeycode(m_display, XK_Print), Mod4Mask | Mod1Mask, m_rootWindow, 0,
             GrabModeAsync, GrabModeAsync); // PrtSc
    XGrabKey(m_display, XKeysymToKeycode(m_display, XK_Print), Mod4Mask, m_rootWindow, 0,
             GrabModeAsync, GrabModeAsync); // PrtSc

    // Setup atoms (explained in more detail in the WindowManager class)
    // Specify which atoms are supported in _NET_SUPPORTED
    m_clientListAtom = XInternAtom(m_display, "_NET_CLIENT_LIST", 0);

    Atom supportedListAtom = XInternAtom(m_display, "_NET_SUPPORTED", 0);
    Atom supportedAtoms[] = {supportedListAtom, m_clientListAtom};

    XChangeProperty(m_display, m_rootWindow, supportedListAtom, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(supportedAtoms),
                    sizeof(supportedAtoms) / sizeof(*supportedAtoms));

    // Set up _NET_SUPPORTING_WM_CHECK as specified by the EWMH specification
    // https://developer.gnome.org/wm-spec/
    Atom supportingWmCheckAtom = XInternAtom(m_display, "_NET_SUPPORTING_WM_CHECK", 0);
    Window supportWindow = XCreateSimpleWindow(m_display, m_rootWindow, 0, 0, 1, 1, 0, 0, 0);

    Window supportWindowList[] = {supportWindow};

    XChangeProperty(m_display, m_rootWindow, supportingWmCheckAtom, XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(supportWindowList), 1);
    XChangeProperty(m_display, supportWindow, supportingWmCheckAtom, XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(supportWindowList), 1);

    Atom nameAtom = XInternAtom(m_display, "_NET_WM_NAME", 0);
    XChangeProperty(m_display, supportWindow, nameAtom, XA_STRING, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(WM_NAME), std::strlen(WM_NAME));

    // Get all monitors and their individual resolutions
    m_monitorInfos = XineramaQueryScreens(m_display, &m_monitorCount);
    SPDLOG_INFO("Found {} monitor(s)", m_monitorCount);

    // Set up our own error handler so X doesn't crash
    XSetErrorHandler(&WindowManager::errorHandler);

    // Add the support window to our event blacklist
    m_eventBlacklistedWindows.insert(supportWindow);

    SPDLOG_INFO("WindowManager initialized");
}

// Destructor - Clean up resources
WindowManager::~WindowManager() {
    // Free monitor information
    if (m_monitorInfos) {
        XFree(m_monitorInfos);
        m_monitorInfos = nullptr;
    }

    // Close display connection
    if (m_display) {
        XCloseDisplay(m_display);
        m_display = nullptr;
    }

    SPDLOG_INFO("WindowManager destroyed");
}

// Implementation of coordinate conversion utilities

float WindowManager::widthDimensionToFloat(int pixels) const {
    return static_cast<float>(pixels) / m_width * 2;
}

float WindowManager::heightDimensionToFloat(int pixels) const {
    return static_cast<float>(pixels) / m_height * 2;
}

float WindowManager::xCoordinateToFloat(int pixels) const {
    return widthDimensionToFloat(pixels) - 1;
}

float WindowManager::yCoordinateToFloat(int pixels) const {
    return -heightDimensionToFloat(pixels) + 1;
}

int WindowManager::floatToWidthDimension(float x) const {
    return static_cast<int>(std::round(x / 2 * m_width));
}

int WindowManager::floatToHeightDimension(float x) const {
    return static_cast<int>(std::round(x / 2 * m_height));
}

int WindowManager::floatToXCoordinate(float x) const {
    return floatToWidthDimension(x + 1);
}

int WindowManager::floatToYCoordinate(float x) const {
    return floatToHeightDimension(-x + 1);
}

// Implementation of monitor information getters

float WindowManager::getMonitorX(int monitorIndex) const {
    if (monitorIndex >= m_monitorCount) {
        error("Monitor index out of bounds");
    }
    return xCoordinateToFloat(m_monitorInfos[monitorIndex].x_org +
                              m_monitorInfos[monitorIndex].width / 2);
}

float WindowManager::getMonitorY(int monitorIndex) const {
    if (monitorIndex >= m_monitorCount) {
        error("Monitor index out of bounds");
    }
    return yCoordinateToFloat(m_monitorInfos[monitorIndex].y_org +
                              m_monitorInfos[monitorIndex].height / 2);
}

float WindowManager::getMonitorWidth(int monitorIndex) const {
    if (monitorIndex >= m_monitorCount) {
        error("Monitor index out of bounds");
    }
    return widthDimensionToFloat(m_monitorInfos[monitorIndex].width);
}

float WindowManager::getMonitorHeight(int monitorIndex) const {
    if (monitorIndex >= m_monitorCount) {
        error("Monitor index out of bounds");
    }
    return heightDimensionToFloat(m_monitorInfos[monitorIndex].height);
}

// Implementation of window management helpers

bool WindowManager::isWindowBlacklisted(Window window) const {
    return m_eventBlacklistedWindows.find(window) != m_eventBlacklistedWindows.end();
}

int WindowManager::findWindowById(Window xid) const {
    for (size_t i = 0; i < m_windows.size(); ++i) {
        if (m_windows[i].getXWindow() == xid) {
            return static_cast<int>(i);
        }
    }

// If we're here, window was not found
#if !DEBUGGING
    error("Nonexistent window XID");
#endif

    return -1;
}

void WindowManager::syncWindow(ManagedWindow &window) {
    XWindowAttributes attributes;
    XGetWindowAttributes(m_display, window.getXWindow(), &attributes);

    window.setVisibility(attributes.map_state == IsViewable);
    window.setPosition(attributes.x, attributes.y);
    window.setSize(attributes.width, attributes.height);

    // TODO: Get window opacity using _NET_WM_WINDOW_OPACITY atom
}

void WindowManager::updateClientList() {
    std::vector<Window> clientList;
    clientList.reserve(m_windows.size());

    for (const auto &window : m_windows) {
        clientList.push_back(window.getXWindow());
    }

    if (!clientList.empty()) {
        XChangeProperty(m_display, m_rootWindow, m_clientListAtom, XA_WINDOW, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(clientList.data()),
                        static_cast<int>(clientList.size()));
    }
}

// Implementation of helper methods for compositor integration

const ManagedWindow *WindowManager::findWindowByXID(Window xid) const {
    for (const auto &window : m_windows) {
        if (window.getXWindow() == xid) {
            return &window;
        }
    }
    return nullptr;
}

// Implementation of window management methods

void WindowManager::processEvents() {
    // Check for X events
    while (XPending(m_display)) {
        XEvent event;
        XNextEvent(m_display, &event);

        // Check for damage events 
        if (m_compositor && m_compositor->getDamageEventBase() > 0) {
            int damageEventBase = m_compositor->getDamageEventBase();
            if (event.type == damageEventBase + XDamageNotify) {
                XDamageNotifyEvent *damageEvent = reinterpret_cast<XDamageNotifyEvent*>(&event);
                m_compositor->handleDamageEvent(damageEvent);
                continue;
            }
        }

        // Process event based on type
        switch (event.type) {
            case CreateNotify: {
                // New window created
                XCreateWindowEvent &e = event.xcreatewindow;
                
                // Skip blacklisted windows
                if (isWindowBlacklisted(e.window)) {
                    SPDLOG_DEBUG("Ignoring blacklisted CreateNotify for window 0x{:x}", e.window);
                    break;
                }

                SPDLOG_INFO("CreateNotify: window=0x{:x}, position=({},{}), size={}x{}, parent=0x{:x}",
                            e.window, e.x, e.y, e.width, e.height, e.parent);

                // Add window to our list
                m_windows.emplace_back(e.window);
                ManagedWindow &window = m_windows.back();

                // Update window info
                syncWindow(window);

                // Update _NET_CLIENT_LIST
                updateClientList();

                // Call client callback if set
                if (m_createEventCallback) {
                    m_createEventCallback(window.getXWindow());
                }
                break;
            }

            case ConfigureNotify: {
                // Window moved or resized
                XConfigureEvent &e = event.xconfigure;
                
                // Skip blacklisted windows
                if (isWindowBlacklisted(e.window)) {
                    SPDLOG_DEBUG("Ignoring blacklisted ConfigureNotify for window 0x{:x}", e.window);
                    break;
                }

                // Find window in our list
                int index = findWindowById(e.window);
                if (index < 0) {
                    SPDLOG_DEBUG("ConfigureNotify for unknown window 0x{:x}, ignoring", e.window);
                    break;
                }

                ManagedWindow &window = m_windows[index];

                // Update window info
                bool wasVisible = window.isVisible();
                int oldX = window.getX();
                int oldY = window.getY();
                int oldWidth = window.getWidth();
                int oldHeight = window.getHeight();

                syncWindow(window);

                bool visible = window.isVisible();
                int x = window.getX();
                int y = window.getY();
                int width = window.getWidth();
                int height = window.getHeight();

                // Call client callback if window changed and callback is set
                if (m_modifyEventCallback && (visible != wasVisible || x != oldX || y != oldY ||
                                              width != oldWidth || height != oldHeight)) {

                    SPDLOG_INFO("ConfigureNotify: window=0x{:x} visible={} pos=({},{}) size={}x{}",
                                e.window, visible, x, y, width, height);

                    m_modifyEventCallback(window.getXWindow(), visible,
                                          xCoordinateToFloat(x + width / 2),
                                          yCoordinateToFloat(y + height / 2),
                                          widthDimensionToFloat(width), heightDimensionToFloat(height));
                }
                break;
            }

            case MapNotify: {
                // Window mapped (shown)
                XMapEvent &e = event.xmap;
                
                // Skip blacklisted windows
                if (isWindowBlacklisted(e.window)) {
                    SPDLOG_DEBUG("Ignoring blacklisted MapNotify for window 0x{:x}", e.window);
                    break;
                }

                // Find window in our list
                int index = findWindowById(e.window);
                if (index < 0) {
                    SPDLOG_DEBUG("MapNotify for unknown window 0x{:x}, ignoring", e.window);
                    break;
                }

                ManagedWindow &window = m_windows[index];
                bool wasVisible = window.isVisible();

                // Update window info
                syncWindow(window);

                // Notify if visibility changed
                if (!wasVisible && window.isVisible() && m_modifyEventCallback) {
                    SPDLOG_INFO("Window 0x{:x} is now visible: pos=({},{}) size={}x{}", e.window,
                                window.getX(), window.getY(), window.getWidth(), window.getHeight());

                    m_modifyEventCallback(window.getXWindow(), true,
                                          xCoordinateToFloat(window.getX() + window.getWidth() / 2),
                                          yCoordinateToFloat(window.getY() + window.getHeight() / 2),
                                          widthDimensionToFloat(window.getWidth()),
                                          heightDimensionToFloat(window.getHeight()));
                }
                break;
            }

            case UnmapNotify: {
                // Window unmapped (hidden)
                XUnmapEvent &e = event.xunmap;
                
                // Skip blacklisted windows
                if (isWindowBlacklisted(e.window)) {
                    SPDLOG_DEBUG("Ignoring blacklisted UnmapNotify for window 0x{:x}", e.window);
                    break;
                }

                // Find window in our list
                int index = findWindowById(e.window);
                if (index < 0) {
                    SPDLOG_DEBUG("UnmapNotify for unknown window 0x{:x}, ignoring", e.window);
                    break;
                }

                ManagedWindow &window = m_windows[index];
                bool wasVisible = window.isVisible();

                // Update window info
                syncWindow(window);

                // Notify if visibility changed
                if (wasVisible && !window.isVisible() && m_modifyEventCallback) {
                    SPDLOG_INFO("Window 0x{:x} is now hidden", e.window);

                    m_modifyEventCallback(window.getXWindow(), false,
                                          xCoordinateToFloat(window.getX() + window.getWidth() / 2),
                                          yCoordinateToFloat(window.getY() + window.getHeight() / 2),
                                          widthDimensionToFloat(window.getWidth()),
                                          heightDimensionToFloat(window.getHeight()));
                }
                break;
            }

            case DestroyNotify: {
                // Window destroyed
                XDestroyWindowEvent &e = event.xdestroywindow;
                
                // Skip blacklisted windows
                if (isWindowBlacklisted(e.window)) {
                    SPDLOG_DEBUG("Ignoring blacklisted DestroyNotify for window 0x{:x}", e.window);
                    break;
                }

                SPDLOG_INFO("DestroyNotify: window=0x{:x}", e.window);

                // Find window in our list
                int index = findWindowById(e.window);
                if (index < 0) {
                    SPDLOG_DEBUG("DestroyNotify for unknown window 0x{:x}, ignoring", e.window);
                    break;
                }

                // Call client callback if set
                if (m_destroyEventCallback) {
                    m_destroyEventCallback(e.window);
                }

                // Remove window from our list
                if (index >= 0 && index < static_cast<int>(m_windows.size())) {
                    m_windows.erase(m_windows.begin() + static_cast<size_t>(index));
                }

                // Update _NET_CLIENT_LIST
                updateClientList();
                break;
            }

            case KeyPress: {
                // Keyboard key pressed
                XKeyEvent &e = event.xkey;

                SPDLOG_DEBUG("KeyPress: window=0x{:x} state={} keycode={}", e.window, e.state,
                             e.keycode);

                // Call client callback if set
                if (m_keyboardEventCallback) {
                    m_keyboardEventCallback(e.window, true, e.state, e.keycode);
                }
                break;
            }

            case KeyRelease: {
                // Keyboard key released
                XKeyEvent &e = event.xkey;

                SPDLOG_DEBUG("KeyRelease: window=0x{:x} state={} keycode={}", e.window, e.state,
                             e.keycode);

                // Call client callback if set
                if (m_keyboardEventCallback) {
                    m_keyboardEventCallback(e.window, false, e.state, e.keycode);
                }
                break;
            }

            case ButtonPress: {
                // Mouse button pressed
                XButtonEvent &e = event.xbutton;

                SPDLOG_DEBUG("ButtonPress: window=0x{:x} button={} pos=({},{})", e.window, e.button,
                             e.x, e.y);

                // Call client callback if set
                if (m_clickEventCallback) {
                    bool handled =
                        m_clickEventCallback(e.window, true, e.state, e.button, xCoordinateToFloat(e.x),
                                             yCoordinateToFloat(e.y));

                    // If not handled and it's Button1 (left click), focus the window
                    if (!handled && e.button == Button1) {
                        focusWindow(e.window);
                    }
                }
                break;
            }

            case ButtonRelease: {
                // Mouse button released
                XButtonEvent &e = event.xbutton;

                SPDLOG_DEBUG("ButtonRelease: window=0x{:x} button={} pos=({},{})", e.window, e.button,
                             e.x, e.y);

                // Call client callback if set
                if (m_clickEventCallback) {
                    m_clickEventCallback(e.window, false, e.state, e.button, xCoordinateToFloat(e.x),
                                         yCoordinateToFloat(e.y));
                }
                break;
            }

            case MotionNotify: {
                // Mouse motion
                XMotionEvent &e = event.xmotion;

                // Skip if no callback set
                if (!m_moveEventCallback)
                    break;

                // Skip blacklisted windows
                if (isWindowBlacklisted(e.window)) {
                    break;
                }

                // Call client callback
                m_moveEventCallback(e.window, e.state, xCoordinateToFloat(e.x),
                                    yCoordinateToFloat(e.y));
                break;
            }

            default: {
                // Unknown event type
                SPDLOG_DEBUG("Unhandled X11 event type: {}", event.type);
                break;
            }
        }
    }
}

void WindowManager::focusWindow(unsigned windowId) {
    // Check if window exists
    int index = findWindowById(windowId);
    if (index < 0)
        return;

    SPDLOG_DEBUG("Focusing window 0x{:x}", windowId);

    // Set input focus to the window
    XSetInputFocus(m_display, windowId, RevertToParent, CurrentTime);

    // Raise the window to the top
    XRaiseWindow(m_display, windowId);
}

void WindowManager::closeWindow(unsigned windowId) {
    // Check if window exists
    int index = findWindowById(windowId);
    if (index < 0)
        return;

    SPDLOG_DEBUG("Closing window 0x{:x}", windowId);

    // Send WM_DELETE_WINDOW message
    Atom wmDeleteWindow = XInternAtom(m_display, "WM_DELETE_WINDOW", False);
    Atom wmProtocols = XInternAtom(m_display, "WM_PROTOCOLS", False);

    if (wmDeleteWindow != None) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = windowId;
        event.xclient.message_type = wmProtocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = wmDeleteWindow;
        event.xclient.data.l[1] = CurrentTime;

        XSendEvent(m_display, windowId, False, NoEventMask, &event);
    }
}

void WindowManager::killWindow(unsigned windowId) {
    // Check if window exists
    int index = findWindowById(windowId);
    if (index < 0)
        return;

    SPDLOG_DEBUG("Killing window 0x{:x}", windowId);

    // Kill the window - this is more brutal than closeWindow()
    XKillClient(m_display, windowId);
}

void WindowManager::moveWindow(unsigned windowId, float x, float y, float width, float height) {
    // Check if window exists
    int index = findWindowById(windowId);
    if (index < 0)
        return;

    // Convert from normalized coordinates to screen pixels
    int pixelX = floatToXCoordinate(x - width / 2);
    int pixelY = floatToYCoordinate(y - height / 2);
    int pixelWidth = floatToWidthDimension(width);
    int pixelHeight = floatToHeightDimension(height);

    SPDLOG_DEBUG("Moving window 0x{:x} to ({},{}) with size {}x{}", windowId, pixelX, pixelY,
                 pixelWidth, pixelHeight);

    // Move and resize the window
    XMoveResizeWindow(m_display, windowId, pixelX, pixelY, pixelWidth, pixelHeight);

    // Make sure X processes the request immediately
    XFlush(m_display);
}

} // namespace ferret
