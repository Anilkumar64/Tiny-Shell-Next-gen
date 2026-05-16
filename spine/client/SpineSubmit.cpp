#include "tinyshell/v1/spine.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::string env_string(const char *name, const std::string &fallback = {}) {
  if (const char *value = std::getenv(name)) {
    return value;
  }
  return fallback;
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read file: " + path);
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

std::shared_ptr<grpc::ChannelCredentials> credentials_from_env() {
  if (env_string("TSH_GRPC_INSECURE_DEV") == "1") {
    if (env_string("TSH_I_KNOW_THIS_IS_INSECURE") != "1") {
      throw std::runtime_error("TSH_GRPC_INSECURE_DEV=1 requires "
                               "TSH_I_KNOW_THIS_IS_INSECURE=1");
    }
    std::cerr << "[TinyShell Submit] WARNING: using insecure gRPC because "
                 "TSH_GRPC_INSECURE_DEV=1\n";
    return grpc::InsecureChannelCredentials();
  }

  grpc::SslCredentialsOptions opts;
  const auto root_ca = env_string("TSH_GRPC_ROOT_CA");
  if (!root_ca.empty()) {
    opts.pem_root_certs = read_file(root_ca);
  }
  const auto cert = env_string("TSH_GRPC_CLIENT_CERT");
  const auto key = env_string("TSH_GRPC_CLIENT_KEY");
  if (!cert.empty() || !key.empty()) {
    if (cert.empty() || key.empty()) {
      throw std::runtime_error(
          "both TSH_GRPC_CLIENT_CERT and TSH_GRPC_CLIENT_KEY are required");
    }
    opts.pem_cert_chain = read_file(cert);
    opts.pem_private_key = read_file(key);
  }
  return grpc::SslCredentials(opts);
}

std::string event_name(tinyshell::v1::JobEventType type) {
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
  case tinyshell::v1::JOB_REJECTED:
    return "job.rejected";
  case tinyshell::v1::AUDIT_RECORDED:
    return "audit.recorded";
  default:
    return "event";
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto target = env_string("TSH_SPINE_TARGET", "127.0.0.1:7443");
    const std::string command = argc > 1 ? argv[1] : "uptime";
    const std::string user = env_string("TSH_USER_ID", "operator");

    auto channel = grpc::CreateChannel(target, credentials_from_env());
    auto stub = tinyshell::v1::ControlPlaneService::NewStub(channel);

    tinyshell::v1::SubmitJobRequest request;
    request.set_user_id(user);
    request.set_command(command);

    tinyshell::v1::SubmitJobResponse response;
    grpc::ClientContext submit_context;
    submit_context.set_deadline(std::chrono::system_clock::now() +
                                std::chrono::seconds(10));
    const auto submit_status =
        stub->SubmitJob(&submit_context, request, &response);
    if (!submit_status.ok()) {
      std::cerr << "SubmitJob failed: " << submit_status.error_message()
                << "\n";
      return 1;
    }
    if (response.job_id().empty()) {
      std::cerr << "SubmitJob rejected: " << response.message() << "\n";
      return 2;
    }

    std::cout << "job_id=" << response.job_id()
              << " message=" << response.message() << "\n";

    tinyshell::v1::WatchJobRequest watch;
    watch.set_job_id(response.job_id());
    grpc::ClientContext watch_context;
    watch_context.set_deadline(std::chrono::system_clock::now() +
                               std::chrono::seconds(30));
    auto reader = stub->WatchJob(&watch_context, watch);

    tinyshell::v1::JobEvent event;
    while (reader->Read(&event)) {
      std::cout << "event seq=" << event.sequence()
                << " type=" << event_name(event.type()) << "\n";
      if (event.has_output()) {
        std::cout << (event.output().stream() == tinyshell::v1::STDERR
                          ? "[stderr] "
                          : "[stdout] ")
                  << event.output().data();
        if (event.output().data().empty() ||
            event.output().data().back() != '\n') {
          std::cout << "\n";
        }
      }
      if (event.has_exit()) {
        std::cout << "exit_code=" << event.exit().exit_code()
                  << " reason=" << event.exit().reason() << "\n";
        break;
      }
      if (event.payload_case() == tinyshell::v1::JobEvent::kMessage) {
        std::cout << "message=" << event.message() << "\n";
      }
    }

    watch_context.TryCancel();
    const auto watch_status = reader->Finish();
    if (!watch_status.ok() &&
        watch_status.error_code() != grpc::StatusCode::CANCELLED) {
      std::cerr << "WatchJob failed: " << watch_status.error_message() << "\n";
      return 3;
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "tsh_spine_submit failed: " << e.what() << "\n";
    return 1;
  }
}
