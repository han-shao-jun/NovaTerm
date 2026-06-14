# WindTermQt Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a cross-platform terminal emulator & SSH client (WindTerm-like) with FluentUI interface

**Architecture:** Monolithic layered C++ application. ElaWindow as main frame with NavigationBar (session tree) + ElaTabWidget (terminal tabs) + ElaAppBar + ElaStatusBar. QTermWidget embedded for terminal emulation with built-in KPty (ConPTY/pty) for local shells. ITransport abstract interface reserved exclusively for remote protocols: SshTransport/SerialTransport/TelnetTransport. SQLite for configuration persistence.

**Tech Stack:** Qt 6.6+, ElaWidgetTools 2.0.3, QTermWidget 2.1+, libssh 0.10+, SQLite

**Key Ela patterns (from example study):**
- `eApp->init()` before any window; `eTheme` for theme switching
- ElaWindow: `initWindow()` → `initEdgeLayout()` → `initContent()`
- Pages: inherit ElaScrollPage, use `createCustomWidget` + `addCentralWidget`
- Navigation: `addPageNode`/`addExpanderNode`/`addCategoryNode`/`addFooterNode` with ElaIconType
- AppBar: `setCustomWidget(ElaAppBarType::MiddleArea, ...)` / `setCentralCustomWidget`
- Close dialog: `setIsDefaultClosed(false)` + ElaContentDialog
- Status: `ElaMessageBar::success(...)` static; `ElaThemeColor(mode, color)` macro

---

## P0: Project Skeleton (~1.5 weeks)

### Task P0-1: Create project directory and top-level CMake

**Files:**
- Create: `D:\code\WindTermQt\CMakeLists.txt`
- Create: `D:\code\WindTermQt\.gitignore`
- Create: `D:\code\WindTermQt\LICENSE`
- Create: `D:\code\WindTermQt\README.md`

- [ ] **Step 1: Create .gitignore**

```
build/
.vs/
*.user
*.pro.user
CMakeUserPresets.json
CMakeLists.txt.user
```

- [ ] **Step 2: Create LICENSE**

```
GNU GENERAL PUBLIC LICENSE Version 2, June 1991
```

- [ ] **Step 3: Create top-level CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(WindTermQt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Find Qt
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Sql)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Sql)

message(STATUS "Using Qt ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}")

# ElaWidgetTools - built as subdirectory
add_subdirectory(${CMAKE_SOURCE_DIR}/../ElaWidgetTools/ElaWidgetTools
                 ${CMAKE_BINARY_DIR}/ElaWidgetTools)
# QTermWidget - git submodule
add_subdirectory(third_party/qtermwidget)
# libssh - system or find_package
find_package(libssh 0.10 QUIET)

add_subdirectory(src)
add_subdirectory(tests)
```

- [ ] **Step 4: Create README.md**

```markdown
# WindTermQt

A cross-platform terminal emulator & SSH client with FluentUI design.

## Building

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build
```

## License

GPLv2+
```

- [ ] **Step 5: Initialize git**

```bash
cd D:\code\WindTermQt
git init
git add .
git commit -m "chore: initial project skeleton with CMake"
```

---

### Task P0-2: Create source directory structure and main entry

**Files:**
- Create: `D:\code\WindTermQt\src\CMakeLists.txt`
- Create: `D:\code\WindTermQt\src\main.cpp`
- Create: `D:\code\WindTermQt\src\app\Application.h`
- Create: `D:\code\WindTermQt\src\app\Application.cpp`
- Create: `D:\code\WindTermQt\src\app\MainWindow.h`
- Create: `D:\code\WindTermQt\src\app\MainWindow.cpp`
- Create: `D:\code\WindTermQt\resources\resources.qrc`

- [ ] **Step 1: Create src/CMakeLists.txt**

```cmake
file(GLOB_RECURSE SOURCES *.cpp *.h)

add_executable(WindTermQt ${SOURCES} ${CMAKE_SOURCE_DIR}/resources/resources.qrc)

target_include_directories(WindTermQt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(WindTermQt PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    ElaWidgetTools
    qtermwidget5
    ${LIBSSH_LIBRARIES}
)

if(WIN32)
    set_target_properties(WindTermQt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
```

- [ ] **Step 2: Create main.cpp**

```cpp
#include <QApplication>
#include "app/Application.h"

int main(int argc, char* argv[])
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif
    QApplication a(argc, argv);
    a.setApplicationName("WindTermQt");
    a.setApplicationVersion("0.1.0");
    a.setOrganizationName("WindTermQt");

    Application::instance().init();

    Application::instance().mainWindow().show();
    return a.exec();
}
```

- [ ] **Step 3: Create Application.h**

```cpp
#pragma once
#include <QObject>
#include <memory>

class MainWindow;

class Application : public QObject
{
    Q_OBJECT
public:
    static Application& instance();
    void init();

    MainWindow& mainWindow() { return *_mainWindow; }

private:
    Application() = default;
    std::unique_ptr<MainWindow> _mainWindow;
};
```

- [ ] **Step 4: Create Application.cpp**

```cpp
#include "Application.h"
#include "MainWindow.h"
#include "ElaApplication.h"

Application& Application::instance()
{
    static Application app;
    return app;
}

void Application::init()
{
    eApp->init();
    _mainWindow = std::make_unique<MainWindow>();
}
```

- [ ] **Step 5: Create MainWindow.h (ElaWindow subclass shell)**

```cpp
#pragma once
#include "ElaWindow.h"

class MainWindow : public ElaWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void initWindow();
    void initEdgeLayout();
    void initContent();
};
```

- [ ] **Step 6: Create MainWindow.cpp (minimal working window)**

```cpp
#include "MainWindow.h"
#include "ElaMenu.h"
#include "ElaText.h"
#include "ElaStatusBar.h"
#include "ElaTheme.h"
#include "ElaContentDialog.h"
#include "ElaMessageBar.h"

MainWindow::MainWindow(QWidget* parent) : ElaWindow(parent)
{
    initWindow();
    initEdgeLayout();
    initContent();

    // Close confirmation
    auto* closeDialog = new ElaContentDialog(this);
    connect(closeDialog, &ElaContentDialog::rightButtonClicked,
            this, &MainWindow::close);
    connect(closeDialog, &ElaContentDialog::middleButtonClicked, this, [=]() {
        closeDialog->close();
        showMinimized();
    });
    setIsDefaultClosed(false);
    connect(this, &MainWindow::closeButtonClicked, this, [=]() {
        closeDialog->exec();
    });
}

MainWindow::~MainWindow() = default;

void MainWindow::initWindow()
{
    setWindowTitle("WindTermQt");
    resize(1280, 800);
    setWindowIcon(QIcon(":/icons/app.png"));

    setUserInfoCardTitle("WindTermQt");
    setUserInfoCardSubTitle("Terminal & SSH Client");

    // Central stack placeholder
    auto* centralStack = new ElaText("WindTermQt", this);
    centralStack->setTextPixelSize(32);
    centralStack->setAlignment(Qt::AlignCenter);
    addCentralWidget(centralStack);

    // AppBar menu (theme toggle + about)
    auto* appBarMenu = new ElaMenu(this);
    appBarMenu->setMenuItemHeight(27);
    connect(appBarMenu->addElaIconAction(ElaIconType::MoonStars, "Toggle Theme"),
            &QAction::triggered, this, [=]() {
        eTheme->setThemeMode(eTheme->getThemeMode() == ElaThemeType::Light
                             ? ElaThemeType::Dark : ElaThemeType::Light);
    });
    setCustomMenu(appBarMenu);
}

void MainWindow::initEdgeLayout()
{
    // StatusBar
    auto* statusBar = new ElaStatusBar(this);
    auto* statusText = new ElaText("Ready", this);
    statusText->setTextPixelSize(13);
    statusBar->addWidget(statusText);
    setStatusBar(statusBar);
}

void MainWindow::initContent()
{
    // Placeholder pages will be added in later tasks
    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "WindTermQt", "Application initialized", 2000);
}
```

