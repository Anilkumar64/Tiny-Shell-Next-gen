#pragma once
#include "ApiClient.h"
#include "TshStyle.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

// Job Scheduler UI — schedule jobs, view queue, pick routing strategy.
class JobWidget : public QWidget {
  Q_OBJECT

  enum class RoutingMode { RoundRobin, Speculative, BFT };

  struct Job {
    QString id;
    QString astName;
    QString status; // Pending | Running | Done | Failed
    QString node;
    QString submitted;
    RoutingMode mode;
  };

public:
  explicit JobWidget(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUi();
    seedJobs();
    m_ticker = new QTimer(this);
    connect(m_ticker, &QTimer::timeout, this, &JobWidget::tickJobs);
    m_ticker->start(1500);
  }

private:
  void buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(16);

    auto *titleLbl = new QLabel("Job Scheduler", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    root->addWidget(titleLbl);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(1);
    root->addWidget(splitter, 1);

    // ── Left: Submit panel ───────────────────────────────────────────────
    auto *submitPanel = new QFrame(splitter);
    submitPanel->setFixedWidth(300);
    submitPanel->setStyleSheet("QFrame { background:#1c1f2b; border:1px solid "
                               "#2a2d3e; border-radius:10px; }");
    auto *sv = new QVBoxLayout(submitPanel);
    sv->setContentsMargins(16, 16, 16, 16);
    sv->setSpacing(12);

    auto *fmHdr = new QLabel("SUBMIT JOB", submitPanel);
    fmHdr->setStyleSheet(
        "font-size:10px; font-weight:600; letter-spacing:1px; color:#4a5068;");
    sv->addWidget(fmHdr);

    auto *astLbl = new QLabel("AST / Command Name:", submitPanel);
    astLbl->setStyleSheet("font-size:12px; color:#8891b0;");
    m_astEdit = new QLineEdit(submitPanel);
    m_astEdit->setPlaceholderText("e.g. grep_pipeline");
    sv->addWidget(astLbl);
    sv->addWidget(m_astEdit);

    // Routing mode
    auto *routeLbl = new QLabel("Routing Strategy:", submitPanel);
    routeLbl->setStyleSheet("font-size:12px; color:#8891b0; margin-top:6px;");
    sv->addWidget(routeLbl);

    m_rrRadio = new QRadioButton("Round-Robin  (default)", submitPanel);
    m_sfRadio =
        new QRadioButton("Speculative Fan-Out  (needs ≥2 nodes)", submitPanel);
    m_bftRadio = new QRadioButton("Byzantine Fault Tolerant  (needs ≥3 nodes)",
                                  submitPanel);
    m_rrRadio->setChecked(true);

    auto *radioGroup = new QButtonGroup(this);
    radioGroup->addButton(m_rrRadio);
    radioGroup->addButton(m_sfRadio);
    radioGroup->addButton(m_bftRadio);

    for (auto *rb : {m_rrRadio, m_sfRadio, m_bftRadio}) {
      rb->setStyleSheet("QRadioButton { color:#e8eaf0; font-size:12px; }"
                        "QRadioButton::indicator:checked { background:#00b4d8; "
                        "border-color:#00b4d8; }");
      sv->addWidget(rb);
    }

    // Description boxes
    m_stratDescLbl =
        new QLabel(routingDesc(RoutingMode::RoundRobin), submitPanel);
    m_stratDescLbl->setWordWrap(true);
    m_stratDescLbl->setStyleSheet(
        "background:#13151c; border:1px solid #2a2d3e; border-radius:6px;"
        "padding:8px; font-size:11px; color:#8891b0; margin-top:4px;");
    sv->addWidget(m_stratDescLbl);

    auto updateDesc = [this] {
      RoutingMode m = m_rrRadio->isChecked()   ? RoutingMode::RoundRobin
                      : m_sfRadio->isChecked() ? RoutingMode::Speculative
                                               : RoutingMode::BFT;
      m_stratDescLbl->setText(routingDesc(m));
    };
    connect(m_rrRadio, &QRadioButton::toggled, this, updateDesc);
    connect(m_sfRadio, &QRadioButton::toggled, this, updateDesc);
    connect(m_bftRadio, &QRadioButton::toggled, this, updateDesc);

    sv->addStretch();

    auto *submitBtn = new QPushButton("▶  Schedule Job", submitPanel);
    submitBtn->setProperty("role", "primary");
    submitBtn->setFixedHeight(40);
    connect(submitBtn, &QPushButton::clicked, this, &JobWidget::scheduleJob);
    sv->addWidget(submitBtn);

    splitter->addWidget(submitPanel);

    // ── Right: queue table ───────────────────────────────────────────────
    auto *rightPanel = new QFrame(splitter);
    rightPanel->setStyleSheet("QFrame { background:#1c1f2b; border:1px solid "
                              "#2a2d3e; border-radius:10px; }");
    auto *rv = new QVBoxLayout(rightPanel);
    rv->setContentsMargins(16, 16, 16, 16);
    rv->setSpacing(10);

    auto *queueHdrRow = new QHBoxLayout;
    auto *queueHdr = new QLabel("JOB QUEUE", rightPanel);
    queueHdr->setStyleSheet(
        "font-size:10px; font-weight:600; letter-spacing:1px; color:#4a5068;");
    m_qDepthLbl = new QLabel("0 active", rightPanel);
    m_qDepthLbl->setStyleSheet("font-size:12px; color:#fbbf24;");
    auto *clearDoneBtn = new QPushButton("Clear Done", rightPanel);
    clearDoneBtn->setFixedHeight(28);
    connect(clearDoneBtn, &QPushButton::clicked, this,
            &JobWidget::clearDoneJobs);
    queueHdrRow->addWidget(queueHdr);
    queueHdrRow->addWidget(m_qDepthLbl);
    queueHdrRow->addStretch();
    queueHdrRow->addWidget(clearDoneBtn);
    rv->addLayout(queueHdrRow);

    m_table = new QTableWidget(0, 6, rightPanel);
    m_table->setHorizontalHeaderLabels(
        {"Job ID", "AST / Command", "Strategy", "Node", "Status", "Submitted"});
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setStretchLastSection(false);
    rv->addWidget(m_table, 1);

    splitter->addWidget(rightPanel);
    splitter->setSizes({300, 700});
  }

