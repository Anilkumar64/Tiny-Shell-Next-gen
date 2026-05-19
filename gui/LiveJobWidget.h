#pragma once

#include "GrpcJobClient.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

class LiveJobWidget : public QWidget {
  Q_OBJECT
public:
  explicit LiveJobWidget(QWidget *parent = nullptr) : QWidget(parent) {
    qRegisterMetaType<tinyshell::v1::JobEvent>("tinyshell::v1::JobEvent");
    m_client = new GrpcJobClient(this);
    m_client->configure(QString::fromLocal8Bit(
        qgetenv("TSH_SPINE_TARGET").isEmpty() ? "127.0.0.1:7443"
                                              : qgetenv("TSH_SPINE_TARGET")));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *top = new QHBoxLayout;
    m_user = new QLineEdit("operator", this);
    m_user->setMaximumWidth(180);
    m_command = new QLineEdit("uptime", this);
    m_command->setPlaceholderText("Command");
    m_submit = new QPushButton("Run", this);
    m_submit->setDefault(true);
    top->addWidget(new QLabel("User", this));
    top->addWidget(m_user);
    top->addWidget(new QLabel("Command", this));
    top->addWidget(m_command, 1);
    top->addWidget(m_submit);
    root->addLayout(top);

    m_status = new QLabel("Idle", this);
    m_status->setStyleSheet("color:#8891b0; padding:4px 0;");
    root->addWidget(m_status);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    m_lifecycle = new QPlainTextEdit(splitter);
    m_lifecycle->setReadOnly(true);
    m_lifecycle->document()->setMaximumBlockCount(5000);
    m_lifecycle->setPlaceholderText("Lifecycle events");
    m_output = new QPlainTextEdit(splitter);
    m_output->setReadOnly(true);
    m_output->document()->setMaximumBlockCount(10000);
    m_output->setPlaceholderText("stdout/stderr");
    splitter->addWidget(m_lifecycle);
    splitter->addWidget(m_output);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    connect(m_submit, &QPushButton::clicked, this, &LiveJobWidget::submit);
    connect(m_command, &QLineEdit::returnPressed, this, &LiveJobWidget::submit);
    connect(m_client, &GrpcJobClient::submitted, this,
            [this](const QString &jobId, const QString &message) {
              m_status->setText("Submitted " + jobId + " · " + message);
              appendLifecycle("submit.accepted", message);
              emit jobAccepted(jobId, m_pendingCommand);
            });
    connect(m_client, &GrpcJobClient::failed, this, [this](const QString &err) {
      m_submit->setEnabled(true);
      m_status->setText("Failed · " + err);
      appendLifecycle("client.error", err);
      emit jobCompleted(false);
    });
    connect(m_client, &GrpcJobClient::streamDisconnected, this,
            [this](const QString &err) {
              m_status->setText("Stream reconnecting...");
              appendLifecycle("stream.reconnect", err);
            });
    connect(m_client, &GrpcJobClient::eventReceived, this,
            &LiveJobWidget::onEvent);
  }

  GrpcJobClient *grpcClient() const { return m_client; }

signals:
  void jobSubmitted(const QString &command);
  void jobAccepted(const QString &jobId, const QString &command);
  void jobCompleted(bool ok);

private slots:
  void submit() {
    m_lifecycle->clear();
    m_output->clear();
    m_submit->setEnabled(false);
    m_status->setText("Submitting...");
    const QString cmd = m_command->text().trimmed();
    m_pendingCommand = cmd;
    emit jobSubmitted(cmd);
    m_client->submit(m_user->text().trimmed(), cmd);
  }

  void onEvent(const tinyshell::v1::JobEvent &event) {
    appendLifecycle(eventName(event.type()),
                    QString("seq=%1 actor=%2 agent=%3")
                        .arg(event.sequence())
                        .arg(QString::fromStdString(event.actor()))
                        .arg(QString::fromStdString(event.agent_id())));

    if (event.has_output()) {
      const auto prefix = event.output().stream() == tinyshell::v1::STDERR
                              ? "[stderr] "
                              : "[stdout] ";
      m_output->moveCursor(QTextCursor::End);
      m_output->insertPlainText(
          prefix + QString::fromUtf8(event.output().data().data(),
                                     event.output().data().size()));
      m_output->moveCursor(QTextCursor::End);
    } else if (event.has_exit()) {
      m_submit->setEnabled(true);
      m_status->setText(
          QString("Exited code=%1 · %2")
              .arg(event.exit().exit_code())
              .arg(QString::fromStdString(event.exit().reason())));
      emit jobCompleted(event.exit().exit_code() == 0);
    } else if (event.payload_case() == tinyshell::v1::JobEvent::kMessage) {
      appendLifecycle("message", QString::fromStdString(event.message()));
      if (event.type() == tinyshell::v1::JOB_FAILED ||
          event.type() == tinyshell::v1::JOB_TIMED_OUT ||
          event.type() == tinyshell::v1::JOB_KILLED ||
          event.type() == tinyshell::v1::JOB_LOST ||
          event.type() == tinyshell::v1::JOB_REJECTED) {
        m_submit->setEnabled(true);
        m_status->setText("Failed · " +
                          QString::fromStdString(event.message()));
        emit jobCompleted(false);
      }
    }

    if (event.type() == tinyshell::v1::AUDIT_RECORDED) {
      appendLifecycle("audit", "final audit event persisted");
    }
  }

private:
  static QString eventName(tinyshell::v1::JobEventType type) {
    switch (type) {
    case tinyshell::v1::JOB_CREATED:
      return "job.created";
    case tinyshell::v1::JOB_VALIDATED:
      return "job.validated";
    case tinyshell::v1::JOB_SIGNED:
      return "job.signed";
    case tinyshell::v1::JOB_ASSIGNED:
      return "job.assigned";
    case tinyshell::v1::JOB_DELIVERED:
      return "job.delivered";
    case tinyshell::v1::JOB_AGENT_ACCEPTED:
      return "job.agent_accepted";
    case tinyshell::v1::JOB_STARTED:
      return "job.started";
    case tinyshell::v1::JOB_STDOUT:
      return "job.stdout";
    case tinyshell::v1::JOB_STDERR:
      return "job.stderr";
    case tinyshell::v1::JOB_EXITED:
      return "job.exited";
    case tinyshell::v1::JOB_FAILED:
      return "job.failed";
    case tinyshell::v1::JOB_TIMED_OUT:
      return "job.timed_out";
    case tinyshell::v1::JOB_KILLED:
      return "job.killed";
    case tinyshell::v1::JOB_LOST:
      return "job.lost";
    case tinyshell::v1::AUDIT_RECORDED:
      return "audit.recorded";
    default:
      return "event";
    }
  }

  void appendLifecycle(const QString &name, const QString &detail) {
    m_lifecycle->appendPlainText(
        QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + "  " + name +
        "  " + detail);
  }

  GrpcJobClient *m_client = nullptr;
  QLineEdit *m_user = nullptr;
  QLineEdit *m_command = nullptr;
  QString m_pendingCommand;
  QPushButton *m_submit = nullptr;
  QLabel *m_status = nullptr;
  QPlainTextEdit *m_lifecycle = nullptr;
  QPlainTextEdit *m_output = nullptr;
};
