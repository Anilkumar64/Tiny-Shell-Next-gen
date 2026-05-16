#pragma once
#include "ApiClient.h"
#include "AuditWidget.h"
#include "ClusterWidget.h"
#include "ConfigWidget.h"
#include "DashboardWidget.h"
#include "JobWidget.h"
#include "LiveJobWidget.h"
#include "RbacWidget.h"
#include "ServerControlPanelPage.h"
#include "TenantWidget.h"
#include "TerminalWidget.h"
#include "TshStyle.h"
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  // url and token are resolved from CLI before the window is constructed,
  // so m_api is fully configured before buildUi() creates any polling widget.
  // Constructing with a blank token was Bug B: all 7 server-panel widgets fired
  // an immediate poll() in their constructor and each got "Bearer token
  // rejected" — one auth failure per widget, hence exactly 7 log events.
  explicit MainWindow(
      QWidget *parent = nullptr,
      const QString &apiUrl = QStringLiteral("https://127.0.0.1:8080"),
      const QString &apiToken = {})
      : QMainWindow(parent) {
    setWindowTitle("TinyShell NextGen — Control Panel");
    setMinimumSize(1100, 720);
    resize(1440, 900);

    m_api = new tsh::ApiClient(this);
    // Configure BEFORE buildUi() so every child widget gets a valid token on
    // its very first request.
    m_api->setBaseUrl(apiUrl);
    if (!apiToken.isEmpty())
      m_api->setToken(apiToken);

    buildUi();
    setStyleSheet(TshStyle::appStyleSheet());

    // Status bar clock
    auto *clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, [this] {
      statusBar()->showMessage(
          "TinyShell NextGen  ·  " +
          QDateTime::currentDateTime().toString("ddd dd MMM yyyy  hh:mm:ss"));
    });
    clockTimer->start(1000);
    statusBar()->showMessage(
        "TinyShell NextGen  ·  " +
        QDateTime::currentDateTime().toString("ddd dd MMM yyyy  hh:mm:ss"));
  }

