#pragma once

#include <QObject>
#include <QQuickItem>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGRenderNode>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QRunnable>
#include <QMutex>
#include <QtQml/qqmlregistration.h>
#include <qeventloop.h>
#include <qnamespace.h>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QClipboard>
#include <QMimeData>
#include <QByteArray>
#include <QtEndian>
#include <QFileInfo>
#include <QDir>
#include <winpr/user.h>

#include <GLES2/gl2.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

/* Forward declaration — implemented in mini-qf-client.cc.
 * Called on every local mouse move. Restores the last non-null RDP cursor
 * if the server hid it via SYSPTR_NULL.  Only restores when the pointer
 * has moved at least CURSOR_RESTORE_THRESHOLD pixels from the hide
 * position, so that tiny sensor noise doesn't defeat auto-hide. */
extern void rdp_notify_mouse_moved(double qx, double qy);

#include "freerdp/freerdp.h"
#include "freerdp/client/cliprdr.h"
#include "freerdp/input.h"

#include "qf_util.h"
#include "qf_log.h"

/* Forward declarations from mini-qf-client.cc */
void start_rdp_connection();
void notify_window_resized();

/* =========================================================================
 * RenderNode — QSGRenderNode subclass for GL texture rendering
 *
 * The render() method runs INSIDE the scene graph's render pass, so native
 * GL commands are valid (unlike beforeRendering/afterRendering signals which
 * fire outside the render pass in Qt 6.8 QRhi).
 *
 * Frame data is uploaded to the texture on the render thread via
 * beforeRendering signal; this node only draws the textured quad.
 * ========================================================================= */
class RenderNode : public QSGRenderNode
{
public:
    /* These are pointers to RdpViewItem's atomics, set/reset atomically */
    std::atomic<GLuint>* glTexture = nullptr;
    std::atomic<GLuint>* program   = nullptr;
    std::atomic<GLuint>* vbo       = nullptr;

    /* Explicit viewport size — set by updatePaintNode from boundingRect.
     * RenderNode renders with these dimensions via glViewport()
     * instead of relying on the scene graph's cached viewport, which may
     * lag behind QML layout changes (see the "stuck resize" bug). */
    int vpW = 0;
    int vpH = 0;

    StateFlags changedStates() const override
    {
        /* We don't modify scene graph state that needs restoring */
        return {};
    }

    void render(const RenderState*) override
    {
        if (!glTexture || !program || !vbo)
        {
            qf::log::info("render/node", "early-return: null ptr");
            return;
        }

        GLuint tex = glTexture->load(std::memory_order_acquire);
        GLuint prog = program->load(std::memory_order_acquire);
        GLuint buf = vbo->load(std::memory_order_acquire);
        if (!tex || !prog || !buf)
        {
            qf::log::info("render/node",
                "skip render: tex={} prog={} buf={}",
                tex, prog, buf);
            return;
        }

        // 显式设置 GL 视口为 QML item 的实际大小，
        // 不依赖场景图的缓存视口（缩放后场景图可能仍用旧尺寸）。
        if (vpW > 0 && vpH > 0)
            glViewport(0, 0, vpW, vpH);

        glUseProgram(prog);

        GLint posAttr = glGetAttribLocation(prog, "aPos");
        GLint tcAttr  = glGetAttribLocation(prog, "aTexCoord");

        glBindBuffer(GL_ARRAY_BUFFER, buf);
        constexpr GLsizei stride = 4 * sizeof(float); // pos(2) + tex(2)

        if (posAttr >= 0)
        {
            glEnableVertexAttribArray(static_cast<GLuint>(posAttr));
            glVertexAttribPointer(static_cast<GLuint>(posAttr), 2, GL_FLOAT,
                                  GL_FALSE, stride, nullptr);
        }
        if (tcAttr >= 0)
        {
            glEnableVertexAttribArray(static_cast<GLuint>(tcAttr));
            glVertexAttribPointer(static_cast<GLuint>(tcAttr), 2, GL_FLOAT,
                                  GL_FALSE, stride,
                                  reinterpret_cast<const void*>(2 * sizeof(float)));
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        GLint texUniform = glGetUniformLocation(prog, "uTexture");
        if (texUniform >= 0)
            glUniform1i(texUniform, 0);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (posAttr >= 0)
            glDisableVertexAttribArray(static_cast<GLuint>(posAttr));
        if (tcAttr >= 0)
            glDisableVertexAttribArray(static_cast<GLuint>(tcAttr));

        glUseProgram(0);
    }
};

/* =========================================================================
 * RdpViewItem — QQuickItem-backed RDP view using GL texture upload
 *
 * Rendering pipeline:
 *
 *   FreeRDP thread → decodes H.264 frames into gdi->primary_buffer (heap)
 *
 *   Qt main thread (QueuedConnection from EndPaint):
 *     updateGdiFrame() → copies dirty rect from GDI buffer to staging buffer
 *
 *   Qt render thread (beforeRendering signal):
 *     uploadTextureOnRenderThread() → glTexSubImage2D from staging buffer
 *
 *   Qt render thread (QSGRenderNode::render):
 *     Renders fullscreen textured quad inside the scene graph's render pass
 *
 *   updatePaintNode creates a RenderNode owned by the scene graph.
 * ========================================================================= */
class RdpViewItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int rdpWidth READ rdpWidth NOTIFY rdpGeometryChanged)
    Q_PROPERTY(int rdpHeight READ rdpHeight NOTIFY rdpGeometryChanged)
    Q_PROPERTY(bool fullscreen READ isFullscreen WRITE setFullscreen NOTIFY fullscreenChanged)