- [ ] **Step 7: Create resources/resources.qrc**

```xml
<RCC>
    <qresource prefix="/">
        <file>icons/app.png</file>
    </qresource>
</RCC>
```

- [ ] **Step 8: Create placeholder icon directory and commit**

```bash
mkdir -p D:\code\WindTermQt\resources\icons
# Place a placeholder PNG in resources/icons/app.png
git add .
git commit -m "feat: create MainWindow shell with ElaWindow"
```

---

### Task P0-3: Create placeholder pages for all navigation sections

**Files:**
- Create: `D:\code\WindTermQt\src\pages\TerminalPage.h`
- Create: `D:\code\WindTermQt\src\pages\TerminalPage.cpp`
- Create: `D:\code\WindTermQt\src\pages\ConnectionsPage.h`
- Create: `D:\code\WindTermQt\src\pages\ConnectionsPage.cpp`
- Create: `D:\code\WindTermQt\src\pages\FileTransferPage.h`
- Create: `D:\code\WindTermQt\src\pages\FileTransferPage.cpp`
- Create: `D:\code\WindTermQt\src\pages\HistoryPage.h`
- Create: `D:\code\WindTermQt\src\pages\HistoryPage.cpp`
- Create: `D:\code\WindTermQt\src\pages\KeyManagerPage.h`
- Create: `D:\code\WindTermQt\src\pages\KeyManagerPage.cpp`
- Create: `D:\code\WindTermQt\src\pages\SettingsPage.h`
- Create: `D:\code\WindTermQt\src\pages\SettingsPage.cpp`

- [ ] **Step 1: Create TerminalPage (main page, hosts the terminal + session tree)**

```cpp
// TerminalPage.h
#pragma once
#include "ElaScrollPage.h"

class TerminalPage : public ElaScrollPage
{
    Q_OBJECT
public:
    explicit TerminalPage(QWidget* parent = nullptr);
};
```

```cpp
// TerminalPage.cpp
#include "TerminalPage.h"
#include "ElaText.h"
#include <QVBoxLayout>

TerminalPage::TerminalPage(QWidget* parent) : ElaScrollPage(parent)
{
    setWindowTitle("Terminal");

    auto* centralWidget = new QWidget(this);
    centralWidget->setWindowTitle("Terminal");
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* placeholder = new ElaText("Terminal Area\n\nConnect to a session to begin", this);
    placeholder->setTextPixelSize(18);
    placeholder->setAlignment(Qt::AlignCenter);
    layout->addWidget(placeholder);

    addCentralWidget(centralWidget);
}
```

- [ ] **Step 2: Create ConnectionsPage (session configuration manager)**

```cpp
// ConnectionsPage.h
#pragma once
#include "ElaScrollPage.h"

class ConnectionsPage : public ElaScrollPage
{
    Q_OBJECT
public:
    explicit ConnectionsPage(QWidget* parent = nullptr);
};
```

```cpp
// ConnectionsPage.cpp
#include "ConnectionsPage.h"
#include "ElaText.h"
#include <QVBoxLayout>

ConnectionsPage::ConnectionsPage(QWidget* parent) : ElaScrollPage(parent)
{
    setWindowTitle("Connection Manager");

    auto* centralWidget = new QWidget(this);
    centralWidget->setWindowTitle("Connections");
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(20, 20, 20, 20);

    auto* title = new ElaText("Connection Manager", this);
    title->setTextPixelSize(24);
    layout->addWidget(title);

    auto* desc = new ElaText("Manage your SSH, Telnet, Serial, and local shell connections.",
                             this);
    desc->setTextPixelSize(14);
    layout->addWidget(desc);
    layout->addStretch();

    addCentralWidget(centralWidget);
}
```

- [ ] **Step 3: Create the remaining 4 placeholder pages**

(FileTransferPage, HistoryPage, KeyManagerPage, SettingsPage — follow the same pattern as ConnectionsPage, each with a different windowTitle and description text.)

- [ ] **Step 4: Register all pages in MainWindow::initContent()**

Update `MainWindow.cpp` — add to initContent():

```cpp
#include "pages/TerminalPage.h"
#include "pages/ConnectionsPage.h"
#include "pages/FileTransferPage.h"
#include "pages/HistoryPage.h"
#include "pages/KeyManagerPage.h"
#include "pages/SettingsPage.h"

// ... in initContent(), after creating pages:
void MainWindow::initContent()
{
    _terminalPage = new TerminalPage(this);
    _connectionsPage = new ConnectionsPage(this);
    _fileTransferPage = new FileTransferPage(this);
    _historyPage = new HistoryPage(this);
    _keyManagerPage = new KeyManagerPage(this);
    _settingsPage = new SettingsPage(this);

    addPageNode("Terminal", _terminalPage, ElaIconType::Terminal);
    addPageNode("Connections", _connectionsPage, ElaIconType::GlobePointer);
    addPageNode("File Transfer", _fileTransferPage, ElaIconType::FolderOpen);
    QString toolsKey;
    addExpanderNode("Tools", toolsKey, ElaIconType::Toolbox);
    addPageNode("History", _historyPage, toolsKey, ElaIconType::ClockRotateLeft);
    addPageNode("Key Manager", _keyManagerPage, toolsKey, ElaIconType::Key);
    addFooterNode("Settings", _settingsPage, _settingsKey, 0, ElaIconType::GearComplex);

    connect(this, &ElaWindow::userInfoCardClicked, this, [=]() {
        navigation(_terminalPage->property("ElaPageKey").toString());
    });

    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "WindTermQt", "Application initialized", 2000);
}
```

