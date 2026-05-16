#include "SpineGrpc.h"

#include <grpcpp/impl/codegen/client_unary_call.h>
#include <grpcpp/impl/codegen/method_handler.h>
#include <grpcpp/impl/codegen/rpc_method.h>
#include <grpcpp/impl/codegen/rpc_service_method.h>
#include <grpcpp/impl/codegen/sync_stream.h>

#include <functional>

namespace tinyshell::v1 {

ControlPlaneService::Stub::Stub(
    const std::shared_ptr<grpc::ChannelInterface> &channel)
    : channel_(channel),
      rpc_submit_("/tinyshell.v1.ControlPlaneService/SubmitJob",
                  grpc::internal::RpcMethod::NORMAL_RPC, channel),
      rpc_watch_("/tinyshell.v1.ControlPlaneService/WatchJob",
                 grpc::internal::RpcMethod::SERVER_STREAMING, channel) {}

grpc::Status ControlPlaneService::Stub::SubmitJob(
    grpc::ClientContext *context, const SubmitJobRequest &request,
    SubmitJobResponse *response) {
  return grpc::internal::BlockingUnaryCall(channel_.get(), rpc_submit_, context,
                                           request, response);
}

std::unique_ptr<grpc::ClientReaderInterface<JobEvent>>
ControlPlaneService::Stub::WatchJob(grpc::ClientContext *context,
                                    const WatchJobRequest &request) {
  return std::unique_ptr<grpc::ClientReaderInterface<JobEvent>>(
      grpc::internal::ClientReaderFactory<JobEvent>::Create(
          channel_.get(), rpc_watch_, context, request));
}

std::unique_ptr<ControlPlaneService::Stub>
ControlPlaneService::NewStub(
    const std::shared_ptr<grpc::ChannelInterface> &channel) {
  return std::make_unique<ControlPlaneService::Stub>(channel);
}

ControlPlaneService::Service::Service() {
  AddMethod(new grpc::internal::RpcServiceMethod(
      "/tinyshell.v1.ControlPlaneService/SubmitJob",
      grpc::internal::RpcMethod::NORMAL_RPC,
      new grpc::internal::RpcMethodHandler<Service, SubmitJobRequest,
                                           SubmitJobResponse>(
          std::mem_fn(&Service::SubmitJob), this)));
  AddMethod(new grpc::internal::RpcServiceMethod(
      "/tinyshell.v1.ControlPlaneService/WatchJob",
      grpc::internal::RpcMethod::SERVER_STREAMING,
      new grpc::internal::ServerStreamingHandler<Service, WatchJobRequest,
                                                 JobEvent>(
          std::mem_fn(&Service::WatchJob), this)));
}

grpc::Status ControlPlaneService::Service::SubmitJob(
    grpc::ServerContext *, const SubmitJobRequest *, SubmitJobResponse *) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not implemented");
}

grpc::Status ControlPlaneService::Service::WatchJob(
    grpc::ServerContext *, const WatchJobRequest *,
    grpc::ServerWriter<JobEvent> *) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not implemented");
}

AgentConnector::Stub::Stub(
    const std::shared_ptr<grpc::ChannelInterface> &channel)
    : channel_(channel),
      rpc_connect_("/tinyshell.v1.AgentConnector/Connect",
                   grpc::internal::RpcMethod::BIDI_STREAMING, channel) {}

std::unique_ptr<grpc::ClientReaderWriterInterface<AgentMessage, ServerMessage>>
AgentConnector::Stub::Connect(grpc::ClientContext *context) {
  return std::unique_ptr<
      grpc::ClientReaderWriterInterface<AgentMessage, ServerMessage>>(
      grpc::internal::ClientReaderWriterFactory<AgentMessage, ServerMessage>::
          Create(channel_.get(), rpc_connect_, context));
}

std::unique_ptr<AgentConnector::Stub>
AgentConnector::NewStub(
    const std::shared_ptr<grpc::ChannelInterface> &channel) {
  return std::make_unique<AgentConnector::Stub>(channel);
}

AgentConnector::Service::Service() {
  AddMethod(new grpc::internal::RpcServiceMethod(
      "/tinyshell.v1.AgentConnector/Connect",
      grpc::internal::RpcMethod::BIDI_STREAMING,
      new grpc::internal::BidiStreamingHandler<Service, AgentMessage,
                                               ServerMessage>(
          std::mem_fn(&Service::Connect), this)));
}

grpc::Status AgentConnector::Service::Connect(
    grpc::ServerContext *,
    grpc::ServerReaderWriter<ServerMessage, AgentMessage> *) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not implemented");
}

} // namespace tinyshell::v1
