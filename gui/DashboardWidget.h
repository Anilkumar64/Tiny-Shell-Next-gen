#pragma once
#include "ApiClient.h"
#include "MetricCard.h"
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QScrollArea>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Live dashboard showing metrics polled from /metrics and /health endpoints.
class DashboardWidget : public QWidget {
  Q_OBJECT
public:
  explicit DashboardWidget(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUi();
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &DashboardWidget::refresh);
    // BUG: dashboard refresh cadence did not match the 5s metrics contract.
    // FIX: poll metrics every 5 seconds.
    m_timer->start(5000);
    refresh();
  }

  void refresh() {
    m_api->getText("/metrics", [this](QString body, QString err) {
      if (!err.isEmpty()) {
        setStatus("⚠  Cannot reach server — " + err, false);
        return;
      }
      parsePrometheus(body);
      setStatus("● Connected  ·  Last update: " +
                    QDateTime::currentDateTime().toString("hh:mm:ss"),
                true);
    });
    m_api->getText("/healthz", [this](QString body, QString) {
      m_healthLbl->setText(body.trimmed().isEmpty() ? "OK"
                                                    : body.trimmed().left(80));
    });
  }

private:
  void buildUi() {
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *root = new QWidget;
    auto *vbox = new QVBoxLayout(root);
    vbox->setContentsMargins(28, 24, 28, 24);
    vbox->setSpacing(20);

    // ── Page header ──────────────────────────────────────────────────────
    auto *hdr = new QHBoxLayout;
    auto *titleLbl = new QLabel("Dashboard", root);
    titleLbl->setStyleSheet(
        "font-size: 22px; font-weight: 700; color: #e8eaf0;");
    m_statusLbl = new QLabel("Connecting…", root);
    m_statusLbl->setStyleSheet("font-size: 12px; color: #8891b0;");
    hdr->addWidget(titleLbl);
    hdr->addStretch();
    hdr->addWidget(m_statusLbl);
    vbox->addLayout(hdr);

    // ── Metric cards ─────────────────────────────────────────────────────
    auto *grid = new QGridLayout;
    grid->setSpacing(14);

    m_okCard = new MetricCard("Commands OK", "✓", MetricCard::Success);
    m_errCard = new MetricCard("Commands Failed", "✗", MetricCard::Danger);
    m_connCard = new MetricCard("Active Connections", "⇄", MetricCard::Default);
    m_queueCard = new MetricCard("Queue Depth", "⏳", MetricCard::Warning);
    m_durCard = new MetricCard("Last Latency", "⚡", MetricCard::Default);
    m_healthCard = new MetricCard("Health", "♥", MetricCard::Success);

    grid->addWidget(m_okCard, 0, 0);
    grid->addWidget(m_errCard, 0, 1);
    grid->addWidget(m_connCard, 0, 2);
    grid->addWidget(m_queueCard, 1, 0);
    grid->addWidget(m_durCard, 1, 1);
    grid->addWidget(m_healthCard, 1, 2);
    vbox->addLayout(grid);

    // ── Health raw text ───────────────────────────────────────────────────
    auto *healthSec = makeSectionLabel("Health Endpoint Response", root);
    vbox->addWidget(healthSec);
    m_healthLbl = new QLabel("—", root);
    m_healthLbl->setWordWrap(true);
    m_healthLbl->setStyleSheet(
        "background:#1c1f2b; border:1px solid #2a2d3e; border-radius:8px;"
        "padding:12px; color:#4ade80; font-family:'JetBrains Mono','Fira "
        "Code',monospace;");
    vbox->addWidget(m_healthLbl);

    // ── Prometheus raw ────────────────────────────────────────────────────
    auto *metSec = makeSectionLabel("Prometheus Metrics (raw)", root);
    vbox->addWidget(metSec);
    m_rawLbl = new QLabel("—", root);
    m_rawLbl->setWordWrap(true);
    m_rawLbl->setStyleSheet(
        "background:#1c1f2b; border:1px solid #2a2d3e; border-radius:8px;"
        "padding:12px; color:#8891b0; font-family:'JetBrains Mono','Fira "
        "Code',monospace;"
        "font-size:12px; line-height:1.6;");
    vbox->addWidget(m_rawLbl);

    vbox->addStretch();
    scroll->setWidget(root);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(scroll);
  }

  static QLabel *makeSectionLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text.toUpper(), parent);
    l->setStyleSheet("font-size: 10px; font-weight: 600; letter-spacing: 1px;"
                     "color: #4a5068; margin-top: 4px;");
    return l;
  }

  void setStatus(const QString &msg, bool ok) {
    m_statusLbl->setText(msg);
    m_statusLbl->setStyleSheet(
        QString("font-size:12px; color:%1;").arg(ok ? "#4ade80" : "#f87171"));
  }

  void parsePrometheus(const QString &body) {
    m_rawLbl->setText(body.trimmed());

    auto extract = [&](const QString &key) -> QString {
      for (const QString &line : body.split('\n')) {
        if (line.startsWith(key)) {
          const QString val = line.mid(key.length()).trimmed();
          return val;
        }
      }
      return "—";
    };

    const QString ok = extract("tsh_commands_total{status=\"ok\"}");
    const QString err = extract("tsh_commands_total{status=\"error\"}");
    const QString dur =
        extract("tsh_command_duration_seconds{quantile=\"0.99\"}");
    const QString conn = extract("tsh_active_connections");
    const QString queue = extract("tsh_scheduler_queue_depth");

    m_okCard->setValue(ok);
    m_okCard->setSub("total successful");
    m_okCard->pulse();
    m_errCard->setValue(err);
    m_errCard->setSub("total failed");
    m_errCard->pulse();
    m_connCard->setValue(conn);
    m_connCard->setSub("open sockets");
    m_connCard->pulse();
    m_queueCard->setValue(queue);
    m_queueCard->setSub("pending jobs");
    m_queueCard->pulse();

    const double ms = dur.toDouble() * 1000.0;
    m_durCard->setValue(QString::number(ms, 'f', 2) + " ms");
    m_durCard->setSub("p99 latency");
    m_durCard->pulse();

    // Derive overall health
    const bool healthy = (err.toInt() == 0 && conn.toInt() >= 0);
    m_healthCard->setValue(healthy ? "Healthy" : "Degraded");
    m_healthCard->setSub(healthy ? "all systems nominal" : "check errors");
    m_healthCard->pulse();
  }

  tsh::ApiClient *m_api;
  QTimer *m_timer = nullptr;
  QLabel *m_statusLbl = nullptr;
  QLabel *m_healthLbl = nullptr;
  QLabel *m_rawLbl = nullptr;
  MetricCard *m_okCard = nullptr;
  MetricCard *m_errCard = nullptr;
  MetricCard *m_connCard = nullptr;
  MetricCard *m_queueCard = nullptr;
  MetricCard *m_durCard = nullptr;
  MetricCard *m_healthCard = nullptr;
};