  void seedJobs() {
    addJob({"j-0001", "grep_pipeline", "Done", "worker-01", jobTime(-120),
            RoutingMode::RoundRobin});
    addJob({"j-0002", "top_ten_procs", "Done", "worker-02", jobTime(-90),
            RoutingMode::Speculative});
    addJob({"j-0003", "audit_query", "Running", "worker-01", jobTime(-30),
            RoutingMode::RoundRobin});
    addJob({"j-0004", "critical_cleanup", "Running", "worker-03", jobTime(-15),
            RoutingMode::BFT});
    addJob({"j-0005", "mem_snapshot", "Pending", "", jobTime(0),
            RoutingMode::RoundRobin});
  }

  static QString jobTime(int offsetSec) {
    return QDateTime::currentDateTime().addSecs(offsetSec).toString("hh:mm:ss");
  }

  void addJob(const Job &j) {
    m_jobs.append(j);
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setRowHeight(row, 42);
    refreshRow(row);
    updateDepthLabel();
  }

  void refreshRow(int row) {
    if (row >= m_jobs.size())
      return;
    const auto &j = m_jobs[row];

    auto set = [&](int c, const QString &v, const QString &col = "#e8eaf0") {
      if (!m_table->item(row, c))
        m_table->setItem(row, c, new QTableWidgetItem);
      m_table->item(row, c)->setText(v);
      m_table->item(row, c)->setForeground(QColor(col));
    };
    set(0, j.id, "#4a5068");
    set(1, j.astName);
    set(2, routingLabel(j.mode), "#8891b0");
    set(3, j.node.isEmpty() ? "—" : j.node, "#8891b0");
    set(5, j.submitted, "#4a5068");

    // Status badge
    QString statusColor = "#8891b0";
    if (j.status == "Running")
      statusColor = "#fbbf24";
    else if (j.status == "Done")
      statusColor = "#4ade80";
    else if (j.status == "Failed")
      statusColor = "#f87171";
    else if (j.status == "Pending")
      statusColor = "#00b4d8";
    if (!m_table->item(row, 4))
      m_table->setItem(row, 4, new QTableWidgetItem);
    m_table->item(row, 4)->setText("● " + j.status);
    m_table->item(row, 4)->setForeground(QColor(statusColor));
  }

