/*
  This file is part of KDUtils.

  SPDX-FileCopyrightText: 2018-2023 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>
  Author: Paul Lemire <paul.lemire@kdab.com>

  SPDX-License-Identifier: MIT

  Contact KDAB at <info@kdab.com> for commercial licensing options.
*/

#include "win32_gui_platform_integration.h"

#include <KDFoundation/platform/win32/win32_platform_integration.h>
#include "win32_platform_window.h"
#include "win32_utils.h"

#include <KDFoundation/logging.h>

using namespace KDFoundation;
using namespace KDGui;

Win32GuiPlatformIntegration::Win32GuiPlatformIntegration()
{
}

Win32GuiPlatformIntegration::~Win32GuiPlatformIntegration()
{
    unregisterWindowClasses();
}

Win32PlatformEventLoop *Win32GuiPlatformIntegration::createPlatformEventLoopImpl()
{
    // We ensure that the logger has been created here rather than in the ctor so that
    // the central logging configuration in CoreApplication has had a chance to execute.
    m_logger = spdlog::get("win32");
    if (!m_logger) {
        m_logger = spdlog::stdout_color_mt("win32");
        m_logger->set_level(spdlog::level::info);
    }

    return new Win32PlatformEventLoop();
}

Win32PlatformWindow *Win32GuiPlatformIntegration::createPlatformWindowImpl(Window *window)
{
    return new Win32PlatformWindow(this, window);
}

bool Win32GuiPlatformIntegration::registerWindowClass(const std::wstring &className, unsigned style, WNDPROC windowProc)
{
    const auto alreadyRegistered = m_windowClasses.find(className) != m_windowClasses.end();
    if (alreadyRegistered)
        return true;

    const auto appInstance = GetModuleHandle(nullptr);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = style;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = appInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = className.c_str();
    wc.hIcon = static_cast<HICON>(LoadImage(nullptr, IDI_APPLICATION, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));

    const auto atom = RegisterClassEx(&wc);
    if (!atom) {
        spdlog::critical("Failed to register window class: {}", windowsErrorMessage(GetLastError()));
    }

    m_windowClasses.insert(className);

    return atom != 0;
}

void Win32GuiPlatformIntegration::unregisterWindowClasses()
{
    const auto appInstance = GetModuleHandle(nullptr);

    for (const auto &name : m_windowClasses) {
        if (!UnregisterClass(name.c_str(), appInstance)) {
            spdlog::critical("Failed to unregister class: {}", windowsErrorMessage(GetLastError()));
        }
    }

    m_windowClasses.clear();
}