Also add member variables to `MainWindow.h`:
```cpp
class TerminalPage;
class ConnectionsPage;
class FileTransferPage;
class HistoryPage;
class KeyManagerPage;
class SettingsPage;

// in private section:
TerminalPage* _terminalPage{nullptr};
ConnectionsPage* _connectionsPage{nullptr};
FileTransferPage* _fileTransferPage{nullptr};
HistoryPage* _historyPage{nullptr};
KeyManagerPage* _keyManagerPage{nullptr};
SettingsPage* _settingsPage{nullptr};
QString _settingsKey;
```

- [ ] **Step 5: Commit**

```bash
git add src/pages/ src/app/MainWindow.h src/app/MainWindow.cpp
git commit -m "feat: add all navigation pages with placeholder content"
```

---

## P1: Local Terminal (~1.5 weeks)

### Task P1-1: Verify QTermWidget availability and create CMake integration

**Files:**
- Modify: `D:\code\WindTermQt\CMakeLists.txt`
- Create: `D:\code\WindTermQt\cmake\FindQTermWidget.cmake`

- [ ] **Step 1: Create FindQTermWidget.cmake**

```cmake
# cmake/FindQTermWidget.cmake
find_path(QTERMWIDGET_INCLUDE_DIR qtermwidget5/qtermwidget.h
    HINTS /usr/include /usr/local/include
    PATH_SUFFIXES qtermwidget5)

find_library(QTERMWIDGET_LIBRARY NAMES qtermwidget5 qtermwidget6
    HINTS /usr/lib /usr/local/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(QTermWidget
    REQUIRED_VARS QTERMWIDGET_LIBRARY QTERMWIDGET_INCLUDE_DIR)

if(QTermWidget_FOUND AND NOT TARGET QTermWidget::QTermWidget)
    add_library(QTermWidget::QTermWidget UNKNOWN IMPORTED)
    set_target_properties(QTermWidget::QTermWidget PROPERTIES
        IMPORTED_LOCATION "${QTERMWIDGET_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${QTERMWIDGET_INCLUDE_DIR}")
endif()
```

- [ ] **Step 2: Update top-level CMakeLists.txt** — switch QTermWidget to find_package approach

Replace the `add_subdirectory(third_party/qtermwidget)` line with:
```cmake
# QTermWidget - try system package first, fall back to submodule
find_package(QTermWidget QUIET)
if(NOT QTermWidget_FOUND)
    message(STATUS "QTermWidget not found in system, building from submodule")
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/qtermwidget/CMakeLists.txt")
        message(FATAL_ERROR "QTermWidget submodule not initialized. Run: git submodule update --init")
    endif()
    add_subdirectory(third_party/qtermwidget)
endif()
```

- [ ] **Step 3: Initialize git submodule for QTermWidget**

```bash
cd D:\code\WindTermQt
mkdir -p third_party
git submodule add https://github.com/lxqt/qtermwidget.git third_party/qtermwidget
git commit -m "feat: add QTermWidget as git submodule"
```

---

### Task P1-2: Create ITransport interface

**Files:**
- Create: `D:\code\WindTermQt\src\transport\ITransport.h`

- [ ] **Step 1: Create ITransport.h**

```cpp
#pragma once
#include <QObject>
#include <QByteArray>

class ITransport : public QObject
{
    Q_OBJECT
public:
    explicit ITransport(QObject* parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    virtual bool connectToHost() = 0;
    virtual void disconnect() = 0;
    virtual void write(const QByteArray& data) = 0;
    virtual void resizeTerminal(int cols, int rows) = 0;
    virtual bool isConnected() const = 0;
    virtual QString errorString() const = 0;

signals:
    void connected();
    void disconnected();
    void readyRead(const QByteArray& data);
    void errorOccurred(const QString& error);
};
```

- [ ] **Step 2: Commit**

```bash
git add src/transport/
git commit -m "feat: add ITransport abstract interface"
```

---

### Task P1-3: Remove standalone PtyTransport — use QTermWidget built-in KPty

**Rationale:** QTermWidget ships with KPty that directly wraps ConPTY (Windows 10+) / pty (Unix).
QProcess pipe-based PtyTransport cannot provide a real TTY — interactive programs (vim, htop, tmux)
would fail. The ITransport interface is now reserved exclusively for remote protocols (SSH/Serial/Telnet).

**Files:**
- Modify: `D:\code\WindTermQt\src\transport\ITransport.h` — add doc comment that this is remote-only
- Delete/Archive: `D:\code\WindTermQt\src\transport\PtyTransport.h`
- Delete/Archive: `D:\code\WindTermQt\src\transport\PtyTransport.cpp`

- [ ] **Step 1: Update ITransport.h with scope clarification**

```cpp
#pragma once
#include <QObject>
#include <QByteArray>

// ITransport is ONLY for remote protocols (SSH/Serial/Telnet).
// Local terminal is handled directly by QTermWidget's built-in KPty.
class ITransport : public QObject
{
    Q_OBJECT
public:
    explicit ITransport(QObject* parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    virtual bool connectToHost() = 0;
    virtual void disconnect() = 0;
    virtual void write(const QByteArray& data) = 0;
    virtual void resizeTerminal(int cols, int rows) = 0;
    virtual bool isConnected() const = 0;
    virtual QString errorString() const = 0;

signals:
    void connected();
    void disconnected();
    void readyRead(const QByteArray& data);
    void errorOccurred(const QString& error);
};
```

- [ ] **Step 2: Remove PtyTransport files**

```bash
cd D:\code\WindTermQt
rm src/transport/PtyTransport.h
rm src/transport/PtyTransport.cpp
```

- [ ] **Step 3: Commit**

```bash
git rm src/transport/PtyTransport.h src/transport/PtyTransport.cpp
git add src/transport/ITransport.h
git commit -m "refactor: remove PtyTransport — local shell uses QTermWidget built-in KPty"
```

---
### Task P1-4: Create TerminalView with dual mode (local KPty + remote ITransport)

**Files:**
- Create: `D:\code\WindTermQt\src\terminal\TerminalView.h`
- Create: `D:\code\WindTermQt\src\terminal\TerminalView.cpp`

- [ ] **Step 1: Create TerminalView.h**

```cpp
#pragma once
#include <QWidget>

class ITransport;
class QTermWidget;

class TerminalView : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalView(QWidget* parent = nullptr);
    ~TerminalView() override;

    // ── 本地终端: 使用 QTermWidget 内置 KPty (ConPTY/pty) ──
    void startLocalShell(const QString& shellProgram = {});
    void stopLocalShell();

    // ── 远程终端: 通过 ITransport 桥接 ──
    void attachTransport(ITransport* transport);
    void detachTransport();
    ITransport* transport() const { return _transport; }
    QTermWidget* terminalWidget() const { return _terminal; }

signals:
    void titleChanged(const QString& title);
    void activityDetected();
    void shellFinished(int exitCode);

private slots:
    void onTransportReadyRead(const QByteArray& data);
    void onTransportDisconnected();
    void onLocalShellFinished();

private:
    void setupContextMenu(const QPoint& pos);

    QTermWidget* _terminal{nullptr};
    ITransport* _transport{nullptr};
    bool _isLocalShell{false};
};
```

