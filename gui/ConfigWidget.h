#pragma once
#include "ApiClient.h"
#include "TshStyle.h"
#include <QCheckBox>
#include <QClipboard>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// Configuration panel — edits connection settings for the ApiClient and
// shows the runtime tsh_config.h values for reference.
class ConfigWidget : public QWidget {
  Q_OBJECT
public:
  explicit ConfigWidget(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUi();
    loadFromApi();
  }

signals:
  void configApplied();

private:
  void buildUi() {
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *root = new QWidget;
    auto *vbox = new QVBoxLayout(root);
    vbox->setContentsMargins(28, 24, 28, 24);
    vbox->setSpacing(20);

    // ── Header ──────────────────────────────────────────────────────────
    auto *titleLbl = new QLabel("Configuration", root);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
    vbox->addWidget(titleLbl);

    // ── Connection group ─────────────────────────────────────────────────
    auto *connGroup = makeGroup("API Connection", root);
    auto *connForm = new QFormLayout(connGroup);
    connForm->setSpacing(12);
    connForm->setLabelAlignment(Qt::AlignRight);

    m_apiUrlEdit = new QLineEdit(m_api->baseUrl(), root);
    m_apiUrlEdit->setPlaceholderText("https://127.0.0.1:8080");
    m_apiUrlEdit->setToolTip(
        "Base URL for the TinyShell HTTP API (TSH_API_PORT)");

    m_tokenEdit = new QLineEdit(m_api->token(), root);
    m_tokenEdit->setPlaceholderText("Bearer token (TSH_API_TOKEN)");
    m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_tokenEdit->setToolTip("Matches the TSH_API_TOKEN environment variable");

    auto *showTokenCb = new QCheckBox("Show token", root);
    connect(showTokenCb, &QCheckBox::toggled, this, [this](bool show) {
      m_tokenEdit->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });

    connForm->addRow("API URL:", m_apiUrlEdit);
    connForm->addRow("API Token:", m_tokenEdit);
    connForm->addRow("", showTokenCb);
    vbox->addWidget(connGroup);

    // ── Server info group (read-only reference) ──────────────────────────
    auto *srvGroup =
        makeGroup("Server Reference (tsh_config.h defaults)", root);
    auto *srvForm = new QFormLayout(srvGroup);
    srvForm->setSpacing(10);
    srvForm->setLabelAlignment(Qt::AlignRight);

    auto addRef = [&](const QString &label, const QString &val,
                      const QString &envVar) {
      auto *row = new QHBoxLayout;
      auto *valLbl = new QLabel(val, root);
      valLbl->setStyleSheet(
          "font-family:'JetBrains Mono','Fira Code',monospace; color:#00b4d8;");
      auto *envLbl = new QLabel("env: " + envVar, root);
      envLbl->setStyleSheet("font-size:11px; color:#4a5068;");
      row->addWidget(valLbl);
      row->addWidget(envLbl);
      row->addStretch();
      srvForm->addRow(label + ":", row);
    };
    addRef("TCP port", "4444", "TSH_PORT");
    addRef("HTTP API port", "8080", "TSH_API_PORT");
    addRef("Bind address", "127.0.0.1", "TSH_API_BIND_ADDR");
    addRef("ZK secret", "≥32 bytes required", "TSH_ZK_SECRET");
    vbox->addWidget(srvGroup);

    // ── Connection test ──────────────────────────────────────────────────
    auto *testGroup = makeGroup("Connection Test", root);
    auto *testVbox = new QVBoxLayout(testGroup);

    auto *testRow = new QHBoxLayout;
    auto *testBtn = new QPushButton("Test Connection", root);
    testBtn->setProperty("role", "primary");
    testBtn->setFixedHeight(36);
    connect(testBtn, &QPushButton::clicked, this,
            &ConfigWidget::testConnection);

    m_testResultLbl = new QLabel("—", root);
    m_testResultLbl->setWordWrap(true);
    m_testResultLbl->setStyleSheet("font-size:12px; font-family:'JetBrains "
                                   "Mono',monospace; color:#8891b0;");

    testRow->addWidget(testBtn);
    testRow->addWidget(m_testResultLbl, 1);
    testVbox->addLayout(testRow);
    vbox->addWidget(testGroup);

    // ── Apply button ─────────────────────────────────────────────────────
    auto *applyRow = new QHBoxLayout;
    auto *applyBtn = new QPushButton("Apply Settings", root);
    applyBtn->setProperty("role", "primary");
    applyBtn->setFixedHeight(40);
    applyBtn->setFixedWidth(180);
    connect(applyBtn, &QPushButton::clicked, this,
            &ConfigWidget::applySettings);
    auto *resetBtn = new QPushButton("Reset to Defaults", root);
    resetBtn->setFixedHeight(40);
    connect(resetBtn, &QPushButton::clicked, this,
            &ConfigWidget::resetDefaults);

    applyRow->addWidget(applyBtn);
    applyRow->addWidget(resetBtn);
    applyRow->addStretch();
    vbox->addLayout(applyRow);

    // ── Env-var cheatsheet ───────────────────────────────────────────────
    auto *cheatGroup = makeGroup("Environment Variable Quick Reference", root);
    auto *cheatVbox = new QVBoxLayout(cheatGroup);
    const QString cheat = "export TSH_PORT=4444\n"
                          "export TSH_API_PORT=8080\n"
                          "export TSH_API_BIND_ADDR=127.0.0.1\n"
                          "export TSH_API_TOKEN=your-secret-token-here\n"
                          "export TSH_ZK_SECRET=$(openssl rand -hex 32)";
    auto *cheatLbl = new QLabel(cheat, root);
    cheatLbl->setStyleSheet(
        "background:#0a0c14; border:1px solid #2a2d3e; border-radius:8px;"
        "padding:14px; color:#4ade80;"
        "font-family:'JetBrains Mono','Fira Code',monospace; font-size:12px;"
        "line-height:1.8;");
    cheatVbox->addWidget(cheatLbl);

    auto *copyBtn = new QPushButton("Copy to Clipboard", root);
    copyBtn->setFixedWidth(160);
    copyBtn->setFixedHeight(30);
    connect(copyBtn, &QPushButton::clicked, this,
            [cheat] { QGuiApplication::clipboard()->setText(cheat); });
    cheatVbox->addWidget(copyBtn, 0, Qt::AlignRight);
    vbox->addWidget(cheatGroup);

    vbox->addStretch();
    scroll->setWidget(root);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(scroll);
  }

