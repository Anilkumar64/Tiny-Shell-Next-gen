#pragma once

#include "ApiClient.h"
#include "AuditLogPanel.h"
#include "ClientConnectionPanel.h"
#include "ExecutionEventStreamWidget.h"
#include "ExecutionTraceView.h"
#include "JobSchedulerPanel.h"
#include "SecurityEventsPanel.h"
#include "WorkerNodeDashboard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

// ServerControlPanelPage - embedded version of ServerControlPanel for use in
// MainWindow
class ServerControlPanelPage : public QWidget {
  Q_OBJECT
public:
  explicit ServerControlPanelPage(tsh::ApiClient *api,
                                  QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUI();
  }

  // Expose the live event stream so MainWindow can forward job-submit commands.
  ExecutionEventStreamWidget *eventStream() const { return m_eventBusPage; }

private:
  void buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(28, 18, 28, 0);
    header->setSpacing(8);

    struct NavItem {
      const char *label;
      int index;
    };
    const NavItem items[] = {
        {"Live Events", 0}, {"Clients", 1}, {"Workers", 2}, {"Audit", 3},
        {"Security", 4},    {"Jobs", 5},    {"Trace", 6}};

    for (const auto &item : items) {
      auto *btn = new QPushButton(item.label, this);
      btn->setCheckable(true);
      btn->setFixedHeight(34);
      btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      btn->setStyleSheet(navButtonStyle(false));
      connect(btn, &QPushButton::clicked, this,
              [this, index = item.index]() { navigate(index); });
      m_navButtons.push_back(btn);
      header->addWidget(btn);
    }
    header->addStretch();
    root->addLayout(header);

    m_stack = new QStackedWidget(this);

    m_eventBusPage = new ExecutionEventStreamWidget(m_api);
    m_clientsPage = new ClientConnectionPanel(m_api);
    m_workersPage = new WorkerNodeDashboard(m_api);
    m_auditPage = new AuditLogPanel(m_api);
    m_securityPage = new SecurityEventsPanel(m_api);
    m_jobsPage = new JobSchedulerPanel(m_api);
    m_tracePage = new ExecutionTraceView(m_api);

    m_stack->addWidget(m_eventBusPage);
    m_stack->addWidget(m_clientsPage);
    m_stack->addWidget(m_workersPage);
    m_stack->addWidget(m_auditPage);
    m_stack->addWidget(m_securityPage);
    m_stack->addWidget(m_jobsPage);
    m_stack->addWidget(m_tracePage);

    root->addWidget(m_stack, 1);
    navigate(0);
  }

  void navigate(int index) {
    m_stack->setCurrentIndex(index);
    for (int i = 0; i < m_navButtons.size(); ++i) {
      m_navButtons[i]->setChecked(i == index);
      m_navButtons[i]->setStyleSheet(navButtonStyle(i == index));
    }
  }

  static QString navButtonStyle(bool active) {
    if (active) {
      return "QPushButton { background:#1a2d3e; border:1px solid #00b4d8;"
             " border-radius:6px; color:#e8eaf0; padding:0 14px;"
             " font-weight:600; }";
    }
    return "QPushButton { background:#1c1f2b; border:1px solid #2a2d3e;"
           " border-radius:6px; color:#8891b0; padding:0 14px;"
           " font-weight:500; }"
           "QPushButton:hover { border-color:#00b4d8; color:#e8eaf0; }";
  }

  tsh::ApiClient *m_api = nullptr;
  QStackedWidget *m_stack = nullptr;
  QVector<QPushButton *> m_navButtons;
  ExecutionEventStreamWidget *m_eventBusPage = nullptr;
  ClientConnectionPanel *m_clientsPage = nullptr;
  WorkerNodeDashboard *m_workersPage = nullptr;
  AuditLogPanel *m_auditPage = nullptr;
  SecurityEventsPanel *m_securityPage = nullptr;
  JobSchedulerPanel *m_jobsPage = nullptr;
  ExecutionTraceView *m_tracePage = nullptr;
};