private:
  void buildUi() {
    auto *central = new QWidget(this);
    auto *mainRow = new QHBoxLayout(central);
    mainRow->setContentsMargins(0, 0, 0, 0);
    mainRow->setSpacing(0);
    setCentralWidget(central);

    // ── Sidebar ──────────────────────────────────────────────────────────
    auto *sidebar = new QFrame(central);
    sidebar->setFixedWidth(220);
    sidebar->setStyleSheet("QFrame { background-color: #13151c;"
                           " border-right: 1px solid #1e2030; }");

    auto *sideVbox = new QVBoxLayout(sidebar);
    sideVbox->setContentsMargins(12, 16, 12, 12);
    sideVbox->setSpacing(4);

    // Logo / brand
    auto *brandRow = new QHBoxLayout;
    auto *logoLbl = new QLabel("⬡", sidebar);
    logoLbl->setStyleSheet(
        "font-size:22px; color:#00b4d8; background:transparent;");
    auto *brandLbl = new QLabel("TinyShell", sidebar);
    brandLbl->setStyleSheet("font-size:16px; font-weight:700; color:#e8eaf0; "
                            "background:transparent;");
    auto *versionLbl = new QLabel("v3.0", sidebar);
    versionLbl->setStyleSheet("font-size:10px; color:#4a5068; "
                              "background:transparent; margin-top:2px;");
    brandRow->addWidget(logoLbl);
    auto *brandCol = new QVBoxLayout;
    brandCol->setSpacing(0);
    brandCol->addWidget(brandLbl);
    brandCol->addWidget(versionLbl);
    brandRow->addLayout(brandCol);
    brandRow->addStretch();
    sideVbox->addLayout(brandRow);
    sideVbox->addSpacing(8);

    // Divider
    sideVbox->addWidget(makeDivider(sidebar));
    sideVbox->addSpacing(6);

    // Nav section label
    sideVbox->addWidget(navSectionLabel("OVERVIEW", sidebar));

    // Nav buttons (icon, label, page index)
    struct NavEntry {
      QString icon;
      QString label;
    };
    const QList<NavEntry> navItems = {
        {"▶", "Execution"}, {"⊞", "Dashboard"}, {"▣", "Server"},
        {"◈", "Cluster"},   {"⊕", "Jobs"},      {"◉", "RBAC"},
        {"◫", "Tenants"},   {"≡", "Audit Log"}, {"⚙", "Settings"},
    };

    m_navButtons.reserve(navItems.size());
    for (int i = 0; i < navItems.size(); ++i) {
      const auto &e = navItems[i];
      auto *btn = new QPushButton(sidebar);
      btn->setCheckable(true);
      btn->setText("  " + e.icon + "   " + e.label);
      btn->setFixedHeight(40);
      btn->setStyleSheet(navBtnStyle());
      btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      connect(btn, &QPushButton::clicked, this,
              [this, i, btn] { navigate(i); });

      // Section dividers
      if (i == 3) {
        sideVbox->addSpacing(4);
        sideVbox->addWidget(navSectionLabel("CLUSTER", sidebar));
      }
      if (i == 5) {
        sideVbox->addSpacing(4);
        sideVbox->addWidget(navSectionLabel("SECURITY", sidebar));
      }
      if (i == 7) {
        sideVbox->addSpacing(4);
        sideVbox->addWidget(navSectionLabel("SYSTEM", sidebar));
      }

      sideVbox->addWidget(btn);
      m_navButtons.append(btn);
    }

    sideVbox->addStretch();
    sideVbox->addWidget(makeDivider(sidebar));
    sideVbox->addSpacing(6);

    // Connection status indicator
    m_connDot = new QLabel("● Connecting", sidebar);
    m_connDot->setStyleSheet("font-size:11px; color:#fbbf24; "
                             "background:transparent; padding:4px 8px;");
    sideVbox->addWidget(m_connDot);

    mainRow->addWidget(sidebar);

    // ── Content stack ─────────────────────────────────────────────────────
    m_stack = new QStackedWidget(central);
    m_stack->setStyleSheet("QStackedWidget { background-color: #0f1117; }");

    auto *live = new LiveJobWidget(m_stack);
    auto *dash = new DashboardWidget(m_api, m_stack);
    auto *server = new ServerControlPanelPage(m_api, m_stack);
    auto *cluster = new ClusterWidget(m_stack);
    auto *jobs = new JobWidget(m_stack);
    auto *rbac = new RbacWidget(m_stack);
    auto *tenants = new TenantWidget(m_stack);
    auto *audit = new AuditWidget(m_stack);
    auto *config = new ConfigWidget(m_api, m_stack);

    // Forward the command name from LiveJobWidget to the Server event stream
    // so spine gRPC events show the command in the "Command" column.
    connect(live, &LiveJobWidget::jobSubmitted, server->eventStream(),
            &ExecutionEventStreamWidget::notifyJobSubmit);

    for (QWidget *w : {(QWidget *)live, (QWidget *)dash, (QWidget *)server,
                       (QWidget *)cluster, (QWidget *)jobs, (QWidget *)rbac,
                       (QWidget *)tenants, (QWidget *)audit, (QWidget *)config})
      m_stack->addWidget(w);

    mainRow->addWidget(m_stack, 1);

    // Status bar
    statusBar()->setStyleSheet(
        "QStatusBar { background:#13151c; border-top:1px solid #1e2030;"
        " color:#4a5068; font-size:11px; }");

    // Start on dashboard
    navigate(0);

    // Poll reachability every 5s
    auto *reachTimer = new QTimer(this);
    connect(reachTimer, &QTimer::timeout, this, &MainWindow::pollReachability);
    reachTimer->start(5000);
    pollReachability();
  }

  void navigate(int idx) {
    m_stack->setCurrentIndex(idx);
    for (int i = 0; i < m_navButtons.size(); ++i) {
      m_navButtons[i]->setChecked(i == idx);
      m_navButtons[i]->setStyleSheet(navBtnStyle(i == idx));
    }
  }

  void pollReachability() {
    m_api->getText("/health", [this](QString, QString err) {
      const bool ok = err.isEmpty();
      m_connDot->setText(ok ? "● Connected" : "● Offline");
      m_connDot->setStyleSheet(
          QString("font-size:11px; color:%1;"
                  " background:transparent; padding:4px 8px;")
              .arg(ok ? "#4ade80" : "#f87171"));
    });
  }

  static QFrame *makeDivider(QWidget *parent) {
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("border: none; border-top: 1px solid #1e2030;");
    line->setFixedHeight(1);
    return line;
  }

  static QLabel *navSectionLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setStyleSheet(
        "font-size:9px; font-weight:700; letter-spacing:1.5px; color:#4a5068;"
        " background:transparent; padding:4px 8px 2px;");
    return l;
  }

  static QString navBtnStyle(bool active = false) {
    if (active) {
      return "QPushButton {"
             "  background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
             "    stop:0 #1a2d3e, stop:1 #1c2a3a);"
             "  border: none;"
             "  border-left: 3px solid #00b4d8;"
             "  border-radius: 6px;"
             "  color: #e8eaf0;"
             "  text-align: left;"
             "  padding: 0 10px;"
             "  font-weight: 600;"
             "  font-size: 13px;"
             "}";
    }
    return "QPushButton {"
           "  background: transparent;"
           "  border: none;"
           "  border-left: 3px solid transparent;"
           "  border-radius: 6px;"
           "  color: #8891b0;"
           "  text-align: left;"
           "  padding: 0 10px;"
           "  font-weight: 500;"
           "  font-size: 13px;"
           "}"
           "QPushButton:hover {"
           "  background: #1c1f2b;"
           "  color: #c8d0e0;"
           "}"
           "QPushButton:checked {"
           "  background: #1a2d3e;"
           "  border-left-color: #00b4d8;"
           "  color: #e8eaf0;"
           "  font-weight: 600;"
           "}";
  }

  tsh::ApiClient *m_api = nullptr;
  QStackedWidget *m_stack = nullptr;
  QList<QPushButton *> m_navButtons;
  QLabel *m_connDot = nullptr;
};