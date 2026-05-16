#pragma once
#include "ApiClient.h"
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Visualizes execution flow: Client → Server → Worker → Result in real-time
class ExecutionTraceView : public QWidget {
  Q_OBJECT
public:
  explicit ExecutionTraceView(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUI();
    startPolling();
  }

private slots:
  void pollTraces() {
    // BUG: trace view polled raw events and built zero traces.
    // FIX: use the trace endpoint that returns execution trace objects.
    m_api->getText("/control/traces", [this](QString body, QString err) {
      if (!err.isEmpty()) {
        m_statusLbl->setText("⚠ Stream offline");
        return;
      }
      QJsonParseError pe;
      const auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
      if (pe.error != QJsonParseError::NoError)
        return;

      updateTraces(doc.array());
    });
  }

private:
  void buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    auto *titleLbl = new QLabel("Execution Trace Visualization", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    root->addWidget(titleLbl);

    m_statusLbl = new QLabel("Live trace view", this);
    m_statusLbl->setStyleSheet(
        "color:#4ade80; font-size:12px; font-weight:600;");
    root->addWidget(m_statusLbl);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border:1px solid #2a2d3e; "
                          "border-radius:8px; background:#0a0c14; }");

    m_traceContainer = new QWidget(scroll);
    m_traceLayout = new QVBoxLayout(m_traceContainer);
    m_traceLayout->setSpacing(20);
    m_traceLayout->setContentsMargins(20, 20, 20, 20);

    scroll->setWidget(m_traceContainer);
    root->addWidget(scroll, 1);
  }

  void updateTraces(const QJsonArray &events) {
    // Clear previous traces
    while (QLayoutItem *item = m_traceLayout->takeAt(0)) {
      delete item->widget();
      delete item;
    }

    // Group events by request_id
    QMap<QString, QJsonArray> traces;
    for (const auto &evt : events) {
      const auto obj = evt.toObject();
      const auto reqId = obj.value("request_id").toString();
      if (!reqId.isEmpty()) {
        traces[reqId].append(obj);
      }
    }

    // Display last 10 traces (iterate from end)
    int shown = 0;
    const auto keys = traces.keys();
    for (int i = keys.size() - 1; i >= 0 && shown < 10; --i, ++shown) {
      const auto evts = traces[keys[i]];
      if (evts.isEmpty())
        continue;

      auto *traceBox = createTraceBox(evts);
      m_traceLayout->addWidget(traceBox);
    }

    m_traceLayout->addStretch();
    m_statusLbl->setText("Displaying " + QString::number(shown) +
                         " execution traces");
  }

  QWidget *createTraceBox(const QJsonArray &events) {
    auto *container = new QWidget();
    auto *layout = new QVBoxLayout(container);
    layout->setSpacing(6);
    layout->setContentsMargins(14, 12, 14, 12);

    container->setStyleSheet("background:#1c1f2b; border:1px solid #2a2d3e; "
                             "border-radius:8px; margin-bottom:10px;");

    // Header with request ID and command
    const auto firstEvt = events[0].toObject();
    const auto reqId = firstEvt.value("request_id").toString().left(12);
    const auto cmd = firstEvt.value("command").toString();
    const auto user = firstEvt.value("user").toString();

    auto *headerLbl =
        new QLabel(QString("%1 [%2] %3").arg(reqId, user, cmd), container);
    headerLbl->setStyleSheet("font-weight:600; color:#00b4d8; font-size:11px;");
    layout->addWidget(headerLbl);

    // Timeline of events
    for (const auto &evt : events) {
      const auto obj = evt.toObject();
      const auto type = obj.value("type").toString();
      const auto timestamp = obj.value("timestamp").toString().right(8);
      const auto detail = obj.value("detail").toString();
      const auto state = obj.value("state").toString();
      const auto worker = obj.value("worker").toString();

      QString displayText;
      QColor color = QColor("#8891b0");

      if (type == "request_received") {
        displayText = QString("[%1] ➜ Client Request").arg(timestamp);
        color = QColor("#00b4d8");
      } else if (type == "authenticated") {
        displayText = QString("[%1] ✓ Authenticated").arg(timestamp);
        color = QColor("#4ade80");
      } else if (type == "ast_parsed") {
        displayText = QString("[%1] ✓ AST Parsed").arg(timestamp);
        color = QColor("#4ade80");
      } else if (type == "rbac_passed") {
        displayText = QString("[%1] ✓ RBAC Validated").arg(timestamp);
        color = QColor("#4ade80");
      } else if (type == "taint_passed") {
        displayText = QString("[%1] ✓ Taint Check OK").arg(timestamp);
        color = QColor("#4ade80");
      } else if (type == "worker_assigned") {
        displayText = QString("[%1] ➜ Assign to %2").arg(timestamp, worker);
        color = QColor("#fbbf24");
      } else if (type == "execution_started") {
        displayText = QString("[%1] ⚙ Executing on %2").arg(timestamp, worker);
        color = QColor("#fbbf24");
      } else if (type == "execution_completed") {
        displayText =
            QString("[%1] ✓ Completed in %2ms")
                .arg(timestamp,
                     QString::number(obj.value("duration_ms").toInt()));
        color = QColor("#4ade80");
      } else if (type == "security_violation") {
        displayText =
            QString("[%1] ✕ Security Violation: %2").arg(timestamp, detail);
        color = QColor("#f87171");
      } else {
        displayText = QString("[%1] • %2").arg(timestamp, type);
      }

      auto *stepLbl = new QLabel(displayText, container);
      stepLbl->setStyleSheet(
          QString("color:%1; font-size:11px; font-family:monospace;")
              .arg(color.name()));
      layout->addWidget(stepLbl);
    }

    return container;
  }

  void startPolling() {
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &ExecutionTraceView::pollTraces);
    timer->start(1000);
    pollTraces();
  }

  tsh::ApiClient *m_api = nullptr;
  QLabel *m_statusLbl = nullptr;
  QVBoxLayout *m_traceLayout = nullptr;
  QWidget *m_traceContainer = nullptr;
};
