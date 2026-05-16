#pragma once

#include "tinyshell/v1/spine.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/proto_utils.h>
#include <grpcpp/impl/codegen/service_type.h>
#include <grpcpp/support/sync_stream.h>

#include <memory>

namespace tinyshell::v1 {

class ControlPlaneService final {
public:
  static constexpr const char *service_full_name() {
    return "tinyshell.v1.ControlPlaneService";
  }

  class Stub {
  public:
    explicit Stub(const std::shared_ptr<grpc::ChannelInterface> &channel);

    grpc::Status SubmitJob(grpc::ClientContext *context,
                           const SubmitJobRequest &request,
                           SubmitJobResponse *response);

    std::unique_ptr<grpc::ClientReaderInterface<JobEvent>>
    WatchJob(grpc::ClientContext *context, const WatchJobRequest &request);

  private:
    std::shared_ptr<grpc::ChannelInterface> channel_;
    grpc::internal::RpcMethod rpc_submit_;
    grpc::internal::RpcMethod rpc_watch_;
  };

  static std::unique_ptr<Stub>
  NewStub(const std::shared_ptr<grpc::ChannelInterface> &channel);

  class Service : public grpc::Service {
  public:
    Service();
    ~Service() override = default;

    virtual grpc::Status SubmitJob(grpc::ServerContext *context,
                                   const SubmitJobRequest *request,
                                   SubmitJobResponse *response);

    virtual grpc::Status WatchJob(grpc::ServerContext *context,
                                  const WatchJobRequest *request,
                                  grpc::ServerWriter<JobEvent> *writer);
  };
};

class AgentConnector final {
public:
  static constexpr const char *service_full_name() {
    return "tinyshell.v1.AgentConnector";
  }

  class Stub {
  public:
    explicit Stub(const std::shared_ptr<grpc::ChannelInterface> &channel);

    std::unique_ptr<grpc::ClientReaderWriterInterface<AgentMessage, ServerMessage>>
    Connect(grpc::ClientContext *context);

  private:
    std::shared_ptr<grpc::ChannelInterface> channel_;
    grpc::internal::RpcMethod rpc_connect_;
  };

  static std::unique_ptr<Stub>
  NewStub(const std::shared_ptr<grpc::ChannelInterface> &channel);

  class Service : public grpc::Service {
  public:
    Service();
    ~Service() override = default;

    virtual grpc::Status
    Connect(grpc::ServerContext *context,
            grpc::ServerReaderWriter<ServerMessage, AgentMessage> *stream);
  };
};

} // namespace tinyshell::v1