  static QString routingLabel(RoutingMode m) {
    switch (m) {
    case RoutingMode::Speculative:
      return "Speculative";
    case RoutingMode::BFT:
      return "BFT";
    default:
      return "Round-Robin";
    }
  }

  static QString routingDesc(RoutingMode m) {
    switch (m) {
    case RoutingMode::Speculative:
      return "Sends the AST to 2 nodes simultaneously. Returns whichever "
             "result arrives first. "
             "Requires ≥2 registered nodes.";
    case RoutingMode::BFT:
      return "Byzantine Fault Tolerant execution on 3 nodes. Majority result "
             "wins. "
             "Requires ≥3 registered nodes. Higher latency, maximum integrity.";
    default:
      return "Distributes jobs across nodes in round-robin order using the "
             "sharded scheduler. "
             "Low overhead, works with any cluster size.";
    }
  }

  void scheduleJob() {
    const QString name = m_astEdit->text().trimmed();
    if (name.isEmpty()) {
      m_astEdit->setFocus();
      return;
    }

    RoutingMode mode = m_rrRadio->isChecked()   ? RoutingMode::RoundRobin
                       : m_sfRadio->isChecked() ? RoutingMode::Speculative
                                                : RoutingMode::BFT;
    Job j;
    j.id = "j-" + QString::number(m_jobs.size() + 1).rightJustified(4, '0');
    j.astName = name;
    j.status = "Pending";
    j.node = "";
    j.submitted = QDateTime::currentDateTime().toString("hh:mm:ss");
    j.mode = mode;
    addJob(j);
    m_astEdit->clear();
  }

  void tickJobs() {
    static const QStringList nodes{"worker-01", "worker-02", "worker-03",
                                   "localhost"};
    bool changed = false;
    for (auto &j : m_jobs) {
      if (j.status == "Pending") {
        j.status = "Running";
        j.node = nodes[QRandomGenerator::global()->bounded(nodes.size())];
        changed = true;
      } else if (j.status == "Running") {
        // 40% chance to finish each tick
        if (QRandomGenerator::global()->bounded(100) < 40) {
          j.status =
              QRandomGenerator::global()->bounded(10) < 1 ? "Failed" : "Done";
          changed = true;
        }
      }
    }
    if (changed) {
      for (int r = 0; r < m_jobs.size(); ++r)
        refreshRow(r);
      updateDepthLabel();
    }
  }

  void clearDoneJobs() {
    for (int r = m_table->rowCount() - 1; r >= 0; --r) {
      if (r < m_jobs.size() &&
          (m_jobs[r].status == "Done" || m_jobs[r].status == "Failed")) {
        m_table->removeRow(r);
        m_jobs.removeAt(r);
      }
    }
    updateDepthLabel();
  }

  void updateDepthLabel() {
    const int pending =
        std::count_if(m_jobs.begin(), m_jobs.end(), [](const Job &j) {
          return j.status == "Pending" || j.status == "Running";
        });
    m_qDepthLbl->setText(QString::number(pending) + " active");
  }

  QTableWidget *m_table = nullptr;
  QLineEdit *m_astEdit = nullptr;
  QRadioButton *m_rrRadio = nullptr;
  QRadioButton *m_sfRadio = nullptr;
  QRadioButton *m_bftRadio = nullptr;
  QLabel *m_stratDescLbl = nullptr;
  QLabel *m_qDepthLbl = nullptr;
  QTimer *m_ticker = nullptr;
  tsh::ApiClient *m_api = nullptr;
  QList<Job> m_jobs;
};
