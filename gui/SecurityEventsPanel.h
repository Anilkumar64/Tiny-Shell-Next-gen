#pragma once
#include "ApiClient.h"
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Shows all security violations: blocked commands, taint violations, failed
// auth, etc.
class SecurityEventsPanel : public QWidget {
  Q_OBJECT
public:
  explicit SecurityEventsPanel(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUI();
    startPolling();
  }

private slots:
  void pollSecurityEvents() {
    // BUG: security panel polled the general feed and missed auth denies.
    // FIX: read the dedicated /control/security feed.
    m_api->getText("/control/security", [this](QString body, QString err) {
      if (!err.isEmpty()) {
        m_statusLbl->setText("⚠ Stream offline");
        return;
      }
      QJsonParseError pe;
      const auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
      if (pe.error != QJsonParseError::NoError)
        return;

      const auto events = doc.object().value("events").toArray();
      updateSecurityTable(events);
    });
  }

private:
  void buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    auto *titleLbl = new QLabel("Security Events", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#f87171;");
    root->addWidget(titleLbl);

    m_statusLbl = new QLabel("Loading…", this);
    m_statusLbl->setStyleSheet(
        "color:#f87171; font-size:12px; font-weight:600;");
    root->addWidget(m_statusLbl);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        {"Time", "Type", "User", "Command", "Reason", "IP Address"});
    m_table->setStyleSheet(
        "QTableWidget { background:#0a0c14; border:1px solid #2a2d3e; "
        "border-radius:8px; color:#e8eaf0; }"
        "QHeaderView::section { background:#1c1f2b; border:none; padding:8px; "
        "font-weight:600; color:#8891b0; }"
        "QTableWidget::item { padding:8px; border-bottom:1px solid #1c1f2b; }"
        "QTableWidget::item:selected { background:#0077a8; }");
    m_table->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(m_table, 1);
  }

  void updateSecurityTable(const QJsonArray &events) {
    m_table->setRowCount(0);
    int violation_count = 0;

    for (int i = events.size() - 1; i >= std::max(0, (int)events.size() - 50);
         --i) {
      const auto evt = events[i].toObject();
      const auto type = evt.value("type").toString();

      const int row = m_table->rowCount();
      m_table->insertRow(row);

      violation_count++;

      auto *typeItem = new QTableWidgetItem();
      const auto reason = evt.value("detail").toString();
      if (reason.contains("Taint"))
        typeItem->setText("Taint Violation");
      else if (reason.contains("Command"))
        typeItem->setText("Blocked Command");
      else if (reason.contains("Auth"))
        typeItem->setText("Auth Failure");
      else
        typeItem->setText("Security Violation");
      typeItem->setForeground(QColor("#f87171"));

      m_table->setItem(
          row, 0,
          new QTableWidgetItem(evt.value("timestamp").toString().right(8)));
      m_table->setItem(row, 1, typeItem);
      m_table->setItem(row, 2,
                       new QTableWidgetItem(evt.value("user").toString()));
      m_table->setItem(row, 3,
                       new QTableWidgetItem(evt.value("command").toString()));
      m_table->setItem(row, 4, new QTableWidgetItem(reason));
      m_table->setItem(row, 5,
                       new QTableWidgetItem(evt.value("client_ip").toString()));
    }

    if (violation_count == 0) {
      m_statusLbl->setText("✓ No security violations");
      m_statusLbl->setStyleSheet(
          "color:#4ade80; font-size:12px; font-weight:600;");
    } else {
      m_statusLbl->setText("⚠ " + QString::number(violation_count) +
                           " security violations");
      m_statusLbl->setStyleSheet(
          "color:#f87171; font-size:12px; font-weight:600;");
    }
  }

  void startPolling() {
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this,
            &SecurityEventsPanel::pollSecurityEvents);
    timer->start(1000);
    pollSecurityEvents();
  }

  tsh::ApiClient *m_api = nullptr;
  QTableWidget *m_table = nullptr;
  QLabel *m_statusLbl = nullptr;
};