  static QGroupBox *makeGroup(const QString &title, QWidget *parent) {
    auto *g = new QGroupBox(title, parent);
    g->setStyleSheet(
        "QGroupBox { background:#1c1f2b; border:1px solid #2a2d3e; "
        "border-radius:10px;"
        " margin-top:18px; padding:16px; font-size:10px; font-weight:600;"
        " letter-spacing:0.8px; color:#4a5068; }"
        "QGroupBox::title { subcontrol-origin:margin; subcontrol-position:top "
        "left;"
        " padding:0 8px; left:14px; }");
    return g;
  }

  void loadFromApi() {
    m_apiUrlEdit->setText(m_api->baseUrl());
    m_tokenEdit->setText(m_api->token());
  }

  void applySettings() {
    m_api->setBaseUrl(m_apiUrlEdit->text().trimmed());
    m_api->setToken(m_tokenEdit->text().trimmed());
    QMessageBox::information(this, "Applied",
                             "API settings updated.\nURL: " + m_api->baseUrl());
    emit configApplied();
  }

  void resetDefaults() {
    m_apiUrlEdit->setText("https://127.0.0.1:8080");
    m_tokenEdit->clear();
    applySettings();
  }

  void testConnection() {
    m_testResultLbl->setText("Testing…");
    m_testResultLbl->setStyleSheet(
        "font-size:12px; color:#fbbf24; font-family:monospace;");
    // Temporarily apply edited values for the test
    const QString savedUrl = m_api->baseUrl();
    const QString savedToken = m_api->token();
    m_api->setBaseUrl(m_apiUrlEdit->text().trimmed());
    m_api->setToken(m_tokenEdit->text().trimmed());

    m_api->getText(
        "/metrics", [this, savedUrl, savedToken](QString body, QString err) {
          m_api->setBaseUrl(savedUrl);
          m_api->setToken(savedToken);
          if (!err.isEmpty()) {
            m_testResultLbl->setText("✗  " + err);
            m_testResultLbl->setStyleSheet(
                "font-size:12px; color:#f87171; font-family:monospace;");
          } else {
            const QString resp = body.trimmed().isEmpty()
                                     ? "(empty — likely OK)"
                                     : body.trimmed().left(120);
            m_testResultLbl->setText("✓  Connected — " + resp);
            m_testResultLbl->setStyleSheet(
                "font-size:12px; color:#4ade80; font-family:monospace;");
          }
        });
  }

  tsh::ApiClient *m_api;
  QLineEdit *m_apiUrlEdit = nullptr;
  QLineEdit *m_tokenEdit = nullptr;
  QLabel *m_testResultLbl = nullptr;
};