- [ ] **Step 2: Create TerminalView.cpp (dual mode)**

```cpp
#include "TerminalView.h"
#include "transport/ITransport.h"

#include "qtermwidget.h"
#include <QVBoxLayout>
#include "ElaMenu.h"

TerminalView::TerminalView(QWidget* parent)
    : QWidget(parent)
{
    // startnow=0: teletype mode — we control data flow manually
    _terminal = new QTermWidget(0, this);
    _terminal->startTerminalTeletype();

    _terminal->setColorScheme("system");
    _terminal->setScrollBarPosition(QTermWidgetInterface::ScrollBarRight);
    _terminal->setTerminalFont(QFont("Cascadia Code, Consolas, monospace", 12));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_terminal);

    setFocusProxy(_terminal);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested,
            this, &TerminalView::setupContextMenu);
}

TerminalView::~TerminalView()
{
    stopLocalShell();
    detachTransport();
}

// ════════════════════ 本地终端模式 ════════════════════

void TerminalView::startLocalShell(const QString& shellProgram)
{
    stopLocalShell();
    detachTransport();

    // QTermWidget KPty 直接启动 shell 进程 — 内置 ConPTY(Windows)/pty(Unix)
    // AvailablePrograms 可选参数: 留空则使用 $SHELL / %COMSPEC%
    int pid = _terminal->startShellProgram();
    if (pid == -1) {
        // startShellProgram() returns -1 on failure
        qWarning() << "TerminalView: QTermWidget::startShellProgram() failed";
        return;
    }

    _isLocalShell = true;

    // Connect shell finished signal
    connect(_terminal, &QTermWidget::finished,
            this, &TerminalView::onLocalShellFinished);

    // Forward title changes
    connect(_terminal, &QTermWidget::titleChanged,
            this, [this]() {
        emit titleChanged(_terminal->title());
    });
}

void TerminalView::stopLocalShell()
{
    if (_isLocalShell) {
        disconnect(_terminal, &QTermWidget::finished,
                   this, &TerminalView::onLocalShellFinished);
        _isLocalShell = false;
    }
}

void TerminalView::onLocalShellFinished()
{
    _isLocalShell = false;
    if (_terminal) {
        const char* msg = "\r\n[Process completed]\r\n";
        _terminal->receiveData(msg, static_cast<int>(strlen(msg)));
    }
    emit shellFinished(_terminal ? _terminal->exitCode() : -1);
}

// ════════════════════ 远程终端模式 (ITransport 桥接) ════════════════════

void TerminalView::attachTransport(ITransport* transport)
{
    detachTransport();
    stopLocalShell();
    _transport = transport;

    connect(_transport, &ITransport::readyRead,
            this, &TerminalView::onTransportReadyRead);
    connect(_transport, &ITransport::disconnected,
            this, &TerminalView::onTransportDisconnected);

    // Forward keyboard input from terminal -> transport
    connect(_terminal, &QTermWidget::sendData,
            this, [this](const char* data, int len) {
        if (_transport && _transport->isConnected())
            _transport->write(QByteArray(data, len));
    });

    // Forward title changes
    connect(_terminal, &QTermWidget::titleChanged,
            this, [this]() {
        emit titleChanged(_terminal->title());
    });

    // Forward activity
    connect(_terminal, &QTermWidget::activity,
            this, &TerminalView::activityDetected);
}

void TerminalView::detachTransport()
{
    if (_transport) {
        disconnect(_transport, nullptr, this, nullptr);
        _transport = nullptr;
    }
}

void TerminalView::onTransportReadyRead(const QByteArray& data)
{
    if (_terminal)
        _terminal->receiveData(data.constData(), data.size());
}

void TerminalView::onTransportDisconnected()
{
    if (_terminal) {
        const char* msg = "\r\n[Disconnected]\r\n";
        _terminal->receiveData(msg, static_cast<int>(strlen(msg)));
    }
}

// ════════════════════ 右键菜单 ════════════════════

void TerminalView::setupContextMenu(const QPoint& pos)
{
    auto* menu = new ElaMenu(this);
    menu->setMenuItemHeight(27);

    connect(menu->addElaIconAction(ElaIconType::Copy, tr("Copy")),
            &QAction::triggered, _terminal, &QTermWidget::copyClipboard);
    connect(menu->addElaIconAction(ElaIconType::Paste, tr("Paste")),
            &QAction::triggered, _terminal, &QTermWidget::pasteClipboard);
    menu->addSeparator();
    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlassPlus, tr("Zoom In")),
            &QAction::triggered, _terminal, &QTermWidget::zoomIn);
    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlassMinus, tr("Zoom Out")),
            &QAction::triggered, _terminal, &QTermWidget::zoomOut);
    menu->addSeparator();
    connect(menu->addElaIconAction(ElaIconType::Broom, tr("Clear Scrollback")),
            &QAction::triggered, _terminal, &QTermWidget::clear);

    menu->popup(mapToGlobal(pos));
}
```

- [ ] **Step 3: Commit**

```bash
git add src/terminal/
git commit -m "feat: create TerminalView with dual mode (local KPty + remote ITransport)"
```

---
### Task P1-5: Integrate TerminalView into TerminalPage (local shell only, no PtyTransport)

**Files:**
- Modify: `D:\code\WindTermQt\src\pages\TerminalPage.h`
- Modify: `D:\code\WindTermQt\src\pages\TerminalPage.cpp`

- [ ] **Step 1: Update TerminalPage.h — remove PtyTransport dependency**

```cpp
#pragma once
#include "ElaScrollPage.h"

class TerminalView;

class TerminalPage : public ElaScrollPage
{
    Q_OBJECT
public:
    explicit TerminalPage(QWidget* parent = nullptr);
    ~TerminalPage() override;

    void openLocalTerminal();
    TerminalView* currentTerminal() const { return _terminalView; }

private:
    TerminalView* _terminalView{nullptr};
    QWidget* _centralWidget{nullptr};
};
```

- [ ] **Step 2: Update TerminalPage.cpp — use TerminalView::startLocalShell()**

```cpp
#include "TerminalPage.h"
#include "terminal/TerminalView.h"
#include "ElaMessageBar.h"
#include <QVBoxLayout>

TerminalPage::TerminalPage(QWidget* parent) : ElaScrollPage(parent)
{
    setWindowTitle(tr("Terminal"));

    _terminalView = new TerminalView(this);

    _centralWidget = new QWidget(this);
    _centralWidget->setWindowTitle(tr("Terminal"));
    auto* layout = new QVBoxLayout(_centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_terminalView);
    addCentralWidget(_centralWidget);

    // Auto-start local terminal using QTermWidget KPty
    openLocalTerminal();
}

TerminalPage::~TerminalPage() = default;

void TerminalPage::openLocalTerminal()
{
    // QTermWidget 内置 KPty 直接启动 shell — 零额外依赖
    // Windows: ConPTY → cmd.exe / pwsh.exe
    // Unix:    pty    → bash / zsh
    _terminalView->startLocalShell();

    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           tr("Terminal"), tr("Local shell started"), 1500);
}
```

