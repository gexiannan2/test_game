#pragma once

#include <functional>
#include <string>
#include <utility>

#include <google/protobuf/message.h>

#include "zrpc/rpc/options.h"
#include "zrpc/rpc/protocol_id.h"
#include "zrpc/rpc/reply.h"

namespace zrpc {
namespace rpc {

template <typename Message>
inline std::string SerializeProto(const Message& message) {
  return message.SerializeAsString();
}

template <typename Message>
inline bool ParseProto(const std::string& bytes, Message* message) {
  return message != nullptr && message->ParseFromString(bytes);
}

using ProtocolHandler = Handler;

class Server;

template <typename Request, typename Response>
using ProtoHandler =
    std::function<Reply(const Request& request, Response* response)>;

class Client;

template <typename Request, typename Response>
void RegisterProto(Server* server, ProtocolId id,
                   ProtoHandler<Request, Response> handler);

template <typename Request, typename Response>
void AsyncCallProto(Client* client, ProtocolId id, const Request& request,
                    std::function<void(const Reply&, const Response&)> callback,
                    const CallOptions& options = {});

template <typename Request, typename Response>
Reply CallProto(Client* client, ProtocolId id, const Request& request,
                Response* response, const CallOptions& options = {});

}  // namespace rpc
}  // namespace zrpc

#include "zrpc/rpc/server.h"
#include "zrpc/rpc/client.h"

namespace zrpc {
namespace rpc {

template <typename Request, typename Response>
inline void RegisterProto(Server* server, ProtocolId id,
                          ProtoHandler<Request, Response> handler) {
  if (server == nullptr || !handler) {
    return;
  }
  server->RegisterProtocol(
      id, [handler = std::move(handler)](const std::string& body) -> Reply {
        Request request;
        if (!ParseProto(body, &request)) {
          return Reply::Error(ErrorCode::kInvalidRequest,
                              "failed to parse request proto");
        }
        Response response;
        const Reply reply = handler(request, &response);
        if (!reply.ok()) {
          return reply;
        }
        return Reply::Ok(SerializeProto(response));
      });
}

template <typename Request, typename Response>
inline void AsyncCallProto(
    Client* client, ProtocolId id, const Request& request,
    std::function<void(const Reply&, const Response&)> callback,
    const CallOptions& options) {
  if (client == nullptr) {
    if (callback) {
      Response empty;
      callback(Reply::Error(ErrorCode::kTransport, "rpc client unavailable"),
               empty);
    }
    return;
  }
  client->AsyncCall(id, SerializeProto(request),
                    [callback = std::move(callback)](const Reply& reply) {
                      Response response;
                      if (!reply.ok()) {
                        if (callback) {
                          callback(reply, response);
                        }
                        return;
                      }
                      if (!ParseProto(reply.body(), &response)) {
                        if (callback) {
                          callback(Reply::Error(ErrorCode::kInvalidResponse,
                                                "failed to parse response proto"),
                                    response);
                        }
                        return;
                      }
                      if (callback) {
                        callback(reply, response);
                      }
                    },
                    options);
}

template <typename Request, typename Response>
inline Reply CallProto(Client* client, ProtocolId id, const Request& request,
                       Response* response, const CallOptions& options) {
  if (client == nullptr) {
    return Reply::Error(ErrorCode::kTransport, "rpc client unavailable");
  }
  if (response == nullptr) {
    return Reply::Error(ErrorCode::kInvalidResponse, "null response");
  }
  const Reply reply = client->Call(id, SerializeProto(request), options);
  if (!reply.ok()) {
    return reply;
  }
  if (!ParseProto(reply.body(), response)) {
    return Reply::Error(ErrorCode::kInvalidResponse,
                        "failed to parse response proto");
  }
  return Reply::Ok(reply.body());
}

}  // namespace rpc
}  // namespace zrpc
