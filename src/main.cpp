#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <string>
#include <chrono>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {
    constexpr wchar_t kWindowClassName[] = L"DynamicIslandWindowClass";
    constexpr UINT_PTR kAnimationTimerId = 1;
    constexpr UINT kAnimationIntervalMs = 16;
    constexpr UINT kTrayIconId = 1001;
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT kTrayMenuExit = 40001;
    constexpr float kCollapsedWidth = 220.0f;
    constexpr float kCollapsedHeight = 54.0f;
    constexpr float kExpandedMinWidth = 420.0f;
    constexpr float kExpandedMinHeight = 176.0f;
    constexpr float kCornerRadius = 26.0f;
    constexpr int kTopMargin = 18;

    template<typename T>
    void SafeRelease(T **value) {
        if (*value) {
            (*value)->Release();
            *value = nullptr;
        }
    }

    struct ColorSet {
        D2D1_COLOR_F background{0.07f, 0.08f, 0.10f, 0.96f};
        D2D1_COLOR_F border{0.26f, 0.30f, 0.36f, 0.90f};
        D2D1_COLOR_F accent{0.26f, 0.66f, 0.98f, 1.0f};
        D2D1_COLOR_F textPrimary{0.96f, 0.97f, 0.98f, 1.0f};
        D2D1_COLOR_F textSecondary{0.69f, 0.74f, 0.80f, 1.0f};
        D2D1_COLOR_F glow{0.19f, 0.56f, 0.98f, 0.20f};
    };

    class IslandApp {
    public:
        ~IslandApp() {
            RemoveTrayIcon();
            DiscardGraphicsResources();
            SafeRelease(&m_writeFactory);
            SafeRelease(&m_d2dFactory);
        }

        HRESULT Initialize(HINSTANCE instance) {
            m_instance = instance;

            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

            HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
            if (FAILED(hr)) {
                return hr;
            }

            hr = DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown **>(&m_writeFactory));
            if (FAILED(hr)) {
                return hr;
            }

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &IslandApp::WndProc;
            wc.hInstance = m_instance;
            wc.lpszClassName = kWindowClassName;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

            RegisterClassExW(&wc);

            RECT workArea{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

            const int startWidth = static_cast<int>(kCollapsedWidth);
            const int startHeight = static_cast<int>(kCollapsedHeight);
            const int startX = workArea.left + ((workArea.right - workArea.left) - startWidth) / 2;
            const int startY = workArea.top + kTopMargin;

            m_hwnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                kWindowClassName,
                L"Dynamic Island",
                WS_POPUP,
                startX,
                startY,
                startWidth,
                startHeight,
                nullptr,
                nullptr,
                m_instance,
                this);

            if (!m_hwnd) {
                return HRESULT_FROM_WIN32(GetLastError());
            }

            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            AddTrayIcon();
            ShowWindow(m_hwnd, SW_SHOWNORMAL);
            UpdateWindow(m_hwnd);
            return S_OK;
        }

        static int Run() {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            return static_cast<int>(msg.wParam);
        }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
            IslandApp *app = nullptr;

            if (message == WM_NCCREATE) {
                auto *createStruct = reinterpret_cast<CREATESTRUCTW *>(lParam);
                app = reinterpret_cast<IslandApp *>(createStruct->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
                app->m_hwnd = hwnd;
            } else {
                app = reinterpret_cast<IslandApp *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            }

            return app
                       ? app->HandleMessage(message, wParam, lParam)
                       : DefWindowProcW(hwnd, message, wParam, lParam);
        }

        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
            switch (message) {
                case WM_CREATE:
                    m_lastAnimationTick = std::chrono::steady_clock::now();
                    SetTimer(m_hwnd, kAnimationTimerId, kAnimationIntervalMs, nullptr);
                    return 0;
                case WM_TIMER:
                    if (wParam == kAnimationTimerId) {
                        OnAnimationFrame();
                    }
                    return 0;
                case WM_MOUSEMOVE:
                    OnMouseMove();
                    return 0;
                case WM_MOUSELEAVE:
                    m_hovered = false;
                    m_trackingMouse = false;
                    m_targetProgress = 0.0f;
                    return 0;
                case kTrayMessage:
                    return OnTrayIconMessage(lParam);
                case WM_COMMAND:
                    if (LOWORD(wParam) == kTrayMenuExit) {
                        DestroyWindow(m_hwnd);
                        return 0;
                    }
                    break;
                case WM_DPICHANGED: {
                    auto *suggested = reinterpret_cast<RECT *>(lParam);
                    SetWindowPos(
                        m_hwnd, nullptr,
                        suggested->left, suggested->top,
                        suggested->right - suggested->left,
                        suggested->bottom - suggested->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                    return 0;
                }
                case WM_SIZE:
                    if (m_renderTarget) {
                        const auto width = LOWORD(lParam);
                        const auto height = HIWORD(lParam);
                        m_renderTarget->Resize(D2D1::SizeU(width, height));
                    }
                    UpdateRegion();
                    return 0;
                case WM_PAINT:
                case WM_DISPLAYCHANGE:
                    OnPaint();
                    return 0;
                case WM_DESTROY:
                    KillTimer(m_hwnd, kAnimationTimerId);
                    RemoveTrayIcon();
                    PostQuitMessage(0);
                    return 0;
                default:
                    return DefWindowProcW(m_hwnd, message, wParam, lParam);
            }

            return DefWindowProcW(m_hwnd, message, wParam, lParam);
        }

        void OnMouseMove() {
            if (!m_trackingMouse) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = m_hwnd;
                TrackMouseEvent(&track);
                m_trackingMouse = true;
            }

            m_hovered = true;
            m_trackingMouse = true;
            m_targetProgress = 1.0f;
        }

        void OnAnimationFrame() {
            const auto now = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration<float>(now - m_lastAnimationTick).count();
            m_lastAnimationTick = now;
            deltaTime = std::clamp(deltaTime, 0.001f, 0.05f);

            if (std::fabs(m_targetProgress - m_progress) < 0.001f && std::fabs(m_velocity) < 0.001f) {
                m_progress = m_targetProgress;
                m_velocity = 0.0f;
                return;
            }

            const float smoothTime = (m_targetProgress > m_progress) ? 0.24f : 0.30f;
            m_progress = SmoothDamp(m_progress, m_targetProgress, m_velocity, smoothTime, deltaTime);
            m_progress = std::clamp(m_progress, 0.0f, 1.0f);

            if (std::fabs(m_targetProgress - m_progress) < 0.0015f && std::fabs(m_velocity) < 0.003f) {
                m_progress = m_targetProgress;
                m_velocity = 0.0f;
            }

            UpdateWindowBounds();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }

        void UpdateWindowBounds() const {
            RECT workArea{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

            const auto expandedSize = MeasureExpandedSize();
            const float eased = EaseOutCubic(m_progress);
            const float width = Lerp(kCollapsedWidth, expandedSize.width, eased);
            const float height = Lerp(kCollapsedHeight, expandedSize.height, eased);

            const int w = static_cast<int>(std::round(width));
            const int h = static_cast<int>(std::round(height));
            const int x = workArea.left + ((workArea.right - workArea.left) - w) / 2;
            const int y = workArea.top + kTopMargin;

            SetWindowPos(
                m_hwnd,
                HWND_TOPMOST,
                x,
                y,
                w,
                h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        static D2D1_SIZE_F MeasureExpandedSize() {
            const std::wstring timeText = BuildTimeText();
            const std::wstring detailText = BuildDetailText();
            const std::wstring statusText = L"Now";
            const std::wstring actionText = L"Native C++";
            const std::wstring fpsText = L"Smooth Hover";

            const float titleWidth = MeasureTextWidth(timeText, 22.0f, true);
            const float detailWidth = MeasureMultilineTextWidth(detailText, 14.0f);
            const float footerWidth = MeasureTextWidth(statusText, 13.0f, true) +
                                      MeasureTextWidth(actionText, 13.0f, true) +
                                      MeasureTextWidth(fpsText, 13.0f, true) + 92.0f;

            const float contentWidth = std::max({titleWidth + 96.0f, detailWidth + 48.0f, footerWidth + 48.0f});
            const float width = std::max(kExpandedMinWidth, contentWidth);

            const float detailHeight = MeasureMultilineTextHeight(detailText, 14.0f, width - 64.0f);
            const float height = std::max(kExpandedMinHeight, 94.0f + detailHeight + 42.0f);

            return D2D1::SizeF(width, height);
        }

        void UpdateRegion() const {
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            HRGN region = CreateRoundRectRgn(
                rc.left,
                rc.top,
                rc.right + 1,
                rc.bottom + 1,
                static_cast<int>(kCornerRadius * 2.0f),
                static_cast<int>(kCornerRadius * 2.0f));
            SetWindowRgn(m_hwnd, region, TRUE);
        }

        HRESULT CreateGraphicsResources() {
            if (m_renderTarget) {
                return S_OK;
            }

            RECT rc{};
            GetClientRect(m_hwnd, &rc);

            const D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
            HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE)),
                D2D1::HwndRenderTargetProperties(m_hwnd, size),
                &m_renderTarget);
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_renderTarget->CreateSolidColorBrush(m_colors.background, &m_backgroundBrush);
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_renderTarget->CreateSolidColorBrush(m_colors.border, &m_borderBrush);
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_renderTarget->CreateSolidColorBrush(m_colors.accent, &m_accentBrush);
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_renderTarget->CreateSolidColorBrush(m_colors.textPrimary, &m_textBrush);
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_renderTarget->CreateSolidColorBrush(m_colors.textSecondary, &m_subtleTextBrush);
            if (FAILED(hr)) {
                return hr;
            }


            hr = m_writeFactory->CreateTextFormat(
                L"Segoe UI Variable Display",
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                22.0f,
                L"zh-cn",
                &m_titleFormat);
            if (FAILED(hr)) {
                hr = m_writeFactory->CreateTextFormat(
                    L"Segoe UI",
                    nullptr,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    22.0f,
                    L"zh-cn",
                    &m_titleFormat);
            }
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_writeFactory->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                14.0f,
                L"zh-cn",
                &m_bodyFormat);
            if (FAILED(hr)) {
                return hr;
            }

            m_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            m_bodyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            return S_OK;
        }

        void DiscardGraphicsResources() {
            SafeRelease(&m_subtleTextBrush);
            SafeRelease(&m_textBrush);
            SafeRelease(&m_accentBrush);
            SafeRelease(&m_borderBrush);
            SafeRelease(&m_backgroundBrush);
            SafeRelease(&m_titleFormat);
            SafeRelease(&m_bodyFormat);
            SafeRelease(&m_renderTarget);
        }

        void OnPaint() {
            HRESULT hr = CreateGraphicsResources();
            if (FAILED(hr)) {
                return;
            }

            PAINTSTRUCT ps{};
            BeginPaint(m_hwnd, &ps);

            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            const auto width = static_cast<float>(rc.right - rc.left);
            const auto height = static_cast<float>(rc.bottom - rc.top);
            const float eased = EaseOutCubic(m_progress);

            m_renderTarget->BeginDraw();
            m_renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

            const auto rect = D2D1::RoundedRect(
                D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f),
                kCornerRadius,
                kCornerRadius);



            m_backgroundBrush->SetColor(m_colors.background);
            m_borderBrush->SetColor(m_colors.border);
            // m_glowBrush->SetColor(
            //     D2D1::ColorF(m_colors.glow.r, m_colors.glow.g, m_colors.glow.b, 0.12f + eased * 0.20f));

            m_renderTarget->FillRoundedRectangle(rect, m_backgroundBrush);
            m_renderTarget->DrawRoundedRectangle(rect, m_borderBrush, 1.2f);

            DrawCollapsedContent(width, height, eased);
            DrawExpandedCard(width, height, eased);

            hr = m_renderTarget->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) {
                DiscardGraphicsResources();
            }

            EndPaint(m_hwnd, &ps);
        }

        void DrawCollapsedContent(float width, float height, float eased) const {
            const float opacity = 1.0f - SmoothStep(0.08f, 0.42f, eased);
            if (opacity <= 0.01f) {
                return;
            }

            const auto timeText = BuildTimeText();
            const float yOffset = -10.0f * SmoothStep(0.0f, 0.45f, eased);

            m_textBrush->SetOpacity(opacity);
            m_subtleTextBrush->SetOpacity(opacity);
            m_accentBrush->SetOpacity(opacity);

            const D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(34.0f, (height / 2.0f) + yOffset), 6.0f, 6.0f);
            m_renderTarget->FillEllipse(dot, m_accentBrush);

            const D2D1_RECT_F titleRect = D2D1::RectF(52.0f, 12.0f + yOffset, width - 18.0f, height - 10.0f + yOffset);
            m_renderTarget->DrawTextW(
                timeText.c_str(),
                static_cast<UINT32>(timeText.size()),
                m_titleFormat,
                titleRect,
                m_textBrush);
        }

        void DrawExpandedCard(float width, float height, float eased) const {
            const float opacity = SmoothStep(0.22f, 0.82f, eased);
            if (opacity <= 0.01f) {
                return;
            }

            const auto timeText = BuildTimeText();
            const auto detailText = BuildDetailText();
            const float enterOffset = (1.0f - opacity) * 12.0f;
            constexpr float cardLeft = 24.0f;
            const float cardTop = 64.0f + enterOffset;
            const float cardRight = width - 24.0f;
            const float cardBottom = height - 18.0f;
            constexpr float footerHeight = 28.0f;
            const float footerTop = cardBottom - footerHeight;

            m_textBrush->SetOpacity(opacity);
            m_subtleTextBrush->SetOpacity(opacity);
            m_accentBrush->SetOpacity(opacity);
            m_borderBrush->SetOpacity(opacity * 0.28f);

            const D2D1_ELLIPSE pulse = D2D1::Ellipse(
                D2D1::Point2F(34.0f, 36.0f + enterOffset * 0.35f),
                7.0f + eased * 2.0f,
                7.0f + eased * 2.0f);
            m_renderTarget->FillEllipse(pulse, m_accentBrush);

            const D2D1_RECT_F titleRect = D2D1::RectF(54.0f, 16.0f + enterOffset * 0.2f, width - 24.0f,
                                                      48.0f + enterOffset * 0.2f);

            const D2D1_RECT_F bodyRect = D2D1::RectF(cardLeft + 14.0f, cardTop + 14.0f, cardRight - 14.0f,
                                                     footerTop - 10.0f);

            m_renderTarget->DrawTextW(
                timeText.c_str(),
                static_cast<UINT32>(timeText.size()),
                m_titleFormat,
                titleRect,
                m_textBrush);

            m_renderTarget->DrawLine(
                D2D1::Point2F(cardLeft, cardTop - 10.0f),
                D2D1::Point2F(cardRight, cardTop - 10.0f),
                m_borderBrush,
                1.0f);

            m_renderTarget->DrawTextW(
                detailText.c_str(),
                static_cast<UINT32>(detailText.size()),
                m_bodyFormat,
                bodyRect,
                m_subtleTextBrush);

            DrawTag(D2D1::RectF(cardLeft, footerTop, cardLeft + 60.0f, footerTop + footerHeight), L"Tag1", opacity);
            DrawTag(D2D1::RectF(cardLeft + 65.0f, footerTop, cardLeft + 125.0f, footerTop + footerHeight),
                    L"Tag2", opacity);
            DrawTag(D2D1::RectF(cardLeft + 130.0f, footerTop, cardLeft + 190.0f, footerTop + footerHeight),
                    L"Tag3", opacity);
        }

        static std::wstring BuildTimeText() {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t buffer[64]{};
            wsprintfW(buffer, L"%02d:%02d", st.wHour, st.wMinute);
            return buffer;
        }

        static std::wstring BuildDetailText() {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t buffer[256]{};
            wsprintfW(
                buffer,
                L"%04d-%02d-%02d\nC++ windows island\nTest Context .",
                st.wYear,
                st.wMonth,
                st.wDay);
            return buffer;
        }

        static float Lerp(float a, float b, float t) {
            return a + (b - a) * t;
        }

        static float SmoothStep(float edge0, float edge1, float value) {
            if (edge0 == edge1) {
                return value < edge0 ? 0.0f : 1.0f;
            }

            const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        static float MeasureTextWidth(const std::wstring &text, float fontSize, bool semiBold) {
            HDC dc = GetDC(nullptr);
            if (!dc) {
                return 0.0f;
            }

            HFONT font = CreateFontW(
                -MulDiv(static_cast<int>(std::round(fontSize)), GetDeviceCaps(dc, LOGPIXELSY), 72),
                0, 0, 0,
                semiBold ? FW_SEMIBOLD : FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                VARIABLE_PITCH, L"Segoe UI");

            auto oldFont = static_cast<HFONT>(SelectObject(dc, font));
            SIZE size{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            SelectObject(dc, oldFont);
            DeleteObject(font);
            ReleaseDC(nullptr, dc);
            return static_cast<float>(size.cx);
        }

        static float MeasureMultilineTextWidth(const std::wstring &text, float fontSize) {
            float maxWidth = 0.0f;
            size_t start = 0;
            while (start <= text.size()) {
                const size_t end = text.find(L'\n', start);
                const std::wstring line = text.substr(
                    start, end == std::wstring::npos ? std::wstring::npos : end - start);
                maxWidth = std::max(maxWidth, MeasureTextWidth(line, fontSize, false));
                if (end == std::wstring::npos) {
                    break;
                }
                start = end + 1;
            }
            return maxWidth;
        }

        static float MeasureMultilineTextHeight(const std::wstring &text, float fontSize, float availableWidth) {
            HDC dc = GetDC(nullptr);
            if (!dc) {
                return 0.0f;
            }

            HFONT font = CreateFontW(
                -MulDiv(static_cast<int>(std::round(fontSize)), GetDeviceCaps(dc, LOGPIXELSY), 72),
                0, 0, 0,
                FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                VARIABLE_PITCH, L"Segoe UI");

            auto oldFont = static_cast<HFONT>(SelectObject(dc, font));
            RECT rc{0, 0, static_cast<LONG>(std::ceil(availableWidth)), 0};
            DrawTextW(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            DeleteObject(font);
            ReleaseDC(nullptr, dc);
            return static_cast<float>(rc.bottom - rc.top);
        }

        static float SmoothDamp(float current, float target, float &currentVelocity, float smoothTime,
                                float deltaTime) {
            smoothTime = std::max(0.0001f, smoothTime);
            const float omega = 2.0f / smoothTime;
            const float x = omega * deltaTime;
            const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
            const float change = current - target;
            const float temp = (currentVelocity + omega * change) * deltaTime;

            currentVelocity = (currentVelocity - omega * temp) * exp;
            return target + (change + temp) * exp;
        }

        static float EaseOutCubic(float value) {
            const float inv = 1.0f - value;
            return 1.0f - (inv * inv * inv);
        }

        void DrawTag(const D2D1_RECT_F &rect, const wchar_t *text, float opacity) const {
            const auto chip = D2D1::RoundedRect(rect, 12.0f, 12.0f);
            m_backgroundBrush->SetColor(D2D1::ColorF(0.11f, 0.13f, 0.16f, 0.92f));
            m_backgroundBrush->SetOpacity(opacity);
            m_borderBrush->SetColor(D2D1::ColorF(0.22f, 0.27f, 0.33f, 0.85f));
            m_borderBrush->SetOpacity(opacity);
            m_accentBrush->SetOpacity(opacity * 0.9f);
            m_renderTarget->FillRoundedRectangle(chip, m_backgroundBrush);
            m_renderTarget->DrawRoundedRectangle(chip, m_borderBrush, 1.0f);
            m_renderTarget->DrawTextW(
                text,
                static_cast<UINT32>(wcslen(text)),
                m_bodyFormat,
                D2D1::RectF(rect.left + 10.0f, rect.top + 4.0f, rect.right - 8.0f, rect.bottom),
                m_accentBrush);
        }

        void AddTrayIcon() {
            m_trayData = {};
            m_trayData.cbSize = sizeof(m_trayData);
            m_trayData.hWnd = m_hwnd;
            m_trayData.uID = kTrayIconId;
            m_trayData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            m_trayData.uCallbackMessage = kTrayMessage;
            m_trayData.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wcscpy_s(m_trayData.szTip, L"cpp dynamic island");
            Shell_NotifyIconW(NIM_ADD, &m_trayData);
            m_hasTrayIcon = true;
        }

        void RemoveTrayIcon() {
            if (!m_hasTrayIcon) {
                return;
            }

            Shell_NotifyIconW(NIM_DELETE, &m_trayData);
            m_trayData.hIcon = nullptr;
            m_hasTrayIcon = false;
        }

        LRESULT OnTrayIconMessage(LPARAM lParam) {
            switch (LOWORD(lParam)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayMenu();
                    return 0;
                case WM_LBUTTONDBLCLK:
                    ToggleVisibility();
                    return 0;
                default:
                    return 0;
            }
        }

        void ShowTrayMenu() {
            HMENU menu = CreatePopupMenu();
            if (!menu) {
                return;
            }

            AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");

            POINT cursor{};
            GetCursorPos(&cursor);
            SetForegroundWindow(m_hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, m_hwnd, nullptr);
            DestroyMenu(menu);
        }

        void ToggleVisibility() {
            if (IsWindowVisible(m_hwnd)) {
                ShowWindow(m_hwnd, SW_HIDE);
                return;
            }

            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

    private:
        HINSTANCE m_instance{};
        HWND m_hwnd{};
        NOTIFYICONDATAW m_trayData{};
        ID2D1Factory *m_d2dFactory{};
        IDWriteFactory *m_writeFactory{};
        ID2D1HwndRenderTarget *m_renderTarget{};
        ID2D1SolidColorBrush *m_backgroundBrush{};
        ID2D1SolidColorBrush *m_borderBrush{};
        ID2D1SolidColorBrush *m_accentBrush{};
        ID2D1SolidColorBrush *m_textBrush{};
        ID2D1SolidColorBrush *m_subtleTextBrush{};
        IDWriteTextFormat *m_titleFormat{};
        IDWriteTextFormat *m_bodyFormat{};
        ColorSet m_colors{};
        float m_progress = 0.0f;
        float m_targetProgress = 0.0f;
        float m_velocity = 0.0f;
        std::chrono::steady_clock::time_point m_lastAnimationTick{};
        bool m_hasTrayIcon = false;
        bool m_hovered = false;
        bool m_trackingMouse = false;
    };
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    IslandApp app;
    if (FAILED(app.Initialize(instance))) {
        MessageBoxW(nullptr, L"Failed to initialize the dynamic island window.", L"Error", MB_ICONERROR);
        return 1;
    }

    return app.Run();
}
