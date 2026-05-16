#pragma once

#include "ApiClient.h"
#include "AuditLogPanel.h"
#include "ClientConnectionPanel.h"
#include "ExecutionEventStreamWidget.h"
#include "ExecutionTraceView.h"
#include "JobSchedulerPanel.h"
#include "SecurityEventsPanel.h"
#include "TshStyle.h"
#include "WorkerNodeDashboard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <memory>

// Professional Qt6 Server Admin Control Panel
// Real-time monitoring of cluster execution, clients, audit logs, and security
class ServerControlPanel : public QMainWindow {
  Q_OBJECT
public:
  explicit ServerControlPanel(
      const QString &serverUrl = "https://127.0.0.1:8080",
      const QString &token = "")
      : QMainWindow() {
    m_api = std::make_unique<tsh::ApiClient>();
    m_api->setBaseUrl(serverUrl);
    m_api->setToken(token);

    buildUI();
    applyTheme();
    setWindowTitle("TinyShell Server Control Panel");
    resize(1800, 1000);
    show();
  }

  ~ServerControlPanel() = default;

private:
  void buildUI() {
    // Central widget with sidebar + stacked pages
    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // ── Left Sidebar ─────────────────────────────────────────────────────
    auto *sidebar = createSidebar();
    mainLayout->addWidget(sidebar, 0);

    // ── Main Content Area ────────────────────────────────────────────────
    m_stack = new QStackedWidget(this);
    m_stack->setStyleSheet("background:#0f1117;");

    m_eventBusPage = new ExecutionEventStreamWidget(m_api.get());
    m_clientsPage = new ClientConnectionPanel(m_api.get());
    m_workersPage = new WorkerNodeDashboard(m_api.get());
    m_auditPage = new AuditLogPanel(m_api.get());
    m_securityPage = new SecurityEventsPanel(m_api.get());
    m_jobsPage = new JobSchedulerPanel(m_api.get());
    m_tracePage = new ExecutionTraceView(m_api.get());

    m_stack->addWidget(m_eventBusPage);
    m_stack->addWidget(m_clientsPage);
    m_stack->addWidget(m_workersPage);
    m_stack->addWidget(m_auditPage);
    m_stack->addWidget(m_securityPage);
    m_stack->addWidget(m_jobsPage);
    m_stack->addWidget(m_tracePage);

    mainLayout->addWidget(m_stack, 1);

    setCentralWidget(centralWidget);
  }

  QWidget *createSidebar() {
    auto *sidebar = new QWidget(this);
    sidebar->setFixedWidth(220);
    sidebar->setStyleSheet(
        "background:#13151c; border-right:1px solid #2a2d3e;");

    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 20, 0, 20);
    layout->setSpacing(8);

    // Title
    auto *titleLbl = new QLabel("Server Control", sidebar);
    titleLbl->setStyleSheet(
        "color:#e8eaf0; font-size:16px; font-weight:700; padding:0 16px;");
    layout->addWidget(titleLbl);

    // Navigation items
    struct NavItem {
      const char *icon;
      const char *label;
      int index;
    };

    const NavItem items[] = {
        {"📡", "Live Events", 0}, {"👥", "Clients", 1}, {"⚙️", "Workers", 2},
        {"📋", "Audit Log", 3},   {"🛡️", "Security", 4}, {"📊", "Jobs", 5},
        {"🔗", "Trace", 6},
    };

    for (const auto &item : items) {
      auto *btn = new QPushButton(
          QString::fromUtf8(item.icon) + "  " + item.label, sidebar);
      btn->setFlat(true);
      btn->setMinimumHeight(40);
      btn->setCursor(Qt::PointingHandCursor);
      btn->setStyleSheet("QPushButton {"
                         "  background: transparent;"
                         "  border: none;"
                         "  color: #8891b0;"
                         "  text-align: left;"
                         "  padding: 0 16px;"
                         "  font-weight: 500;"
                         "  font-size: 13px;"
                         "}"
                         "QPushButton:hover {"
                         "  background: #1c1f2b;"
                         "  color: #00b4d8;"
                         "}"
                         "QPushButton:pressed {"
                         "  background: #0077a8;"
                         "  color: #e8eaf0;"
                         "}");

      connect(btn, &QPushButton::clicked, this, [this, index = item.index]() {
        m_stack->setCurrentIndex(index);
      });

      layout->addWidget(btn);
    }

    layout->addStretch();

    // Status indicator
    m_statusLabel = new QLabel("● Live", sidebar);
    m_statusLabel->setStyleSheet(
        "color:#4ade80; font-size:11px; font-weight:600; padding:0 16px;");
    layout->addWidget(m_statusLabel);

    return sidebar;
  }

  void applyTheme() { setStyleSheet(TshStyle::appStyleSheet()); }

  std::unique_ptr<tsh::ApiClient> m_api;
  QStackedWidget *m_stack = nullptr;
  QLabel *m_statusLabel = nullptr;

  ExecutionEventStreamWidget *m_eventBusPage = nullptr;
  ClientConnectionPanel *m_clientsPage = nullptr;
  WorkerNodeDashboard *m_workersPage = nullptr;
  AuditLogPanel *m_auditPage = nullptr;
  SecurityEventsPanel *m_securityPage = nullptr;
  JobSchedulerPanel *m_jobsPage = nullptr;
  ExecutionTraceView *m_tracePage = nullptr;
};