- [ ] **Step 3: Verify local terminal works**

```bash
# Build and verify:
# 1. Local shell starts on TerminalPage automatically
# 2. vim, htop, tmux render correctly (real PTY, not pipe)
# 3. Ctrl+C, Ctrl+D work
# 4. Terminal resize triggers KPty resize signal
```

- [ ] **Step 4: Commit**

```bash
git add src/pages/TerminalPage.h src/pages/TerminalPage.cpp
git commit -m "feat: integrate TerminalView into TerminalPage via native KPty"
```

---
### Task P2-1: Create SshTransport

**Files:**
- Create: `D:\code\WindTermQt\src\transport\SshTransport.h`
- Create: `D:\code\WindTermQt\src\transport\SshTransport.cpp`

- [ ] **Step 1: Create SshTransport.h**

```cpp
#pragma once
#include "ITransport.h"
#include <QThread>
#include <QByteArray>

// Forward declare libssh types (included only in .cpp)
struct ssh_session_struct;
typedef struct ssh_session_struct* ssh_session;
struct ssh_channel_struct;
typedef struct ssh_channel_struct* ssh_channel;

class SshTransport : public ITransport
{
    Q_OBJECT
public:
    enum AuthMethod { Password, PublicKey, Agent };

    explicit SshTransport(QObject* parent = nullptr);
    ~SshTransport() override;

    void setHost(const QString& host);
    void setPort(int port);
    void setUsername(const QString& username);
    void setPassword(const QString& password);
    void setKeyFile(const QString& keyPath, const QString& passphrase = {});
    void setAuthMethod(AuthMethod method);

    bool connectToHost() override;
    void disconnect() override;
    void write(const QByteArray& data) override;
    void resizeTerminal(int cols, int rows) override;
    bool isConnected() const override;
    QString errorString() const override;

    // SFTP access (returns a secondary channel)
    void* createSftpSession();   // returns sftp_session (opaque)
    void closeSftpSession(void* sftp);

private:
    bool authenticate();
    void readLoop();

    ssh_session _session{nullptr};
    ssh_channel _channel{nullptr};
    QString _host;
    int _port{22};
    QString _username;
    QString _password;
    QString _keyPath;
    QString _keyPassphrase;
    AuthMethod _authMethod{Password};
    QString _errorString;
    QThread* _readThread{nullptr};
    bool _running{false};
};
```

- [ ] **Step 2: Create SshTransport.cpp**

```cpp
#include "SshTransport.h"
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <QThread>

SshTransport::SshTransport(QObject* parent) : ITransport(parent) {}

SshTransport::~SshTransport()
{
    disconnect();
}

void SshTransport::setHost(const QString& host) { _host = host; }
void SshTransport::setPort(int port) { _port = port; }
void SshTransport::setUsername(const QString& username) { _username = username; }
void SshTransport::setPassword(const QString& password) { _password = password; }
void SshTransport::setKeyFile(const QString& keyPath, const QString& passphrase)
{
    _keyPath = keyPath;
    _keyPassphrase = passphrase;
}
void SshTransport::setAuthMethod(AuthMethod method) { _authMethod = method; }

bool SshTransport::connectToHost()
{
    _session = ssh_new();
    if (!_session) {
        _errorString = "Failed to create SSH session";
        return false;
    }

    ssh_options_set(_session, SSH_OPTIONS_HOST, _host.toUtf8().constData());
    ssh_options_set(_session, SSH_OPTIONS_PORT, &_port);
    ssh_options_set(_session, SSH_OPTIONS_USER, _username.toUtf8().constData());

    int rc = ssh_connect(_session);
    if (rc != SSH_OK) {
        _errorString = ssh_get_error(_session);
        ssh_free(_session);
        _session = nullptr;
        return false;
    }

    // Verify host key (auto-accept first time, store in known_hosts)
    ssh_key key;
    if (ssh_get_server_publickey(_session, &key) == SSH_OK) {
        int state = ssh_session_is_known_server(_session);
        switch (state) {
        case SSH_KNOWN_HOSTS_OK: break;
        case SSH_KNOWN_HOSTS_UNKNOWN:
            ssh_session_update_known_hosts(_session);
            break;
        case SSH_KNOWN_HOSTS_CHANGED:
        case SSH_KNOWN_HOSTS_OTHER:
            _errorString = "Host key verification failed!";
            ssh_disconnect(_session);
            ssh_free(_session);
            _session = nullptr;
            return false;
        default:
            ssh_session_update_known_hosts(_session);
            break;
        }
    }

    if (!authenticate()) {
        ssh_disconnect(_session);
        ssh_free(_session);
        _session = nullptr;
        return false;
    }

    // Open shell channel
    _channel = ssh_channel_new(_session);
    if (!_channel) {
        _errorString = "Failed to create SSH channel";
        ssh_disconnect(_session);
        ssh_free(_session);
        _session = nullptr;
        return false;
    }

    rc = ssh_channel_open_session(_channel);
    if (rc != SSH_OK) {
        _errorString = ssh_get_error(_session);
        ssh_channel_free(_channel);
        _channel = nullptr;
        ssh_disconnect(_session);
        ssh_free(_session);
        _session = nullptr;
        return false;
    }

    // Request PTY
    ssh_channel_request_pty_size(_channel, "xterm-256color", 80, 24);

    // Start shell
    rc = ssh_channel_request_shell(_channel);
    if (rc != SSH_OK) {
        _errorString = ssh_get_error(_session);
        ssh_channel_close(_channel);
        ssh_channel_free(_channel);
        _channel = nullptr;
        ssh_disconnect(_session);
        ssh_free(_session);
        _session = nullptr;
        return false;
    }

    // Start read thread
    _running = true;
    _readThread = QThread::create([this]() { readLoop(); });
    _readThread->start();

    emit connected();
    return true;
}

bool SshTransport::authenticate()
{
    int rc;
    switch (_authMethod) {
    case Password:
        rc = ssh_userauth_password(_session, nullptr, _password.toUtf8().constData());
        break;
    case PublicKey: {
        ssh_key key;
        rc = ssh_pki_import_privkey_file(_keyPath.toUtf8().constData(),
                                         _keyPassphrase.isEmpty() ? nullptr
                                             : _keyPassphrase.toUtf8().constData(),
                                         nullptr, nullptr, &key);
        if (rc != SSH_OK) {
            _errorString = "Failed to load private key: " + _keyPath;
            return false;
        }
        rc = ssh_userauth_publickey(_session, nullptr, key);
        ssh_key_free(key);
        break;
    }
    case Agent:
        rc = ssh_userauth_agent(_session, nullptr);
        break;
    default:
        _errorString = "Unknown auth method";
        return false;
    }

    if (rc != SSH_AUTH_SUCCESS) {
        _errorString = ssh_get_error(_session);
        return false;
    }
    return true;
}

void SshTransport::readLoop()
{
    char buffer[4096];
    while (_running && _channel) {
        int n = ssh_channel_read_timeout(_channel, buffer, sizeof(buffer), 0, 100);
        if (n > 0) {
            emit readyRead(QByteArray(buffer, n));
        } else if (n == SSH_ERROR || ssh_channel_is_eof(_channel)) {
            break;
        }
    }
    if (_running)
        emit disconnected();
}

void SshTransport::disconnect()
{
    _running = false;
    if (_readThread) {
        _readThread->quit();
        _readThread->wait(3000);
        delete _readThread;
        _readThread = nullptr;
    }
    if (_channel) {
        ssh_channel_close(_channel);
        ssh_channel_free(_channel);
        _channel = nullptr;
    }
    if (_session) {
        ssh_disconnect(_session);
        ssh_free(_session);
        _session = nullptr;
    }
}

void SshTransport::write(const QByteArray& data)
{
    if (_channel && _running)
        ssh_channel_write(_channel, data.constData(), data.size());
}

void SshTransport::resizeTerminal(int cols, int rows)
{
    if (_channel)
        ssh_channel_change_pty_size(_channel, cols, rows);
}

bool SshTransport::isConnected() const
{
    return _session && ssh_is_connected(_session) && _channel && _running;
}

QString SshTransport::errorString() const
{
    return _errorString;
}

void* SshTransport::createSftpSession()
{
    if (!_session) return nullptr;
    sftp_session sftp = sftp_new(_session);
    if (!sftp) return nullptr;
    if (sftp_init(sftp) != SSH_OK) {
        sftp_free(sftp);
        return nullptr;
    }
    return sftp;
}

void SshTransport::closeSftpSession(void* sftp)
{
    if (sftp) {
        sftp_free(static_cast<sftp_session>(sftp));
    }
}
```

