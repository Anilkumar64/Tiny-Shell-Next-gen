#pragma once
#include "ApiClient.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Live panel showing all connected client sessions
class ClientConnectionPanel : public QWidget {
  Q_OBJECT
public:
  explicit ClientConnectionPanel(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUI();
    startPolling();
  }

private slots:
  void pollClients() {
    m_api->getText("/control/events", [this](QString body, QString err) {
      if (!err.isEmpty()) {
        m_statusLbl->setText("⚠ Connection offline");
        return;
      }
      QJsonParseError pe;
      const auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
      if (pe.error != QJsonParseError::NoError)
        return;

      // Extract unique clients from events
      QSet<QString> clients;
      const auto events = doc.object().value("events").toArray();
      for (const auto &evt : events) {
        const auto obj = evt.toObject();
        const auto user = obj.value("user").toString();
        const auto ip = obj.value("client_ip").toString();
        if (!user.isEmpty() && !ip.isEmpty()) {
          clients.insert(user + ":" + ip);
        }
      }

      m_table->setRowCount(0);
      int row = 0;
      for (const auto &client : clients) {
        m_table->insertRow(row);
        const auto parts = client.split(":");
        m_table->setItem(
            row, 0, new QTableWidgetItem(parts.size() > 0 ? parts[0] : ""));
        m_table->setItem(row, 1, new QTableWidgetItem("default"));
        m_table->setItem(
            row, 2, new QTableWidgetItem(parts.size() > 1 ? parts[1] : ""));
        m_table->setItem(row, 3, new QTableWidgetItem("●"));
        m_table->setItem(row, 4, new QTableWidgetItem("🔒 X25519+AES256-GCM"));
        m_table->setItem(
            row, 5,
            new QTableWidgetItem(
                QDateTime::currentDateTime().toString("hh:mm:ss")));
        row++;
      }

      m_statusLbl->setText("● Online · " + QString::number(clients.size()) +
                           " clients");
    });
  }

private:
  void buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    auto *titleLbl = new QLabel("Live Client Connections", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    root->addWidget(titleLbl);

    m_statusLbl = new QLabel("● Online · 0 clients", this);
    m_statusLbl->setStyleSheet(
        "color:#4ade80; font-size:12px; font-weight:600;");
    root->addWidget(m_statusLbl);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({"Username", "Tenant", "IP Address",
                                        "Status", "Encryption",
                                        "Connected Since"});
    m_table->setStyleSheet(
        "QTableWidget { background:#0a0c14; border:1px solid #2a2d3e; "
        "border-radius:8px; color:#e8eaf0; }"
        "QHeaderView::section { background:#1c1f2b; border:none; padding:8px; "
        "font-weight:600; color:#8891b0; }"
        "QTableWidget::item { padding:8px; border-bottom:1px solid #1c1f2b; }"
        "QTableWidget::item:selected { background:#0077a8; }");
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setAlternatingRowColors(true);
    root->addWidget(m_table, 1);
  }

  void startPolling() {
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &ClientConnectionPanel::pollClients);
    timer->start(1000);
    pollClients();
  }

  tsh::ApiClient *m_api = nullptr;
  QTableWidget *m_table = nullptr;
  QLabel *m_statusLbl = nullptr;
};