public:
    RdpViewItem(QQuickItem* parent = nullptr) : QQuickItem(parent)
    {
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setFlag(QQuickItem::ItemIsFocusScope, true);
        setFocus(true);
        setFlag(QQuickItem::ItemHasContents, true);

        /* Install global event filter to intercept ShortcutOverride events
         * when fullscreen, preventing Qt from consuming keys before they
         * reach the RDP session (e.g. Tab, Ctrl+C, direction keys). */
        if (auto* app = QGuiApplication::instance())
            app->installEventFilter(this);

        connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
                this, &RdpViewItem::dataChangedCallback);

        qf::log::info("view/init", "RdpViewItem created");
    }
    bool isFullscreen() const { return m_fullscreen; }

    void setFullscreen(bool fs)
    {
        if (m_fullscreen != fs)
        {
            m_fullscreen = fs;
            emit fullscreenChanged();
        }
    }

    /* Intercept ShortcutOverride when fullscreen to prevent Qt
     * from consuming shortcut keys before they reach RDP. */
    bool eventFilter(QObject* /*obj*/, QEvent* event) override
    {
        if (event->type() == QEvent::ShortcutOverride && m_fullscreen)
        {
            if (QGuiApplication::modalWindow())
                return false;
            event->accept();
            return true;
        }
        return false;
    }

    /* DEBUG: track geometry changes */
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override
    {
        QQuickItem::geometryChange(newGeometry, oldGeometry);
        if (newGeometry.size() != oldGeometry.size()) {
            qf::log::info("view/geometry", "size {}x{} -> {}x{}",
                          static_cast<int>(oldGeometry.width()),
                          static_cast<int>(oldGeometry.height()),
                          static_cast<int>(newGeometry.width()),
                          static_cast<int>(newGeometry.height()));
        }
    }

    ~RdpViewItem() override
    {
        /* Schedule GL resource cleanup on render thread */
        if (window())
        {
            window()->scheduleRenderJob(
                new GLResourceCleanupJob(m_glTexture, m_program, m_vbo),
                QQuickWindow::BeforeSynchronizingStage);
        }
    }

    /* Expose current RDP resolution to QML for poll logging. */
    int rdpWidth() const { return m_rdpWidth; }
    int rdpHeight() const { return m_rdpHeight; }
    void setRdpGeometry(int w, int h)
    {
        if (m_rdpWidth != w || m_rdpHeight != h)
        {
            m_rdpWidth = w;
            m_rdpHeight = h;
            emit rdpGeometryChanged();
        }
    }

    /* Called from QML after layout completes. */
    Q_INVOKABLE void startConnection()
    {
        start_rdp_connection();
    }

    Q_INVOKABLE void notifyWindowResized()
    {
        notify_window_resized();
    }

    /* Called from FreeRDP thread when the connection drops. */
    Q_INVOKABLE void notifyDisconnected()
    {
        m_rdpContext = nullptr;
        m_qfClientContext.reset();
        QCoreApplication::quit();
    }

    void setFreeRDP_context(rdpContext* context)
    {
        m_rdpContext = context;
    }

    void set_qfclient_context(std::shared_ptr<qf::client_t> context)
    {
        m_qfClientContext = context;
    }

    /* =====================================================================
     * Frame buffer management — replaces DMA-BUF with CPU staging buffer
     * ===================================================================== */

    /* Called from RDP thread (noop_desktop_resize / my_post_connect).
     * Synchronously updates staging buffer dimensions so copyFrameData()
     * always sees matching m_frameWidth/Height and gdi->stride.
     * Only touches POD members and std::vector — safe for cross-thread call. */
    void resizeStagingBuffer(uint32_t w, uint32_t h)
    {
        if (w == m_frameWidth && h == m_frameHeight && !m_frameBuffer.empty())
            return;

        m_frameWidth = w;
        m_frameHeight = h;
        m_frameBuffer.resize(static_cast<size_t>(w) * h * 4); // BGRA, 4 Bpp
        
        const size_t size = static_cast<size_t>(w) * h * 4;
        for (size_t i = 3; i < size; i += 4)
            m_frameBuffer[i] = 0xFF;

        m_texInitNeeded.store(true, std::memory_order_release);

        qf::log::info("view/frame", "staging buffer resized to {}x{}", w, h);
    }

    /* Called from GUI thread (via invokeMethod after resizeStagingBuffer).
     * Sets up the beforeRendering connection and triggers a scene graph
     * redraw so the new texture gets created and uploaded. */
    void notifyFrameResized()
    {
        ensureRenderConnected();
        update();
    }

    /* Connect beforeRendering to upload frame data to GL texture.
     * Called once when the first frame buffer is allocated. */
    void ensureRenderConnected()
    {
        if (window() && !m_texConnected.load(std::memory_order_acquire))
        {
            QObject::connect(
                window(), &QQuickWindow::beforeRendering, this,
                [this]()
                {
                    /* Runs on the render thread with GL context bound. */
                    uploadTextureOnRenderThread();
                },
                Qt::DirectConnection);
            m_texConnected.store(true, std::memory_order_release);
        }
    }

    /* Called from FreeRDP thread (noop_end_paint) — copies dirty rect from
     * GDI buffer to staging buffer while the frame data is still stable.
     * Only touches m_frameBuffer (non-Qt memory), safe for cross-thread call. */
    void copyFrameData(const uint8_t* srcBuffer, uint32_t srcStride,
                       int rx, int ry, int rw, int rh)
    {
        if (!srcBuffer || !m_frameWidth || !m_frameHeight || m_frameBuffer.empty())
            return;

        /* Clamp dirty rect to frame dimensions */
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > static_cast<int>(m_frameWidth))  rw = m_frameWidth - rx;
        if (ry + rh > static_cast<int>(m_frameHeight)) rh = m_frameHeight - ry;

        if (rw <= 0 || rh <= 0)
            return;

        const int bpp = 4; // BGRA, 4 bytes per pixel
        uint8_t* dst = m_frameBuffer.data();
        const uint32_t dstStride = m_frameWidth * bpp;

        /* Copy dirty rect row by row (GDI stride may differ from full-frame stride) */
        for (int y = 0; y < rh; y++)
        {
            memcpy(dst + (static_cast<size_t>(ry) + y) * dstStride + static_cast<size_t>(rx) * bpp,
                   srcBuffer + (static_cast<size_t>(ry) + y) * srcStride + static_cast<size_t>(rx) * bpp,
                   static_cast<size_t>(rw) * bpp);
        }

        /* Mark dirty region for render thread upload */
        m_dirtyX = rx;
        m_dirtyY = ry;
        m_dirtyW = rw;
        m_dirtyH = rh;
        m_uploadNeeded.store(true, std::memory_order_release);
    }

    /* Called from Qt main thread (via QueuedConnection from noop_end_paint).
     * Data was already copied by copyFrameData on the FreeRDP thread. */
    void updateGdiFrame(rdpGdi* /*gdi*/, int /*rx*/, int /*ry*/, int /*rw*/, int /*rh*/)
    {
        /* No data copy needed — copyFrameData ran on the FreeRDP thread.
         * Just trigger a scene graph update so the render thread uploads. */
        update();
    }

    void clearFrame()
    {
        update();
    }

    /* =====================================================================
     * Scene graph rendering (sync phase, GUI thread)
     *
     * Creates a RenderNode that draws the GL texture inside the scene
     * graph's render pass (where native GL commands are valid).
     * ===================================================================== */
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             QQuickItem::UpdatePaintNodeData*) override
    {
        auto* node = static_cast<RenderNode*>(oldNode);

        if (!m_frameWidth || !m_frameHeight || boundingRect().isEmpty())
        {
            if (boundingRect().isEmpty()) {
                qf::log::info("view/paint", "updatePaintNode: boundingRect empty, skipping");
            }
            delete node;
            return nullptr;
        }

        GLuint tex = m_glTexture.load(std::memory_order_acquire);
        if (!tex)
        {
            /* Texture not yet created — skip rendering until uploadTextureOnRenderThread runs */
            delete node;
            return nullptr;
        }

        if (!node)
        {
            node = new RenderNode();
            node->setFlag(QSGNode::OwnedByParent, false);
            qf::log::info("view/paint",
                "created node: rect={}x{} tex={}",
                static_cast<int>(boundingRect().width()),
                static_cast<int>(boundingRect().height()), tex);
        }

        node->glTexture = &m_glTexture;
        node->program   = &m_program;
        node->vbo       = &m_vbo;

        /* 物理像素：boundingRect 返回设备无关像素，乘以 devicePixelRatio
         * 得到真实物理像素，确保 glViewport 覆盖整个帧缓冲。 */
        QQuickWindow* vpWin = window();
        double vpDpr = vpWin ? vpWin->devicePixelRatio() : 1.0;
        int rectW = static_cast<int>(std::round(boundingRect().width() * vpDpr));
        int rectH = static_cast<int>(std::round(boundingRect().height() * vpDpr));
        qf::log::debug("view/paint",
            "updatePaintNode: boundingRect={:.0f}x{:.0f} dpr={} vp={}x{}",
            boundingRect().width(), boundingRect().height(), vpDpr, rectW, rectH);
        node->vpW = rectW;
        node->vpH = rectH;

        return node;
    }

    /* =====================================================================
     * Render-thread handler (called from beforeRendering signal)
     *
     * Creates the GL texture on first call, then uploads dirty frame
     * regions via glTexSubImage2D on subsequent calls.
     * ===================================================================== */
    void uploadTextureOnRenderThread()
    {
        if (!m_frameWidth || !m_frameHeight || m_frameBuffer.empty())
            return;

        /* First-time: create GL texture, shader program, VBO */
        if (m_texInitNeeded.load(std::memory_order_acquire))
        {
            /* Delete old GL resources if any */
            GLuint oldTex = m_glTexture.exchange(0, std::memory_order_acq_rel);
            GLuint oldProg = m_program.exchange(0, std::memory_order_acq_rel);
            GLuint oldVbo = m_vbo.exchange(0, std::memory_order_acq_rel);
            if (oldTex) glDeleteTextures(1, &oldTex);
            if (oldProg) glDeleteProgram(oldProg);
            if (oldVbo) glDeleteBuffers(1, &oldVbo);

            /* Create new GL texture */
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         static_cast<GLsizei>(m_frameWidth),
                         static_cast<GLsizei>(m_frameHeight),
                         0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            /* Build shader program */
            GLuint prog = buildQuadProgram();

            /* Create VBO */
            GLuint vbo = 0;
            {
                const float verts[] = {
                    -1.0f, -1.0f,  0.0f, 1.0f,
                     1.0f, -1.0f,  1.0f, 1.0f,
                    -1.0f,  1.0f,  0.0f, 0.0f,
                     1.0f,  1.0f,  1.0f, 0.0f
                };
                glGenBuffers(1, &vbo);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
            }

            m_glTexture.store(tex, std::memory_order_release);
            m_program.store(prog, std::memory_order_release);
            m_vbo.store(vbo, std::memory_order_release);
            m_texInitNeeded.store(false, std::memory_order_release);

            qf::log::info("view/gl", "GL texture {}x{} created on render thread",
                          m_frameWidth, m_frameHeight);
        }

        /* Upload full frame from staging buffer.
         * NOTE: The staging buffer uses full-frame stride (m_frameWidth * 4).
         * Uploading the entire frame avoids stride mismatch issues that would
         * occur if we uploaded only the dirty rect (GL_UNPACK_ROW_LENGTH is
         * needed for sub-rect upload with full-frame stride). */
        if (m_uploadNeeded.load(std::memory_order_acquire))
        {
            GLuint tex = m_glTexture.load(std::memory_order_acquire);
            if (tex && m_frameWidth > 0 && m_frameHeight > 0)
            {
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                                0, 0,
                                static_cast<GLsizei>(m_frameWidth),
                                static_cast<GLsizei>(m_frameHeight),
                                GL_BGRA_EXT, GL_UNSIGNED_BYTE,
                                m_frameBuffer.data());
            }
            m_uploadNeeded.store(false, std::memory_order_release);
        }
    }

    /* =====================================================================
     * Shader helpers
     * ===================================================================== */

    static GLuint compileShader(GLenum type, const char* source)
    {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[512];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            qf::log::error("render/shader", "shader compile error ({}): {}",
                           type == GL_VERTEX_SHADER ? "VS" : "FS", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    static GLuint buildQuadProgram()
    {
        const char* vsSrc = R"(
            #version 300 es
            in vec2 aPos;
            in vec2 aTexCoord;
            out vec2 vTexCoord;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
                vTexCoord = aTexCoord;
            }
        )";
        const char* fsSrc = R"(
            #version 300 es
            precision mediump float;
            in vec2 vTexCoord;
            uniform sampler2D uTexture;
            out vec4 fragColor;
            void main() {
                fragColor = texture(uTexture, vTexCoord);
                fragColor.a = 1.0;
            }
        )";

        GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
        if (!vs || !fs)
        {
            glDeleteShader(vs);
            glDeleteShader(fs);
            return 0;
        }

        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);

        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[512];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            qf::log::error("render/shader", "program link error: {}", log);
            glDeleteProgram(program);
            glDeleteShader(vs);
            glDeleteShader(fs);
            return 0;
        }

        glDetachShader(program, vs);
        glDetachShader(program, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);

        return program;
    }

    /* =====================================================================
     * Render-thread cleanup job
     * ===================================================================== */
    class GLResourceCleanupJob : public QRunnable
    {
    public:
        GLResourceCleanupJob(std::atomic<GLuint>& texture,
                             std::atomic<GLuint>& prog,
                             std::atomic<GLuint>& buf)
            : m_texture(texture), m_program(prog), m_vbo(buf)
        {}

        void run() override
        {
            GLuint tex = m_texture.exchange(0, std::memory_order_acq_rel);
            GLuint prog = m_program.exchange(0, std::memory_order_acq_rel);
            GLuint vbo = m_vbo.exchange(0, std::memory_order_acq_rel);

            if (tex) glDeleteTextures(1, &tex);
            if (prog) glDeleteProgram(prog);
            if (vbo) glDeleteBuffers(1, &vbo);
        }

    private:
        std::atomic<GLuint>& m_texture;
        std::atomic<GLuint>& m_program;
        std::atomic<GLuint>& m_vbo;
    };

    /* =====================================================================
     * Mouse / Keyboard / Clipboard (unchanged)
     * ===================================================================== */

    std::string get_mouse_flags_string(UINT16 flags) {
        std::string buffer{};
        if (flags & PTR_FLAGS_MOVE) buffer += "MOVE, ";
        if (flags & PTR_FLAGS_DOWN) buffer += "DOWN, ";
        if (flags & PTR_FLAGS_BUTTON1) buffer += "BUTTON1 (Left), ";
        if (flags & PTR_FLAGS_BUTTON2) buffer += "BUTTON2 (Right), ";
        if (flags & PTR_FLAGS_BUTTON3) buffer += "BUTTON3 (Middle), ";
        if (flags & PTR_FLAGS_WHEEL) buffer += "WHEEL, ";
        if (flags & PTR_FLAGS_HWHEEL) buffer += "HWHEEL, ";
        return buffer;
    }

    void mouseEventScaleSend(uint32_t mouse_x, uint32_t mouse_y, uint16_t freerdp_mouse_event) {
        if(!m_rdpContext) return;
        uint32_t host_w = freerdp_settings_get_uint32(m_rdpContext->settings, FreeRDP_DesktopWidth);
        uint32_t host_h = freerdp_settings_get_uint32(m_rdpContext->settings, FreeRDP_DesktopHeight);
        uint32_t map_x = mouse_x * host_w / width();
        uint32_t map_y = mouse_y * host_h / height();
        if (!freerdp_input_send_mouse_event(m_rdpContext->input, freerdp_mouse_event, map_x, map_y))
            qf::log::warn("input/mouse", "failed to send mouse event flags={} x={} y={}",
                          freerdp_mouse_event, map_x, map_y);
    }

    void mousePressEvent(QMouseEvent* event) override {
        uint16_t flags = (event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN : PTR_FLAGS_BUTTON2 | PTR_FLAGS_DOWN;
        mouseEventScaleSend(event->position().x(), event->position().y(), flags);
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        uint16_t flags = (event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 : PTR_FLAGS_BUTTON2;
        mouseEventScaleSend(event->position().x(), event->position().y(), flags);
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        rdp_notify_mouse_moved(event->position().x(), event->position().y());
        mouseEventScaleSend(event->position().x(), event->position().y(), PTR_FLAGS_MOVE);
        event->accept();
    }
    void hoverMoveEvent(QHoverEvent* event) override {
        rdp_notify_mouse_moved(event->position().x(), event->position().y());
        mouseEventScaleSend(event->position().x(), event->position().y(), PTR_FLAGS_MOVE);
        event->accept();
    }
    void wheelEvent(QWheelEvent* event) override {
        int delta = event->angleDelta().y();
        uint16_t flags = PTR_FLAGS_WHEEL;
        if (delta < 0) { flags |= PTR_FLAGS_WHEEL_NEGATIVE; delta = -delta; }
        flags |= static_cast<uint16_t>(delta) & WheelRotationMask;
        mouseEventScaleSend(event->position().x(), event->position().y(), flags);
        event->accept();
    }

    void keyboardUnicodeEventSend(QKeyEvent* event, bool down) {
        if (!m_rdpContext) return;
        UINT32 freerdp_key_code = qf::to_freerdp_key_code(event);
        if (freerdp_key_code == RDP_SCANCODE_UNKNOWN) {
            uint16_t flags = down ? 0 : KBD_FLAGS_RELEASE;
            if (!event->text().isEmpty())
                freerdp_input_send_unicode_keyboard_event(m_rdpContext->input, flags, event->text().unicode()->unicode());
            return;
        }
        freerdp_input_send_keyboard_event_ex(m_rdpContext->input, down,
                                             down && event->isAutoRepeat(), freerdp_key_code);
    }

    void keyPressEvent(QKeyEvent* event) override { keyboardUnicodeEventSend(event, true); event->accept(); }
    void keyReleaseEvent(QKeyEvent* event) override { keyboardUnicodeEventSend(event, false); event->accept(); }

    /* Send Ctrl+Alt+Delete to the RDP server (from toolbar button). */
    Q_INVOKABLE void sendCtrlAltDelete()
    {
        if (!m_rdpContext || !m_rdpContext->input)
            return;
        rdpInput* input = m_rdpContext->input;
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LCONTROL);
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LMENU);
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_DELETE);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_DELETE);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LMENU);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LCONTROL);
    }

    static QImage imageFromDib(const QByteArray& dib) {
        if (dib.size() < 40) return {};
        const uchar* bytes = reinterpret_cast<const uchar*>(dib.constData());
        const quint32 headerSize = qFromLittleEndian<quint32>(bytes);
        if (headerSize < 12 || static_cast<qsizetype>(headerSize) > dib.size()) return {};
        quint16 bitCount = 0; quint32 compression = 0; quint32 colorUsed = 0;
        if (headerSize >= 40 && dib.size() >= 40) {
            bitCount = qFromLittleEndian<quint16>(bytes + 14);
            compression = qFromLittleEndian<quint32>(bytes + 16);
            colorUsed = qFromLittleEndian<quint32>(bytes + 32);
        }
        quint32 colorTableBytes = 0;
        if (colorUsed > 0) colorTableBytes = colorUsed * 4;
        else if (bitCount > 0 && bitCount <= 8) colorTableBytes = (1u << bitCount) * 4;
        const quint32 bitfieldsBytes = (headerSize == 40 && compression == 3) ? 12 : 0;
        const quint32 pixelOffset = 14 + headerSize + bitfieldsBytes + colorTableBytes;
        const quint32 fileSize = 14 + static_cast<quint32>(dib.size());
        QByteArray bmp; bmp.reserve(static_cast<qsizetype>(fileSize));
        bmp.append('B'); bmp.append('M');
        auto appendLe16 = [&bmp](quint16 v) { char buf[2]; qToLittleEndian(v, (uchar*)buf); bmp.append(buf, 2); };
        auto appendLe32 = [&bmp](quint32 v) { char buf[4]; qToLittleEndian(v, (uchar*)buf); bmp.append(buf, 4); };
        appendLe32(fileSize); appendLe16(0); appendLe16(0); appendLe32(pixelOffset);
        bmp.append(dib);
        return QImage::fromData(bmp, "BMP");
    }

    void updateClipboardFilesFromRemote(const std::vector<QString>& paths) {
        if (!m_qfClientContext || !m_qfClientContext->cliprdr_client_context_) return;
        if (paths.empty()) return;
        QList<QUrl> urls;
        for (const auto& path : paths)
            if (QFileInfo::exists(path)) urls.append(QUrl::fromLocalFile(path));
        if (urls.empty()) return;
        m_clipboardDataFromRemote = true;
        auto* data = new QMimeData(); data->setUrls(urls);
        QGuiApplication::clipboard()->setMimeData(data);
        m_clipboardDataFromRemote = false;
    }

    void updateClipboardDataFromRemote(const QByteArray& data, uint32_t formatId, const QString& formatName) {
        if (!m_qfClientContext || !m_qfClientContext->cliprdr_client_context_) return;
        m_clipboardDataFromRemote = true;
        if (formatName == QStringLiteral("PNG")) {
            QImage image = QImage::fromData(data, "PNG");
            if (!image.isNull()) QGuiApplication::clipboard()->setImage(image);
        } else if (formatId == CF_DIB || formatId == CF_DIBV5) {
            QImage image = imageFromDib(data);
            if (!image.isNull()) QGuiApplication::clipboard()->setImage(image);
        } else if(formatId == CF_UNICODETEXT) {
            qsizetype charCount = data.size() / static_cast<qsizetype>(sizeof(char16_t));
            const char16_t* textData = reinterpret_cast<const char16_t*>(data.constData());
            if (charCount > 0 && textData[charCount - 1] == u'\0') --charCount;
            QGuiApplication::clipboard()->setText(QString::fromUtf16(textData, charCount));
        }
        m_clipboardDataFromRemote = false;
    }

    QString clipboardDisplayName(const QFileInfo& root, const QFileInfo& fileInfo) const {
        QString displayName = root.fileName();
        if (root.absoluteFilePath() != fileInfo.absoluteFilePath()) {
            QDir rootDir(root.absoluteFilePath());
            displayName += "/" + rootDir.relativeFilePath(fileInfo.absoluteFilePath());
        }
        displayName.replace("/", "\\"); return displayName;
    }

    void appendClipboardInfoFile(const QFileInfo& root, const QFileInfo& fileInfo) {
        qf::clipboard_info_file_t c;
        c.display_name_ = clipboardDisplayName(root, fileInfo);
        c.local_path_ = fileInfo.absoluteFilePath(); c.total_ = fileInfo.size(); c.is_directory_ = fileInfo.isDir();
        auto it = std::find_if(m_qfClientContext->clipboard_info_files_.begin(),
            m_qfClientContext->clipboard_info_files_.end(),
            [&](const qf::clipboard_info_file_t& f){ return f.local_path_ == fileInfo.absoluteFilePath(); });
        if (it == m_qfClientContext->clipboard_info_files_.end())
            m_qfClientContext->clipboard_info_files_.push_back(c);
        if (!fileInfo.isDir()) return;
        QDir dir(fileInfo.absoluteFilePath());
        for (const QString& f : dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
            appendClipboardInfoFile(root, QFileInfo(dir.filePath(f)));
    }

    void dataChangedCallback() {
        if (!m_qfClientContext) return;
        auto& clipboardContext = m_qfClientContext->cliprdr_client_context_;
        if(!clipboardContext) return;
        if (m_clipboardDataFromRemote) return;
        auto RemoteClipboardFormatList = [&,this](uint32_t fid, const char* fname) {
            CLIPRDR_FORMAT_LIST fl = {}; CLIPRDR_FORMAT f = {};
            f.formatId = fid; f.formatName = const_cast<char*>(fname);
            fl.numFormats = 1; fl.formats = &f;
            clipboardContext->ClientFormatList(clipboardContext, &fl);
        };
        QClipboard* cb = QGuiApplication::clipboard();
        const QMimeData* md = cb->mimeData();
        if (md->hasUrls()) {
            QByteArray uriList; m_qfClientContext->clipboard_info_files_.clear();
            for (const QUrl& url : md->urls()) {
                if (url.isLocalFile()) {
                    QFileInfo file(url.toLocalFile());
                    if (!file.isDir() && !file.isFile()) continue;
                    appendClipboardInfoFile(file, file);
                    uriList.append(url.toEncoded()); uriList.append('\n');
                }
            }
            if (m_qfClientContext->clipboard_info_files_.empty()) return;
            if (m_qfClientContext->cliprdr_file_context_) {
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, TRUE);
                cliprdr_file_context_update_client_data(m_qfClientContext->cliprdr_file_context_,
                    uriList.constData(), static_cast<size_t>(uriList.size()));
            }
            RemoteClipboardFormatList(qf::CLIPBOARD_FORMAT_FILE, qf::CLIPBOARD_FORMAT_FILE_NAME);
        } else if (md->hasText()) {
            if (m_qfClientContext->cliprdr_file_context_)
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, FALSE);
            RemoteClipboardFormatList(CF_UNICODETEXT, nullptr);
        } else if (md->hasImage()) {
            if (m_qfClientContext->cliprdr_file_context_)
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, FALSE);
            CLIPRDR_FORMAT_LIST fl = {}; CLIPRDR_FORMAT formats[2] = {};
            formats[0].formatId = qf::CLIPBOARD_FORMAT_PNG; formats[0].formatName = const_cast<char*>("PNG");
            formats[1].formatId = CF_DIB; formats[1].formatName = nullptr;
            fl.numFormats = 2; fl.formats = formats;
            clipboardContext->ClientFormatList(clipboardContext, &fl);
        }
    }

signals:
    void rdpGeometryChanged();
    void clipboardDataResponseFromRemote();
    void fullscreenChanged();

private:
    /* Frame buffer — CPU-side copy of the decoded frame, uploaded to GL texture */
    std::vector<uint8_t> m_frameBuffer;
    uint32_t             m_frameWidth  = 0;
    uint32_t             m_frameHeight = 0;

    /* Dirty rect tracking for incremental texture upload */
    std::atomic<bool>    m_uploadNeeded{false};
    int                  m_dirtyX = 0, m_dirtyY = 0, m_dirtyW = 0, m_dirtyH = 0;

    /* GL resources — atomically published for render thread access */
    std::atomic<GLuint>  m_glTexture{0};
    std::atomic<GLuint>  m_program{0};
    std::atomic<GLuint>  m_vbo{0};
    std::atomic<bool>    m_texInitNeeded{true};
    std::atomic<bool>    m_texConnected{false};

    int                               m_rdpWidth = 0;
    int                               m_rdpHeight = 0;
    rdpContext*                       m_rdpContext = nullptr;
    std::shared_ptr<qf::client_t>     m_qfClientContext;
    bool                              m_clipboardDataFromRemote = false;
    bool                              m_fullscreen = false;
};
