#pragma once

#include "tinyshell/v1/spine.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <QObject>
#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

class GrpcJobClient : public QObject {
  Q_OBJECT
public:
  explicit GrpcJobClient(QObject *parent = nullptr) : QObject(parent) {}
  ~GrpcJobClient() override { stop(); }

  void configure(QString target) { m_target = std::move(target); }

  void submit(QString userId, QString command) {
    stop();
    m_stop.store(false);
    m_thread = std::thread([this, userId = std::move(userId),
                            command = std::move(command)] {
      try {
        auto channel = grpc::CreateChannel(m_target.toStdString(), credentials());
        auto stub = tinyshell::v1::ControlPlaneService::NewStub(channel);

        tinyshell::v1::SubmitJobRequest req;
        req.set_user_id(userId.toStdString());
        req.set_command(command.toStdString());

        tinyshell::v1::SubmitJobResponse resp;
        grpc::ClientContext submitContext;
        submitContext.set_deadline(std::chrono::system_clock::now() +
                                   std::chrono::seconds(10));
        setActiveContext(&submitContext);
        const auto submitStatus = stub->SubmitJob(&submitContext, req, &resp);
        setActiveContext(nullptr);
        if (!submitStatus.ok()) {
          emit failed(QString::fromStdString(submitStatus.error_message()));
          return;
        }
        if (resp.job_id().empty()) {
          emit failed(QString::fromStdString(resp.message()));
          return;
        }

        const auto jobId = QString::fromStdString(resp.job_id());
        emit submitted(jobId, QString::fromStdString(resp.message()));

        std::uint64_t lastSequence = 0;
        int backoffMs = 500;
        while (!m_stop.load()) {
          tinyshell::v1::WatchJobRequest watch;
          watch.set_job_id(resp.job_id());
          watch.set_after_sequence(lastSequence);

          grpc::ClientContext watchContext;
          watchContext.set_deadline(std::chrono::system_clock::now() +
                                    std::chrono::minutes(10));
          setActiveContext(&watchContext);
          auto reader = stub->WatchJob(&watchContext, watch);
          tinyshell::v1::JobEvent event;
          bool sawTerminal = false;
          bool readAny = false;
          while (!m_stop.load() && reader->Read(&event)) {
            readAny = true;
            if (event.sequence() > lastSequence) {
              lastSequence = event.sequence();
            }
            emit eventReceived(event);
            if (isTerminal(event.type())) {
              sawTerminal = true;
              break;
            }
          }
          watchContext.TryCancel();
          const auto watchStatus = reader->Finish();
          setActiveContext(nullptr);

          if (m_stop.load() || sawTerminal) {
            break;
          }
          if (!watchStatus.ok()) {
            emit streamDisconnected(
                QString::fromStdString(watchStatus.error_message()));
          }
          if (readAny)
            backoffMs = 500;
          sleepWithStop(backoffMs);
          backoffMs = std::min(backoffMs * 2, 16000);
        }
      } catch (const std::exception &e) {
        setActiveContext(nullptr);
        emit failed(QString::fromStdString(e.what()));
      }
    });
  }

  void stop() {
    m_stop.store(true);
    {
      std::lock_guard<std::mutex> lock(m_contextMutex);
      if (m_activeContext != nullptr) {
        m_activeContext->TryCancel();
      }
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

signals:
  void submitted(QString jobId, QString message);
  void eventReceived(tinyshell::v1::JobEvent event);
  void streamDisconnected(QString error);
  void failed(QString error);

private:
  void setActiveContext(grpc::ClientContext *context) {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    m_activeContext = context;
  }

  static bool isTerminal(tinyshell::v1::JobEventType type) {
    return type == tinyshell::v1::JOB_EXITED ||
           type == tinyshell::v1::JOB_FAILED ||
           type == tinyshell::v1::JOB_TIMED_OUT ||
           type == tinyshell::v1::JOB_KILLED ||
           type == tinyshell::v1::JOB_LOST ||
           type == tinyshell::v1::JOB_REJECTED;
  }

  void sleepWithStop(int totalMs) const {
    int sleptMs = 0;
    while (!m_stop.load() && sleptMs < totalMs) {
      const int stepMs = std::min(100, totalMs - sleptMs);
      std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
      sleptMs += stepMs;
    }
  }

  static std::string env(const char *name) {
    if (const char *value = std::getenv(name)) {
      return value;
    }
    return {};
  }

  static std::string readFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      throw std::runtime_error("failed to read file: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }

  static std::shared_ptr<grpc::ChannelCredentials> credentials() {
    if (env("TSH_GRPC_INSECURE_DEV") == "1") {
      return grpc::InsecureChannelCredentials();
    }
    grpc::SslCredentialsOptions opts;
    const auto root = env("TSH_GRPC_ROOT_CA");
    if (!root.empty()) {
      opts.pem_root_certs = readFile(root);
    }
    const auto cert = env("TSH_GRPC_CLIENT_CERT");
    const auto key = env("TSH_GRPC_CLIENT_KEY");
    if (!cert.empty() || !key.empty()) {
      if (cert.empty() || key.empty()) {
        throw std::runtime_error(
            "both TSH_GRPC_CLIENT_CERT and TSH_GRPC_CLIENT_KEY are required");
      }
      opts.pem_cert_chain = readFile(cert);
      opts.pem_private_key = readFile(key);
    }
    return grpc::SslCredentials(opts);
  }

  QString m_target = "127.0.0.1:7443";
  std::atomic<bool> m_stop{false};
  std::thread m_thread;
  std::mutex m_contextMutex;
  grpc::ClientContext *m_activeContext = nullptr;
};