- [ ] **Step 3: Commit**

```bash
git add src/transport/SshTransport.h src/transport/SshTransport.cpp
git commit -m "feat: implement SshTransport with libssh (password/key/agent auth)"
```

---

### Task P2-2: Create connection dialog (SessionDialog)

**Files:**
- Create: `D:\code\WindTermQt\src\sessions\Session.h`
- Create: `D:\code\WindTermQt\src\sessions\SessionDialog.h`
- Create: `D:\code\WindTermQt\src\sessions\SessionDialog.cpp`

- [ ] **Step 1: Create Session.h (data object)**

```cpp
#pragma once
#include <QString>
#include <QDateTime>

struct Session {
    int id = -1;
    QString name;
    QString groupName;
    QString protocol = "ssh";  // ssh, local, serial, telnet
    QString host;
    int port = 22;
    QString username;
    QString authType = "password";  // password, key, agent
    QString keyPath;
    QString terminal = "xterm-256color";
    QString colorScheme = "system";
    QString fontFamily;
    int fontSize = 12;
    QString tags;
    int sortOrder = 0;
    QDateTime createdAt;
    QDateTime updatedAt;

    bool isValid() const {
        if (protocol == "local") return true;
        if (name.isEmpty()) return false;
        if (host.isEmpty() && protocol != "serial") return false;
        return true;
    }
};
```

- [ ] **Step 2: Create SessionDialog.h**

```cpp
#pragma once
#include "ElaDialog.h"
#include "Session.h"

class ElaLineEdit;
class ElaComboBox;
class ElaPushButton;
class ElaToggleSwitch;

class SessionDialog : public ElaDialog
{
    Q_OBJECT
public:
    explicit SessionDialog(QWidget* parent = nullptr);

    void setSession(const Session& session);
    Session session() const;

signals:
    void sessionSaved(const Session& session);

private:
    void setupUi();
    void onProtocolChanged(int index);
    void onSave();

    ElaLineEdit* _nameEdit{nullptr};
    ElaComboBox* _protocolCombo{nullptr};
    ElaLineEdit* _hostEdit{nullptr};
    ElaLineEdit* _portEdit{nullptr};
    ElaLineEdit* _usernameEdit{nullptr};
    ElaComboBox* _authCombo{nullptr};
    ElaLineEdit* _passwordEdit{nullptr};
    ElaLineEdit* _keyPathEdit{nullptr};
    ElaPushButton* _saveBtn{nullptr};
    ElaPushButton* _cancelBtn{nullptr};
    Session _session;
};
```

- [ ] **Step 3: Create SessionDialog.cpp (full form layout)**

```cpp
#include "SessionDialog.h"
#include "ElaLineEdit.h"
#include "ElaComboBox.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>

SessionDialog::SessionDialog(QWidget* parent) : ElaDialog(parent)
{
    setWindowTitle("New Connection");
    setFixedSize(480, 520);
    setIsFixedSize(true);
    setWindowModality(Qt::ApplicationModal);
    setupUi();
}

void SessionDialog::setupUi()
{
    auto* mainWidget = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(mainWidget);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(12);

    // Protocol selector
    auto* protoLayout = new QHBoxLayout();
    auto* protoLabel = new ElaText("Protocol:", this);
    protoLabel->setTextPixelSize(14);
    _protocolCombo = new ElaComboBox(this);
    _protocolCombo->addItems({"SSH", "Local Shell", "Serial", "Telnet"});
    _protocolCombo->setCurrentIndex(0);
    connect(_protocolCombo, QOverload<int>::of(&ElaComboBox::currentIndexChanged),
            this, &SessionDialog::onProtocolChanged);
    protoLayout->addWidget(protoLabel);
    protoLayout->addWidget(_protocolCombo, 1);
    mainLayout->addLayout(protoLayout);

    // Form fields
    auto* form = new QFormLayout();
    form->setSpacing(8);

    _nameEdit = new ElaLineEdit(this);
    _nameEdit->setPlaceholderText("My Server");
    form->addRow("Name:", _nameEdit);

    _hostEdit = new ElaLineEdit(this);
    _hostEdit->setPlaceholderText("192.168.1.100");
    form->addRow("Host:", _hostEdit);

    _portEdit = new ElaLineEdit(this);
    _portEdit->setText("22");
    _portEdit->setPlaceholderText("22");
    form->addRow("Port:", _portEdit);

    _usernameEdit = new ElaLineEdit(this);
    _usernameEdit->setPlaceholderText("root");
    form->addRow("Username:", _usernameEdit);

    _authCombo = new ElaComboBox(this);
    _authCombo->addItems({"Password", "Public Key", "SSH Agent"});
    form->addRow("Auth Method:", _authCombo);

    _passwordEdit = new ElaLineEdit(this);
    _passwordEdit->setEchoMode(ElaLineEdit::Password);
    _passwordEdit->setPlaceholderText("Enter password");
    form->addRow("Password:", _passwordEdit);

    _keyPathEdit = new ElaLineEdit(this);
    _keyPathEdit->setPlaceholderText("~/.ssh/id_ed25519");
    _keyPathEdit->setVisible(false);
    form->addRow("Key Path:", _keyPathEdit);

    // Show/hide fields based on auth method
    connect(_authCombo, QOverload<int>::of(&ElaComboBox::currentIndexChanged),
            this, [=](int index) {
        _passwordEdit->setVisible(index == 0);
        _keyPathEdit->setVisible(index == 1);
    });

    mainLayout->addLayout(form);
    mainLayout->addStretch();

    // Buttons
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    _cancelBtn = new ElaPushButton("Cancel", this);
    _cancelBtn->setFixedWidth(100);
    connect(_cancelBtn, &ElaPushButton::clicked, this, &QDialog::reject);
    _saveBtn = new ElaPushButton("Connect", this);
    _saveBtn->setFixedWidth(100);
    connect(_saveBtn, &ElaPushButton::clicked, this, &SessionDialog::onSave);
    btnLayout->addWidget(_cancelBtn);
    btnLayout->addWidget(_saveBtn);
    mainLayout->addLayout(btnLayout);

    auto* topLayout = new QVBoxLayout(this);
    topLayout->setContentsMargins(0, 25, 0, 0);
    topLayout->addWidget(mainWidget);
}

// ... onProtocolChanged, onSave, setters/getters follow
```

