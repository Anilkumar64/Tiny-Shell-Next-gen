#pragma once
#include "ApiClient.h"
#include "GrpcJobClient.h"
#include "tinyshell/v1/spine.pb.h"
#include <QDateTime>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>
#include <vector>

// Real-time execution event stream.
// Merges two sources:
//   1. HTTP /control/events  — events from POST /exec (tsh_server)
//   2. Spine gRPC WatchJob   — events from the spine gRPC path (Execution tab)
class ExecutionEventStreamWidget : public QWidget {
  Q_OBJECT
public:
  explicit ExecutionEventStreamWidget(tsh::ApiClient *api,
                                      QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUI();

    // ── Spine gRPC watcher ─────────────────────────────────────────────
    // We use a dedicated GrpcJobClient wired to an internal slot so that
    // every job event that flows through the spine also appears here.
    m_grpcClient = new GrpcJobClient(this);
    const QByteArray target = qgetenv("TSH_SPINE_TARGET");
    m_grpcClient->configure(target.isEmpty() ? QStringLiteral("127.0.0.1:7443")
                                             : QString::fromLocal8Bit(target));

    connect(m_grpcClient, &GrpcJobClient::submitted, this,
            [this](QString jobId, QString msg) {
              addRow(QDateTime::currentDateTime().toString("hh:mm:ss.zzz"),
                     "job_submitted", jobId.left(12), "operator", m_lastCommand,
                     "submitted", msg);
            });
    connect(m_grpcClient, &GrpcJobClient::eventReceived, this,
            &ExecutionEventStreamWidget::onSpineEvent);
    connect(m_grpcClient, &GrpcJobClient::failed, this, [this](QString err) {
      addRow(QDateTime::currentDateTime().toString("hh:mm:ss.zzz"),
             "grpc_error", "-", "-", "-", "error", err);
    });

    startPolling();
  }

  // Called by LiveJobWidget when the user submits a job, so we know the
  // command name to display for subsequent spine events.
  void notifyJobSubmit(const QString &command) { m_lastCommand = command; }

private slots:
  void pollHttpEvents() {
    // BUG: live events repolled from the beginning and appeared empty/stale.
    // FIX: long-poll only events newer than the last received sequence.
    m_api->getText(QString("/control/events?since_sequence=%1").arg(m_lastHttpSeq),
                   [this](QString body, QString err) {
      if (!err.isEmpty()) {
        m_statusLbl->setText("⚠ HTTP stream error: " + err);
        return;
      }
      QJsonParseError pe;
      const auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
      if (pe.error != QJsonParseError::NoError) {
        m_statusLbl->setText("⚠ Invalid event JSON");
        return;
      }
      const auto events = doc.object().value("events").toArray();
      // Only add HTTP events that are newer than what we've already shown.
      int newCount = 0;
      for (int i = 0; i < events.size(); ++i) {
        const auto evt = events[i].toObject();
        const auto seq = static_cast<quint64>(evt.value("sequence").toDouble());
        if (seq > m_lastHttpSeq) {
          m_lastHttpSeq = seq;
          addRow(evt.value("timestamp").toString().right(12),
                 evt.value("type").toString(),
                 evt.value("request_id").toString().left(12),
                 evt.value("user").toString(), evt.value("command").toString(),
                 evt.value("state").toString(), evt.value("detail").toString());
          ++newCount;
        }
      }
      updateStatus();
    });
  }

