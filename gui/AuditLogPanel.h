#pragma once
#include "ApiClient.h"
#include <algorithm>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Audit log viewer with filtering, searching, and export
class AuditLogPanel : public QWidget {
  Q_OBJECT
public:
  explicit AuditLogPanel(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUI();
    startPolling();
  }

private slots:
  void pollAuditLogs() {
    m_api->getText(QString("/control/events?since_sequence=%1").arg(m_lastSeq),
                   [this](QString body, QString err) {
      if (!err.isEmpty()) {
        m_statusLbl->setText("⚠ Stream offline");
        return;
      }
      QJsonParseError pe;
      const auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
      if (pe.error != QJsonParseError::NoError)
        return;

      const auto events = doc.object().value("events").toArray();
      for (const auto &value : events) {
        const auto obj = value.toObject();
        const auto seq = static_cast<quint64>(obj.value("sequence").toDouble());
        if (seq <= m_lastSeq)
          continue;
        m_lastSeq = seq;
        m_allEvents.append(obj);
      }
      while (m_allEvents.size() > 1000)
        m_allEvents.removeAt(0);
      updateAuditTable();
    });
  }

  void onFilterChanged() { updateAuditTable(); }

  void onExport() {
    const auto path = QFileDialog::getSaveFileName(
        this, "Export audit log",
        "audit-" +
            QDateTime::currentDateTime().toString("yyyy-MM-dd_hh-mm-ss") +
            ".csv",
        "CSV (*.csv)");
    if (path.isEmpty())
      return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
      return;

    file.write("timestamp,user,tenant,command,worker,result,duration_ms\n");
    for (int row = 0; row < m_table->rowCount(); ++row) {
      auto *timeItem = m_table->item(row, 0);
      auto *userItem = m_table->item(row, 1);
      auto *tenantItem = m_table->item(row, 2);
      auto *cmdItem = m_table->item(row, 3);
      auto *workerItem = m_table->item(row, 4);
      auto *resultItem = m_table->item(row, 5);
      auto *durationItem = m_table->item(row, 6);

      if (timeItem)
        file.write("\"" + timeItem->text().toUtf8() + "\",");
      if (userItem)
        file.write("\"" + userItem->text().toUtf8() + "\",");
      if (tenantItem)
        file.write("\"" + tenantItem->text().toUtf8() + "\",");
      if (cmdItem)
        file.write("\"" + cmdItem->text().toUtf8() + "\",");
      if (workerItem)
        file.write("\"" + workerItem->text().toUtf8() + "\",");
      if (resultItem)
        file.write("\"" + resultItem->text().toUtf8() + "\",");
      if (durationItem)
        file.write(durationItem->text().toUtf8());
      file.write("\n");
    }
    file.close();
    m_statusLbl->setText("✓ Exported " + QString::number(m_table->rowCount()) +
                         " audit records");
  }

private:
  void buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    auto *titleLbl = new QLabel("Audit Log Viewer", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    root->addWidget(titleLbl);

    m_statusLbl = new QLabel("Loading audit log…", this);
    m_statusLbl->setStyleSheet(
        "color:#4ade80; font-size:12px; font-weight:600;");
    root->addWidget(m_statusLbl);

    // Filter row
    auto *filterRow = new QHBoxLayout;
    filterRow->setSpacing(10);

    auto *userLabel = new QLabel("User:", this);
    m_userFilter = new QLineEdit(this);
    m_userFilter->setPlaceholderText("Filter by user…");
    m_userFilter->setMaximumWidth(150);
    m_userFilter->setStyleSheet(
        "QLineEdit { background:#1c1f2b; border:1px solid #2a2d3e; "
        "border-radius:6px; color:#e8eaf0; padding:6px; }");
    connect(m_userFilter, &QLineEdit::textChanged, this,
            &AuditLogPanel::onFilterChanged);

    auto *resultLabel = new QLabel("Result:", this);
    m_resultFilter = new QComboBox(this);
    m_resultFilter->addItems({"All", "Success", "Failed", "Blocked"});
    m_resultFilter->setMaximumWidth(120);
    m_resultFilter->setStyleSheet(
        "QComboBox { background:#1c1f2b; border:1px solid #2a2d3e; "
        "border-radius:6px; color:#e8eaf0; padding:6px; }");
    connect(m_resultFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AuditLogPanel::onFilterChanged);

    auto *exportBtn = new QPushButton("Export", this);
    exportBtn->setMaximumWidth(100);
    exportBtn->setStyleSheet(
        "QPushButton { background:#0077a8; border:none; border-radius:6px; "
        "color:#e8eaf0; padding:6px 12px; font-weight:600; }"
        "QPushButton:hover { background:#00a0d2; }");
    connect(exportBtn, &QPushButton::clicked, this, &AuditLogPanel::onExport);

    filterRow->addWidget(userLabel);
    filterRow->addWidget(m_userFilter);
    filterRow->addWidget(resultLabel);
    filterRow->addWidget(m_resultFilter);
    filterRow->addStretch();
    filterRow->addWidget(exportBtn);
    root->addLayout(filterRow);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({"Time", "User", "Tenant", "Command",
                                        "Worker", "Result", "Duration (ms)"});
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

  void updateAuditTable() {
    m_table->setRowCount(0);
    const auto userFilter = m_userFilter->text().toLower();
    const auto resultFilter = m_resultFilter->currentText();

    for (int i = m_allEvents.size() - 1;
         i >= std::max(0, (int)m_allEvents.size() - 100); --i) {
      const auto evt = m_allEvents[i].toObject();
      const auto type = evt.value("type").toString();

      if (type != "execution_completed" && type != "execution_failed" &&
          type != "security_violation")
        continue;

      const auto user = evt.value("user").toString();
      const auto result = (type == "execution_completed")  ? "Success"
                          : (type == "security_violation") ? "Blocked"
                                                           : "Failed";

      if (!userFilter.isEmpty() && !user.toLower().contains(userFilter))
        continue;
      if (resultFilter != "All" && result != resultFilter)
        continue;

      const int row = m_table->rowCount();
      m_table->insertRow(row);

      m_table->setItem(
          row, 0,
          new QTableWidgetItem(evt.value("timestamp").toString().right(8)));
      m_table->setItem(row, 1, new QTableWidgetItem(user));
      m_table->setItem(row, 2,
                       new QTableWidgetItem(evt.value("tenant").toString()));
      m_table->setItem(row, 3,
                       new QTableWidgetItem(evt.value("command").toString()));
      m_table->setItem(row, 4,
                       new QTableWidgetItem(evt.value("worker").toString()));
      m_table->setItem(row, 5, new QTableWidgetItem(result));
      m_table->setItem(row, 6,
                       new QTableWidgetItem(QString::number(
                           evt.value("duration_ms").toVariant().toLongLong())));
    }

    m_statusLbl->setText("● Live · " + QString::number(m_table->rowCount()) +
                         " audit records");
  }

  void startPolling() {
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &AuditLogPanel::pollAuditLogs);
    timer->start(1000);
    pollAuditLogs();
  }

  tsh::ApiClient *m_api = nullptr;
  QTableWidget *m_table = nullptr;
  QLabel *m_statusLbl = nullptr;
  QLineEdit *m_userFilter = nullptr;
  QComboBox *m_resultFilter = nullptr;
  quint64 m_lastSeq = 0;
  QJsonArray m_allEvents;
};
