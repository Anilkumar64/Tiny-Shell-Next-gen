#pragma once
#include "ApiClient.h"
#include "TshStyle.h"
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

// Terminal-style widget for executing commands via the /exec HTTP API endpoint.
// Supports the allowed commands: ps, uptime, who, df
class TerminalWidget : public QWidget {
  Q_OBJECT
public:
  explicit TerminalWidget(tsh::ApiClient *api, QWidget *parent = nullptr)
      : QWidget(parent), m_api(api) {
    buildUi();
    printBanner();
  }

private slots:
  void execute() {
    const QString cmd = m_input->text().trimmed();
    if (cmd.isEmpty())
      return;

    m_history.prepend(cmd);
    m_histIdx = -1;
    m_input->clear();

    appendLine("$ " + cmd, "#00b4d8");

    // Build form body: command=<cmd>
    // Include optional headers for user/tenant tracking
    const QByteArray body = "command=" + QUrl::toPercentEncoding(cmd);
    appendLine("[dispatch] Connecting to TinyShell server...", "#8891b0");

    m_api->postForm("/exec", body, [this, cmd](QString resp, QString err) {
      if (!err.isEmpty()) {
        appendLine("[❌ Network error] " + err, "#f87171");
        appendLine("Make sure the server is running: TSH_API_TOKEN=<token> "
                   "./tsh_server",
                   "#fbbf24");
      } else if (resp.trimmed().isEmpty()) {
        appendLine("[❌ Empty response from server]", "#fbbf24");
        appendLine("Possible causes:", "#8891b0");
        appendLine("  • Invalid or missing Bearer token", "#8891b0");
        appendLine(
            "  • Server not accepting this command (try: ps, uptime, who, df)",
            "#8891b0");
        appendLine("  • Server configuration issue", "#8891b0");
      } else {
        const auto parsed = parseExecResponse(resp);
        if (!parsed.error.isEmpty()) {
          appendLine("[❌ Execution failed]", "#f87171");
          appendLine(parsed.error, "#f87171");
        } else {
          appendLine(QString("[✓ Success] worker:%1 | duration:%2ms")
                         .arg(parsed.worker.isEmpty() ? "local" : parsed.worker)
                         .arg(parsed.durationMs),
                     "#4ade80");

          // Display real command output
          if (parsed.output.isEmpty()) {
            appendLine("(no output)", "#8891b0");
          } else {
            // Format output with proper line breaks
            for (const auto &line : parsed.output.split('\n')) {
              if (!line.isEmpty()) {
                appendLine(line, "#e8eaf0");
              }
            }
          }
        }
      }
      appendLine("", {});
    });
  }

  void onQuickBtn() {
    const QString cmd =
        qobject_cast<QPushButton *>(sender())->text().remove('&');
    m_input->setText(cmd);
    execute();
  }

  void clearOutput() {
    m_output->clear();
    printBanner();
  }

private:
  void buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    // ── Header ──────────────────────────────────────────────────────────
    auto *hdrRow = new QHBoxLayout;
    auto *titleLbl = new QLabel("Terminal", this);
    titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");

    auto *clearBtn = new QPushButton("Clear", this);
    clearBtn->setFixedWidth(72);
    connect(clearBtn, &QPushButton::clicked, this,
            &TerminalWidget::clearOutput);

    hdrRow->addWidget(titleLbl);
    hdrRow->addStretch();
    hdrRow->addWidget(clearBtn);
    root->addLayout(hdrRow);

    // ── Info banner ─────────────────────────────────────────────────────
    auto *infoLbl =
        new QLabel("Distributed /exec:  "
                   "<b style='color:#00b4d8'>ps</b>  "
                   "<b style='color:#00b4d8'>uptime</b>  "
                   "<b style='color:#00b4d8'>who</b>  "
                   "<b style='color:#00b4d8'>df</b>"
                   "  &nbsp;|&nbsp;  "
                   "server validates AST/RBAC/allowlist and executes on worker",
                   this);
    infoLbl->setStyleSheet(
        "background:#1c1f2b; border:1px solid #2a2d3e; border-radius:8px;"
        "padding:10px 14px; color:#8891b0; font-size:12px;");
    root->addWidget(infoLbl);

    // ── Quick-launch buttons ─────────────────────────────────────────────
    auto *quickRow = new QHBoxLayout;
    quickRow->setSpacing(8);
    quickRow->addWidget(new QLabel("Quick:", this));
    for (const char *cmd : {"ps", "uptime", "who", "df"}) {
      auto *btn = new QPushButton(cmd, this);
      btn->setFixedHeight(30);
      btn->setStyleSheet(
          "QPushButton { background:#1c1f2b; border:1px solid #2a2d3e;"
          "  border-radius:6px; color:#00b4d8; font-family:'JetBrains "
          "Mono','Courier New',mono;"
          "  padding:0 14px; font-weight:600; }"
          "QPushButton:hover { background:#22263a; border-color:#00b4d8; }");
      connect(btn, &QPushButton::clicked, this, &TerminalWidget::onQuickBtn);
      quickRow->addWidget(btn);
    }
    quickRow->addStretch();
    root->addLayout(quickRow);