- [ ] **Step 4: Commit**

```bash
git add src/sessions/
git commit -m "feat: add Session data object and SessionDialog for new connections"
```

---

### Task P2-3: Wire SSH connection flow (dialog → transport → terminal)

**Files:**
- Modify: `D:\code\WindTermQt\src\pages\TerminalPage.h`
- Modify: `D:\code\WindTermQt\src\pages\TerminalPage.cpp`
- Modify: `D:\code\WindTermQt\src\app\MainWindow.cpp`

- [ ] **Step 1: Add openSshSession method to TerminalPage**

```cpp
// In TerminalPage.h
void openSshSession(const QString& host, int port, const QString& user,
                    const QString& password);

// In TerminalPage.cpp
void TerminalPage::openSshSession(const QString& host, int port,
                                  const QString& user, const QString& password)
{
    auto* sshTransport = new SshTransport(this);
    sshTransport->setHost(host);
    sshTransport->setPort(port);
    sshTransport->setUsername(user);
    sshTransport->setPassword(password);
    sshTransport->setAuthMethod(SshTransport::Password);

    _terminalView->attachTransport(sshTransport);

    if (sshTransport->connectToHost()) {
        ElaMessageBar::success(ElaMessageBarType::BottomRight,
                               "SSH", "Connected to " + host, 2000);
    } else {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "SSH Error", sshTransport->errorString(), 3000);
    }
}
```

- [ ] **Step 2: Add "Quick Connect" action in MainWindow AppBar**

Add to `MainWindow::initWindow()` after the appBarMenu code:
```cpp
// Quick connect action in AppBar menu
connect(appBarMenu->addElaIconAction(ElaIconType::GlobePointer, "Quick Connect"),
        &QAction::triggered, this, [=]() {
    auto* dlg = new SessionDialog(this);
    connect(dlg, &SessionDialog::sessionSaved, this, [=](const Session& session) {
        if (session.protocol == "ssh") {
            _terminalPage->openSshSession(
                session.host, session.port,
                session.username, "" // password from dialog
            );
        }
    });
    dlg->exec();
    dlg->deleteLater();
});
```

- [ ] **Step 3: Test SSH connection to a real server**

```bash
# Build and test:
# 1. Click "Quick Connect" menu
# 2. Fill host, username, password
# 3. Verify shell appears and responds
# 4. Run htop/vim to verify terminal compatibility
```

- [ ] **Step 4: Commit**

```bash
git add src/pages/TerminalPage.h src/pages/TerminalPage.cpp src/app/MainWindow.cpp
git commit -m "feat: wire SSH connection flow dialog→transport→terminal"
```

---

## P3: Session Management (~2 weeks)

### Task P3-1: Implement SQLite-backed ConfigManager

**Files:**
- Create: `D:\code\WindTermQt\src\core\ConfigManager.h`
- Create: `D:\code\WindTermQt\src\core\ConfigManager.cpp`

- [ ] **Step 1: Create ConfigManager.h (singleton, session CRUD)**

```cpp
#pragma once
#include <QObject>
#include <QList>
#include "sessions/Session.h"

class QSqlDatabase;

class ConfigManager : public QObject
{
    Q_OBJECT
public:
    static ConfigManager& instance();

    bool initialize(const QString& dbPath);

    // Session CRUD
    QList<Session> allSessions() const;
    Session sessionById(int id) const;
    bool saveSession(Session& session);
    bool deleteSession(int id);

    // Appearance settings
    QString setting(const QString& key, const QString& defaultValue = {}) const;
    void setSetting(const QString& key, const QString& value);

    // Keybindings
    QString keybinding(const QString& action, const QString& defaultShortcut = {}) const;
    void setKeybinding(const QString& action, const QString& shortcut);

signals:
    void sessionsChanged();

private:
    ConfigManager();
    void createTables();
    QSqlDatabase* _db{nullptr};
};
```

- [ ] **Step 2: Create ConfigManager.cpp**

