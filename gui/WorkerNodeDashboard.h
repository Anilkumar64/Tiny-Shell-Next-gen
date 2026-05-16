#pragma once
#include "ApiClient.h"
#include <QLabel>
#include <QProgressBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Dashboard showing worker node status, health, CPU/RAM usage
class WorkerNodeDashboard : public QWidget {
  Q_OBJECT
public:
  explicit WorkerNodeDashboard(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUI();
    startPolling();
  }

private slots:
  void pollWorkers() {
    m_api->getText("/control/events", [this](QString body, QString err) {
      if (!err.isEmpty()) {
        m_statusLbl->setText("⚠ Stream offline");
        return;
      }
      QJsonParseError pe;
      const auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
      if (pe.error != QJsonParseError::NoError)
        return;

      const auto workers = doc.object().value("workers").toArray();
      updateWorkerTable(workers);
      m_statusLbl->setText("● Active · " + QString::number(workers.size()) +
                           " workers");
    });
  }

private:
  void buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    auto *titleLbl = new QLabel("Worker Node Dashboard", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    root->addWidget(titleLbl);

    m_statusLbl = new QLabel("● Active · 0 workers", this);
    m_statusLbl->setStyleSheet(
        "color:#4ade80; font-size:12px; font-weight:600;");
    root->addWidget(m_statusLbl);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(8);
    m_table->setHorizontalHeaderLabels({"Node ID", "State", "CPU", "RAM",
                                        "Active Jobs", "Last Heartbeat",
                                        "Command", "Health"});
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

  void updateWorkerTable(const QJsonArray &workers) {
    m_table->setRowCount(0);
    for (int i = 0; i < workers.size(); ++i) {
      const auto w = workers[i].toObject();
      const int row = m_table->rowCount();
      m_table->insertRow(row);

      const auto nodeId =
          w.value("node_id").toString("worker-" + QString::number(i + 1));
      const auto state = w.value("online").toBool() ? "● Online" : "⊘ Offline";
      const int cpu = w.value("cpu_usage").toInt(0);
      const int ram = w.value("ram_usage").toInt(0);

      m_table->setItem(row, 0, new QTableWidgetItem(nodeId));
      m_table->setItem(row, 1, new QTableWidgetItem(state));
      m_table->setItem(row, 2,
                       new QTableWidgetItem(QString::number(cpu) + "%"));
      m_table->setItem(row, 3,
                       new QTableWidgetItem(QString::number(ram) + "%"));
      m_table->setItem(row, 4,
                       new QTableWidgetItem(
                           QString::number(w.value("active_jobs").toInt(0))));
      m_table->setItem(
          row, 5,
          new QTableWidgetItem(w.value("last_heartbeat").toString("now")));
      m_table->setItem(
          row, 6,
          new QTableWidgetItem(w.value("running_command").toString("-")));

      const int health = w.value("health_score").toInt(100);
      auto *healthItem = new QTableWidgetItem();
      healthItem->setText(QString::number(health) + "%");
      if (health >= 80)
        healthItem->setForeground(QColor("#4ade80"));
      else if (health >= 50)
        healthItem->setForeground(QColor("#fbbf24"));
      else
        healthItem->setForeground(QColor("#f87171"));
      m_table->setItem(row, 7, healthItem);
    }
  }

  void startPolling() {
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &WorkerNodeDashboard::pollWorkers);
    timer->start(1500);
    pollWorkers();
  }

  tsh::ApiClient *m_api = nullptr;
  QTableWidget *m_table = nullptr;
  QLabel *m_statusLbl = nullptr;
};