  void onSpineEvent(tinyshell::v1::JobEvent event) {
    const QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString type;
    switch (event.type()) {
    case tinyshell::v1::JOB_CREATED:
      type = "job_created";
      break;
    case tinyshell::v1::JOB_VALIDATED:
      type = "job_validated";
      break;
    case tinyshell::v1::JOB_SIGNED:
      type = "job_signed";
      break;
    case tinyshell::v1::JOB_ASSIGNED:
      type = "job_assigned";
      break;
    case tinyshell::v1::JOB_DELIVERED:
      type = "job_delivered";
      break;
    case tinyshell::v1::JOB_AGENT_ACCEPTED:
      type = "agent_accept";
      break;
    case tinyshell::v1::JOB_STARTED:
      type = "job_started";
      break;
    case tinyshell::v1::JOB_STDOUT:
      type = "stdout";
      break;
    case tinyshell::v1::JOB_STDERR:
      type = "stderr";
      break;
    case tinyshell::v1::JOB_EXITED:
      type = "job_exited";
      break;
    case tinyshell::v1::JOB_FAILED:
      type = "job_failed";
      break;
    case tinyshell::v1::JOB_TIMED_OUT:
      type = "timed_out";
      break;
    case tinyshell::v1::JOB_KILLED:
      type = "killed";
      break;
    case tinyshell::v1::JOB_LOST:
      type = "job_lost";
      break;
    case tinyshell::v1::JOB_REJECTED:
      type = "job_rejected";
      break;
    case tinyshell::v1::AUDIT_RECORDED:
      type = "audit";
      break;
    default:
      type = "event";
      break;
    }

    QString detail = QString::fromStdString(event.message());
    if (event.has_output()) {
      const auto &out = event.output();
      detail = QString::fromStdString(out.data()).trimmed().left(80);
    }
    if (event.has_exit()) {
      detail = "exit_code=" + QString::number(event.exit().exit_code()) + " " +
               QString::fromStdString(event.exit().reason());
    }

    const QString state = [&] {
      switch (event.state()) {
      case tinyshell::v1::JOB_STATE_PENDING:
        return "pending";
      case tinyshell::v1::JOB_STATE_ASSIGNED:
        return "assigned";
      case tinyshell::v1::JOB_STATE_DELIVERED:
        return "delivered";
      case tinyshell::v1::JOB_STATE_STARTING:
        return "starting";
      case tinyshell::v1::JOB_STATE_RUNNING:
        return "running";
      case tinyshell::v1::JOB_STATE_STREAMING:
        return "streaming";
      case tinyshell::v1::JOB_STATE_EXITED:
        return "exited";
      case tinyshell::v1::JOB_STATE_FAILED:
        return "failed";
      case tinyshell::v1::JOB_STATE_TIMED_OUT:
        return "timed_out";
      case tinyshell::v1::JOB_STATE_KILLED:
        return "killed";
      case tinyshell::v1::JOB_STATE_LOST:
        return "lost";
      case tinyshell::v1::JOB_STATE_REJECTED:
        return "rejected";
      default:
        return "unknown";
      }
    }();

    addRow(ts, type, QString::fromStdString(event.job_id()).left(12),
           QString::fromStdString(event.actor()), m_lastCommand, state, detail);
    updateStatus();
  }

private:
  void buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    auto *titleLbl = new QLabel("Live Execution Event Stream", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    root->addWidget(titleLbl);

    m_statusLbl = new QLabel("● Live · 0 events", this);
    m_statusLbl->setStyleSheet(
        "color:#4ade80; font-size:12px; font-weight:600;");
    root->addWidget(m_statusLbl);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels(
        {"Time", "Type", "Request ID", "User", "Command", "State", "Detail"});
    m_table->setStyleSheet(
        "QTableWidget { background:#0a0c14; border:1px solid #2a2d3e; "
        "border-radius:8px; color:#e8eaf0; }"
        "QHeaderView::section { background:#1c1f2b; border:none; padding:8px; "
        "font-weight:600; color:#8891b0; }"
        "QTableWidget::item { padding:8px; border-bottom:1px solid #1c1f2b; }"
        "QTableWidget::item:selected { background:#0077a8; }");
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(false);
    root->addWidget(m_table, 1);
  }

  void addRow(const QString &time, const QString &type, const QString &reqId,
              const QString &user, const QString &command, const QString &state,
              const QString &detail) {
    // Keep newest at top — insert at row 0.
    m_table->insertRow(0);
    m_table->setItem(0, 0, new QTableWidgetItem(time));
    m_table->setItem(0, 1, new QTableWidgetItem(type));
    m_table->setItem(0, 2, new QTableWidgetItem(reqId));
    m_table->setItem(0, 3, new QTableWidgetItem(user));
    m_table->setItem(0, 4, new QTableWidgetItem(command));
    m_table->setItem(0, 5, new QTableWidgetItem(state));
    m_table->setItem(0, 6, new QTableWidgetItem(detail));

    // Colour-code by event type
    QColor rowColor;
    if (type.contains("fail") || type.contains("error") ||
        type.contains("reject") || type.contains("killed"))
      rowColor = QColor(180, 60, 60, 40);
    else if (type == "stdout" || type == "job_exited")
      rowColor = QColor(60, 180, 100, 30);
    else if (type.contains("security") || type.contains("violation"))
      rowColor = QColor(220, 150, 0, 40);

    if (rowColor.isValid()) {
      for (int c = 0; c < m_table->columnCount(); ++c)
        if (auto *item = m_table->item(0, c))
          item->setBackground(rowColor);
    }

    // Cap at 200 rows
    while (m_table->rowCount() > 200)
      m_table->removeRow(m_table->rowCount() - 1);
  }

  void updateStatus() {
    m_statusLbl->setText("● Live · " + QString::number(m_table->rowCount()) +
                         " events");
  }

  void startPolling() {
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this,
            &ExecutionEventStreamWidget::pollHttpEvents);
    timer->start(1000);
    pollHttpEvents();
  }

  tsh::ApiClient *m_api = nullptr;
  GrpcJobClient *m_grpcClient = nullptr;
  QTableWidget *m_table = nullptr;
  QLabel *m_statusLbl = nullptr;
  quint64 m_lastHttpSeq = 0;
  QString m_lastCommand;
};