    // ── Output area ─────────────────────────────────────────────────────
    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(QFont(
        "JetBrains Mono,Fira Code,Cascadia Code,Consolas,Courier New", 12));
    m_output->setStyleSheet("QPlainTextEdit {"
                            "  background-color: #0a0c14;"
                            "  border: 1px solid #2a2d3e;"
                            "  border-radius: 10px;"
                            "  color: #e8eaf0;"
                            "  padding: 12px;"
                            "  selection-background-color: #0077a8;"
                            "}");
    m_output->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    root->addWidget(m_output, 1);

    // ── Input row ───────────────────────────────────────────────────────
    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(10);

    auto *promptLbl = new QLabel("$", this);
    promptLbl->setStyleSheet(
        "font-size:16px; font-weight:700; color:#00b4d8;"
        "font-family:'JetBrains Mono','Courier New',monospace;");

    m_input = new QLineEdit(this);
    m_input->setFont(
        QFont("JetBrains Mono,Fira Code,Consolas,Courier New", 13));
    m_input->setPlaceholderText("type command and press Enter…");
    m_input->setStyleSheet("QLineEdit {"
                           "  background-color: #0a0c14;"
                           "  border: 1px solid #2a2d3e;"
                           "  border-radius: 8px;"
                           "  color: #e8eaf0;"
                           "  padding: 8px 12px;"
                           "}"
                           "QLineEdit:focus { border-color: #00b4d8; }");
    m_input->installEventFilter(this);
    connect(m_input, &QLineEdit::returnPressed, this, &TerminalWidget::execute);

    auto *runBtn = new QPushButton("Run ↵", this);
    runBtn->setProperty("role", "primary");
    runBtn->setFixedHeight(36);
    runBtn->setFixedWidth(80);
    connect(runBtn, &QPushButton::clicked, this, &TerminalWidget::execute);

    inputRow->addWidget(promptLbl);
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(runBtn);
    root->addLayout(inputRow);
  }

  // Up/Down arrow for history navigation
  bool eventFilter(QObject *obj, QEvent *ev) override {
    if (obj == m_input && ev->type() == QEvent::KeyPress) {
      auto *ke = static_cast<QKeyEvent *>(ev);
      if (ke->key() == Qt::Key_Up) {
        if (m_histIdx + 1 < m_history.size())
          m_input->setText(m_history[++m_histIdx]);
        return true;
      }
      if (ke->key() == Qt::Key_Down) {
        if (m_histIdx > 0)
          m_input->setText(m_history[--m_histIdx]);
        else {
          m_histIdx = -1;
          m_input->clear();
        }
        return true;
      }
    }
    return QWidget::eventFilter(obj, ev);
  }

  void appendLine(const QString &text, const QString &color) {
    if (color.isEmpty()) {
      m_output->appendPlainText("");
      return;
    }
    // Use HTML for coloured output
    QString safe = text.toHtmlEscaped();
    safe.replace("\n", "<br/>");
    m_output->appendHtml(QString("<span style='color:%1; white-space:pre; "
                                 "font-family:monospace;'>%2</span>")
                             .arg(color, safe));
    m_output->verticalScrollBar()->setValue(
        m_output->verticalScrollBar()->maximum());
  }

  void printBanner() {
    appendLine("╔══════════════════════════════════════════════════╗\n"
               "║   TinyShell NextGen  ·  Distributed Terminal       ║\n"
               "║   Client → Server → Worker → Result               ║\n"
               "╚══════════════════════════════════════════════════╝",
               "#00b4d8");
    appendLine("Allowed distributed commands: ps  uptime  who  df", "#8891b0");
    appendLine("", {});
  }

  struct ParsedExecResponse {
    QString output;
    QString error;
    QString worker;
    qint64 durationMs = 0;
  };

  static ParsedExecResponse parseExecResponse(const QString &body) {
    ParsedExecResponse parsed;
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
      parsed.output = body;
      return parsed;
    }

    const auto obj = doc.object();
    parsed.worker = obj.value("worker").toString();
    parsed.durationMs = obj.value("duration_ms").toVariant().toLongLong();
    if (obj.contains("error")) {
      parsed.error = obj.value("error").toString();
    } else {
      parsed.output = obj.value("output").toString();
    }
    return parsed;
  }

  tsh::ApiClient *m_api;
  QPlainTextEdit *m_output = nullptr;
  QLineEdit *m_input = nullptr;
  QStringList m_history;
  int m_histIdx = -1;
};
