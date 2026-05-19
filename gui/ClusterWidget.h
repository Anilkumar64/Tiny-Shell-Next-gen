#pragma once
#include "ApiClient.h"
#include "TshStyle.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

// Manages cluster worker nodes — add, remove, view health scores.
// Data is kept in-process; a real backend endpoint would replace the in-memory
// map.
class ClusterWidget : public QWidget {
  Q_OBJECT

  struct NodeRow {
    QString host;
    int port;
    double healthScore;
    bool healthy;
    int successJobs;
    int failedJobs;
    double responseMs;
  };

public:
  explicit ClusterWidget(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUi();
    // Seed with example nodes to show the UI populated
    addNodeRow({"localhost", 4445, 95.0, true, 1024, 3, 12.4});
    addNodeRow({"worker-01", 4445, 87.5, true, 843, 12, 28.1});
    addNodeRow({"worker-02", 4445, 0.0, false, 200, 50, 180.0});
    addNodeRow({"worker-03", 4445, 72.0, true, 560, 8, 45.2});
    refreshStats();
  }

private:
  void buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(16);

    // ── Header ──────────────────────────────────────────────────────────
    auto *hdrRow = new QHBoxLayout;
    auto *titleLbl = new QLabel("Cluster", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");

    auto *addBtn = new QPushButton("+ Add Node", this);
    addBtn->setProperty("role", "primary");
    addBtn->setFixedHeight(34);
    connect(addBtn, &QPushButton::clicked, this, &ClusterWidget::showAddDialog);

    auto *refreshBtn = new QPushButton("↻ Refresh", this);
    refreshBtn->setFixedHeight(34);
    connect(refreshBtn, &QPushButton::clicked, this,
            &ClusterWidget::refreshStats);

    hdrRow->addWidget(titleLbl);
    hdrRow->addStretch();
    hdrRow->addWidget(refreshBtn);
    hdrRow->addWidget(addBtn);
    root->addLayout(hdrRow);

    // ── Summary cards ────────────────────────────────────────────────────
    auto *cardRow = new QHBoxLayout;
    cardRow->setSpacing(14);
    m_totalLbl = makeStatCard("Total Nodes", "0", cardRow);
    m_healthyLbl = makeStatCard("Healthy", "0", cardRow, "#4ade80");
    m_deadLbl = makeStatCard("Unreachable", "0", cardRow, "#f87171");
    m_avgScoreLbl = makeStatCard("Avg Health Score", "—", cardRow, "#fbbf24");
    root->addLayout(cardRow);

    // ── Node table ───────────────────────────────────────────────────────
    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels({"Host", "Port", "Status",
                                        "Health Score", "OK Jobs",
                                        "Failed Jobs", "Latency ms"});
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(3, 160);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setIconSize({10, 10});
    root->addWidget(m_table, 1);

    // ── Bottom toolbar ───────────────────────────────────────────────────
    auto *bottomRow = new QHBoxLayout;
    m_selInfoLbl = new QLabel("Select a row to manage the node.", this);
    m_selInfoLbl->setStyleSheet("color:#8891b0; font-size:12px;");

    m_removeBtn = new QPushButton("✕ Remove Node", this);
    m_removeBtn->setProperty("role", "danger");
    m_removeBtn->setFixedHeight(34);
    m_removeBtn->setEnabled(false);
    connect(m_removeBtn, &QPushButton::clicked, this,
            &ClusterWidget::removeSelected);

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
      m_removeBtn->setEnabled(m_table->currentRow() >= 0);
      m_selInfoLbl->setText(
          m_table->currentRow() >= 0
              ? "Selected: " + m_table->item(m_table->currentRow(), 0)->text()
              : "Select a row to manage the node.");
    });

    bottomRow->addWidget(m_selInfoLbl);
    bottomRow->addStretch();
    bottomRow->addWidget(m_removeBtn);
    root->addLayout(bottomRow);
  }

  static QLabel *makeStatCard(const QString &label, const QString &val,
                              QHBoxLayout *row,
                              const QString &color = "#e8eaf0") {
    auto *frame = new QFrame;
    frame->setStyleSheet(
        "QFrame { background:#1c1f2b; border:1px solid #2a2d3e;"
        " border-radius:8px; padding:12px; }");
    frame->setFixedHeight(72);
    auto *vl = new QVBoxLayout(frame);
    vl->setContentsMargins(14, 8, 14, 8);
    vl->setSpacing(2);

    auto *valLbl = new QLabel(val, frame);
    valLbl->setStyleSheet(
        QString("font-size:22px; font-weight:700; color:%1;").arg(color));
    auto *lblLbl = new QLabel(label, frame);
    lblLbl->setStyleSheet("font-size:10px; color:#8891b0; font-weight:600; "
                          "letter-spacing:0.5px;");
    vl->addWidget(valLbl);
    vl->addWidget(lblLbl);

    row->addWidget(frame, 1);
    return valLbl; // caller stores to update later
  }

  void addNodeRow(const NodeRow &n) {
    m_nodes.append(n);
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setRowHeight(row, 44);

    auto setItem = [&](int col, const QString &text,
                       const QString &color = "#e8eaf0") {
      auto *item = new QTableWidgetItem(text);
      item->setForeground(QColor(color));
      m_table->setItem(row, col, item);
    };

    setItem(0, n.host);
    setItem(1, QString::number(n.port));

    // Status badge
    auto *statusItem =
        new QTableWidgetItem(n.healthy ? "● Healthy" : "● Unreachable");
    statusItem->setForeground(QColor(n.healthy ? "#4ade80" : "#f87171"));
    statusItem->setFont([] {
      QFont f;
      f.setBold(true);
      return f;
    }());
    m_table->setItem(row, 2, statusItem);

    // Health score with embedded progress bar
    auto *pb = new QProgressBar;
    pb->setRange(0, 100);
    pb->setValue(qRound(n.healthScore));
    pb->setTextVisible(true);
    pb->setFormat(QString::number(n.healthScore, 'f', 1) + "%");
    pb->setStyleSheet(
        QString(
            "QProgressBar { background:#1a1d2e; border:none; border-radius:4px;"
            " color:#e8eaf0; font-size:11px; font-weight:600; }"
            "QProgressBar::chunk { background:%1; border-radius:3px; }")
            .arg(n.healthScore > 70   ? "#4ade80"
                 : n.healthScore > 40 ? "#fbbf24"
                                      : "#f87171"));
    m_table->setCellWidget(row, 3, pb);

    setItem(4, QString::number(n.successJobs), "#4ade80");
    setItem(5, QString::number(n.failedJobs),
            n.failedJobs > 0 ? "#f87171" : "#8891b0");
    setItem(6, QString::number(n.responseMs, 'f', 1) + " ms",
            n.responseMs > 100 ? "#fbbf24" : "#8891b0");
  }

  void refreshStats() {
    const int total = m_nodes.size();
    const int healthy =
        std::count_if(m_nodes.begin(), m_nodes.end(),
                      [](const NodeRow &n) { return n.healthy; });
    const int dead = total - healthy;
    double avgScore = 0;
    for (const auto &n : m_nodes)
      avgScore += n.healthScore;
    if (total)
      avgScore /= total;

    m_totalLbl->setText(QString::number(total));
    m_healthyLbl->setText(QString::number(healthy));
    m_deadLbl->setText(QString::number(dead));
    m_avgScoreLbl->setText(QString::number(avgScore, 'f', 1) + "%");
  }

  void showAddDialog() {
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle("Add Worker Node");
    dlg->setFixedWidth(340);
    dlg->setStyleSheet("background:#1c1f2b; color:#e8eaf0;");

    auto *form = new QFormLayout(dlg);
    form->setContentsMargins(20, 20, 20, 16);
    form->setSpacing(14);
    form->setLabelAlignment(Qt::AlignRight);

    auto *hostEdit = new QLineEdit("localhost", dlg);
    auto *portSpin = new QSpinBox(dlg);
    portSpin->setRange(1, 65535);
    portSpin->setValue(4445);

    form->addRow("Host:", hostEdit);
    form->addRow("Port:", portSpin);

    auto *btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    btns->button(QDialogButtonBox::Ok)->setProperty("role", "primary");
    connect(btns, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    form->addRow(btns);

    if (dlg->exec() == QDialog::Accepted) {
      NodeRow nr{
          hostEdit->text().trimmed(), portSpin->value(), 50.0, true, 0, 0, 0.0};
      addNodeRow(nr);
      refreshStats();
    }
    dlg->deleteLater();
  }

  void removeSelected() {
    const int row = m_table->currentRow();
    if (row < 0)
      return;
    const QString host = m_table->item(row, 0)->text();
    if (QMessageBox::question(
            this, "Remove Node",
            QString("Remove node <b>%1</b> from the cluster?").arg(host),
            QMessageBox::Yes | QMessageBox::Cancel) == QMessageBox::Yes) {
      m_table->removeRow(row);
      m_nodes.removeAt(row);
      refreshStats();
      m_removeBtn->setEnabled(false);
    }
  }

  QTableWidget *m_table = nullptr;
  QPushButton *m_removeBtn = nullptr;
  QLabel *m_selInfoLbl = nullptr;
  QLabel *m_totalLbl = nullptr;
  QLabel *m_healthyLbl = nullptr;
  QLabel *m_deadLbl = nullptr;
  QLabel *m_avgScoreLbl = nullptr;
  QList<NodeRow> m_nodes;
  tsh::ApiClient *m_api = nullptr;
};