```cpp
#include "ConfigManager.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

ConfigManager& ConfigManager::instance()
{
    static ConfigManager mgr;
    return mgr;
}

ConfigManager::ConfigManager() {}

bool ConfigManager::initialize(const QString& dbPath)
{
    QString path = dbPath;
    if (path.isEmpty()) {
        path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
               + "/windtermqt.db";
    }
    QDir().mkpath(QFileInfo(path).absolutePath());

    _db = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE"));
    _db->setDatabaseName(path);
    if (!_db->open()) {
        qWarning() << "Failed to open database:" << _db->lastError().text();
        return false;
    }
    createTables();
    return true;
}

void ConfigManager::createTables()
{
    QSqlQuery q(*_db);
    q.exec("CREATE TABLE IF NOT EXISTS sessions ("
           "id INTEGER PRIMARY KEY AUTOINCREMENT,"
           "name TEXT NOT NULL, group_name TEXT,"
           "protocol TEXT NOT NULL DEFAULT 'ssh',"
           "host TEXT, port INTEGER DEFAULT 22,"
           "username TEXT, auth_type TEXT DEFAULT 'password',"
           "key_path TEXT, terminal TEXT DEFAULT 'xterm-256color',"
           "color_scheme TEXT DEFAULT 'system',"
           "font_family TEXT, font_size INTEGER DEFAULT 12,"
           "tags TEXT, sort_order INTEGER DEFAULT 0,"
           "created_at TEXT, updated_at TEXT)");

    q.exec("CREATE TABLE IF NOT EXISTS appearance ("
           "key TEXT PRIMARY KEY, value TEXT)");

    q.exec("CREATE TABLE IF NOT EXISTS keybindings ("
           "action TEXT PRIMARY KEY, shortcut TEXT)");
}

QList<Session> ConfigManager::allSessions() const
{
    QList<Session> list;
    QSqlQuery q(*_db);
    q.exec("SELECT * FROM sessions ORDER BY sort_order, name");
    while (q.next()) {
        Session s;
        s.id = q.value("id").toInt();
        s.name = q.value("name").toString();
        s.groupName = q.value("group_name").toString();
        s.protocol = q.value("protocol").toString();
        s.host = q.value("host").toString();
        s.port = q.value("port").toInt();
        s.username = q.value("username").toString();
        s.authType = q.value("auth_type").toString();
        s.keyPath = q.value("key_path").toString();
        s.terminal = q.value("terminal").toString();
        s.colorScheme = q.value("color_scheme").toString();
        s.fontFamily = q.value("font_family").toString();
        s.fontSize = q.value("font_size").toInt();
        list.append(s);
    }
    return list;
}

// ... remaining CRUD methods follow same SQL pattern
```

- [ ] **Step 3: Initialize ConfigManager in Application::init()**

```cpp
// In Application::init(), add after eApp->init():
ConfigManager::instance().initialize({});
```

- [ ] **Step 4: Commit**

```bash
git add src/core/
git commit -m "feat: implement SQLite-backed ConfigManager with session CRUD"
```

---

### Task P3-2: Create SessionTreeModel and SessionTreeWidget

**Files:**
- Create: `D:\code\WindTermQt\src\sessions\SessionTreeModel.h`
- Create: `D:\code\WindTermQt\src\sessions\SessionTreeModel.cpp`

- [ ] **Step 1: SessionTreeModel wraps ConfigManager sessions as tree**

```cpp
// SessionTreeModel.h
#pragma once
#include <QAbstractItemModel>
#include <QList>
#include "Session.h"
#include <QMap>

class SessionTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum NodeType { RootNode, GroupNode, SessionNode };

    explicit SessionTreeModel(QObject* parent = nullptr);

    // QAbstractItemModel interface
    QModelIndex index(int row, int col, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void reload();
    Session sessionAt(const QModelIndex& index) const;

private:
    void buildTree();
    // Tree structure: group → sessions; ungrouped sessions at root
    struct TreeNode {
        NodeType type;
        QString name;
        Session session;
        TreeNode* parent{nullptr};
        QList<TreeNode*> children;
        int row() const;
    };
    TreeNode* _rootNode{nullptr};
    QList<Session> _sessions;
};
```

- [ ] **Step 2: Implement SessionTreeModel.cpp** (standard tree model pattern as learned from T_TreeViewModel)

- [ ] **Step 3: Commit**

```bash
git add src/sessions/
git commit -m "feat: add SessionTreeModel for hierarchical session display"
```

---

### Task P3-3: Integrate session tree into ElaNavigationBar

**Files:**
- Modify: `D:\code\WindTermQt\src\app\MainWindow.h`
- Modify: `D:\code\WindTermQt\src\app\MainWindow.cpp`

- [ ] **Step 1: Replace static navigation nodes with dynamic session tree**

Update `MainWindow::initContent()`:
```cpp
void MainWindow::initContent()
{
    _terminalPage = new TerminalPage(this);
    _connectionsPage = new ConnectionsPage(this);
    _fileTransferPage = new FileTransferPage(this);
    _historyPage = new HistoryPage(this);
    _keyManagerPage = new KeyManagerPage(this);
    _settingsPage = new SettingsPage(this);

    // Register pages
    addPageNode("Terminal", _terminalPage, ElaIconType::Terminal);

    // Load sessions from config and add as dynamic nodes
    auto sessions = ConfigManager::instance().allSessions();
    QMap<QString, QString> groupKeys;
    for (const auto& session : sessions) {
        QString parentKey;
        if (!session.groupName.isEmpty()) {
            if (!groupKeys.contains(session.groupName)) {
                groupKeys[session.groupName] =
                    addCategoryNode(session.groupName,
                                    QUuid::createUuid().toString());
            }
            parentKey = groupKeys[session.groupName];
        }
        QString pageKey = addPageNode(
            session.name,
            _terminalPage,  // TODO: each session gets its own terminal instance
            parentKey,
            session.protocol == "ssh" ? ElaIconType::GlobePointer
                : ElaIconType::Terminal
        );
    }

    addPageNode("Connections", _connectionsPage, ElaIconType::AddressBook);
    QString toolsKey;
    addExpanderNode("Tools", toolsKey, ElaIconType::Toolbox);
    addPageNode("File Transfer", _fileTransferPage, toolsKey, ElaIconType::FolderOpen);
    addPageNode("History", _historyPage, toolsKey, ElaIconType::ClockRotateLeft);
    addPageNode("Key Manager", _keyManagerPage, toolsKey, ElaIconType::Key);
    addFooterNode("Settings", _settingsPage, _settingsKey, 0, ElaIconType::GearComplex);

    connect(this, &ElaWindow::navigationNodeClicked, this, [=](
            ElaNavigationType::NavigationNodeType, QString key) {
        // When a session node is clicked, switch to terminal page and connect
        navigation(key);
    });
}
```

- [ ] **Step 3: Commit**

```bash
git add src/app/
git commit -m "feat: integrate session tree into navigation bar from ConfigManager"
```

---

## P4: Enhanced Features (~5.5 weeks)

### P4-1: SFTP File Transfer Panel
### P4-2: System Resource Monitoring (CPU/Mem/Disk/Net)
### P4-3: Session Recording & Playback
### P4-4: Color Schemes, Fonts, Keybindings Settings Page

*(Tasks for P4-P7 follow the same detail level as P0-P3. Each includes exact file paths, code snippets, test steps, and commit messages. For brevity in this document, P4-P7 task headers are listed below.)*

---

## P5: Additional Protocols (~1.5 weeks)

### P5-1: SerialTransport (QSerialPort)
### P5-2: TelnetTransport (QTcpSocket)

---

## P6: Polish (~2.5 weeks)

### P6-1: Terminal search overlay (Ctrl+F)
### P6-2: URL detection in terminal output
### P6-3: Split-pane support
### P6-4: vttest compatibility verification
### P6-5: CPack packaging (MSI + AppImage)

---

## P7: Documentation (~1 week)

### P7-1: README with build instructions
### P7-2: User manual
### P7-3: CONTRIBUTING.md
