void FerretApp::init() {
    SPDLOG_INFO("Initializing ferret window manager");

    // Create window manager and configure callbacks
    m_windowManager = std::make_unique<WindowManager>();
    configureWindowManager();

    // Create compositor
    m_compositor = std::make_unique<Compositor>(*m_windowManager);
    
    // Register compositor with window manager for damage events
    m_windowManager->setCompositor(m_compositor.get());

    // Register window event handlers from the window manager in the compositor
    m_windowManager->setCreateEventCallback(
        [this](unsigned window) { m_compositor->handleWindowCreated(window); });

    m_windowManager->setDestroyEventCallback(
        [this](unsigned window) { m_compositor->handleWindowDestroyed(window); });

    m_windowManager->setModifyEventCallback(
        [this](unsigned window, bool visible, float x, float y, float width, float height) {
            m_compositor->handleWindowModified(window);
        });
} 