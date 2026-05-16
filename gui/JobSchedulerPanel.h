#pragma once
#include "ApiClient.h"
#include <QLabel>
#include <QProgressBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Shows job queue state: queued, running, completed, failed jobs
class JobSchedulerPanel : public QWidget {
  Q_OBJECT
public:
  explicit JobSchedulerPanel(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUI();
    startPolling();
  }

private slots:
  void pollJobs() {
    m_api->getText("/control/events", [this](QString body, QString err) {
      if (!err.isEmpty()) {
        m_statusLbl->setText("⚠ Stream offline");
        return;
      }
      QJsonParseError pe;
      const auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
      if (pe.error != QJsonParseError::NoError)
        return;

      const auto events = doc.object().value("events").toArray();
      updateJobStats(events);
      updateJobTable(events);
    });
  }

private:
  void buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    auto *titleLbl = new QLabel("Job Scheduler", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    root->addWidget(titleLbl);

    m_statusLbl = new QLabel("Loading…", this);
    m_statusLbl->setStyleSheet(
        "color:#4ade80; font-size:12px; font-weight:600;");
    root->addWidget(m_statusLbl);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        {"Job ID", "User", "Command", "State", "Worker", "Progress"});
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

  void updateJobStats(const QJsonArray &events) {
    int queued = 0, running = 0, completed = 0, failed = 0;

    for (const auto &evt : events) {
      const auto obj = evt.toObject();
      const auto state = obj.value("state").toString();

      if (state == "queued")
        queued++;
      else if (state == "running")
        running++;
      else if (state == "completed")
        completed++;
      else if (state == "failed")
        failed++;
    }

    m_statusLbl->setText(
        QString("Queued:%1  Running:%2  Completed:%3  Failed:%4")
            .arg(queued)
            .arg(running)
            .arg(completed)
            .arg(failed));
  }

  void updateJobTable(const QJsonArray &events) {
    m_table->setRowCount(0);
    int job_count = 0;

    for (int i = events.size() - 1; i >= 0 && job_count < 30; --i) {
      const auto evt = events[i].toObject();
      const auto state = evt.value("state").toString();

      if (state.isEmpty() || state == "parsed" || state == "rbac_passed")
        continue;

      const int row = m_table->rowCount();
      m_table->insertRow(row);

      auto *stateItem = new QTableWidgetItem(state);
      if (state == "completed")
        stateItem->setForeground(QColor("#4ade80"));
      else if (state == "failed")
        stateItem->setForeground(QColor("#f87171"));
      else if (state == "running")
        stateItem->setForeground(QColor("#fbbf24"));

      m_table->setItem(
          row, 0,
          new QTableWidgetItem(evt.value("request_id").toString().left(12)));
      m_table->setItem(row, 1,
                       new QTableWidgetItem(evt.value("user").toString()));
      m_table->setItem(row, 2,
                       new QTableWidgetItem(evt.value("command").toString()));
      m_table->setItem(row, 3, stateItem);
      m_table->setItem(row, 4,
                       new QTableWidgetItem(evt.value("worker").toString()));

      auto *progressItem = new QTableWidgetItem("●");
      if (state == "running")
        progressItem->setForeground(QColor("#fbbf24"));
      else if (state == "completed")
        progressItem->setForeground(QColor("#4ade80"));
      m_table->setItem(row, 5, progressItem);

      job_count++;
    }
  }

  void startPolling() {
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &JobSchedulerPanel::pollJobs);
    timer->start(1000);
    pollJobs();
  }

  tsh::ApiClient *m_api = nullptr;
  QTableWidget *m_table = nullptr;
  QLabel *m_statusLbl = nullptr;
};
