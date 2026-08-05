#pragma once

#include <string>

#include "zrpc/rpc/protocol_id.h"
#include "zrpc/rpc/reply.h"
#include "zrpc/rpc/server.h"

namespace direct_example {

constexpr zrpc::rpc::ProtocolId kPing = 3001;
constexpr zrpc::rpc::ProtocolId kLogin = 3002;

struct PingRequest {
  std::string message;
};

struct PingResponse {
  std::string message;
};

struct LoginRequest {
  std::string username;
  std::string password;
};

struct LoginResponse {
  std::string token;
  std::string player_id;
};

inline std::string EncodePingRequest(const PingRequest& request) {
  return request.message;
}

inline bool DecodePingRequest(const std::string& body, PingRequest* request) {
  if (request == nullptr) {
    return false;
  }
  request->message = body;
  return true;
}

inline std::string EncodePingResponse(const PingResponse& response) {
  return response.message;
}

inline bool DecodePingResponse(const std::string& body, PingResponse* response) {
  if (response == nullptr) {
    return false;
  }
  response->message = body;
  return true;
}

inline std::string EncodeLoginRequest(const LoginRequest& request) {
  return request.username + "|" + request.password;
}

inline bool DecodeLoginRequest(const std::string& body, LoginRequest* request) {
  if (request == nullptr) {
    return false;
  }
  const size_t pos = body.find('|');
  if (pos == std::string::npos) {
    return false;
  }
  request->username = body.substr(0, pos);
  request->password = body.substr(pos + 1);
  return !request->username.empty();
}

inline std::string EncodeLoginResponse(const LoginResponse& response) {
  return response.token + "|" + response.player_id;
}

inline bool DecodeLoginResponse(const std::string& body, LoginResponse* response) {
  if (response == nullptr) {
    return false;
  }
  const size_t pos = body.find('|');
  if (pos == std::string::npos) {
    return false;
  }
  response->token = body.substr(0, pos);
  response->player_id = body.substr(pos + 1);
  return !response->token.empty() && !response->player_id.empty();
}

inline void RegisterDirectProtocols(zrpc::rpc::Server* server) {
  if (server == nullptr) {
    return;
  }

  server->RegisterProtocol(kPing, [](const std::string& body) -> zrpc::rpc::Reply {
    PingRequest request;
    if (!DecodePingRequest(body, &request) || request.message.empty()) {
      return zrpc::rpc::Reply::Error(zrpc::rpc::ErrorCode::kInvalidRequest,
                                     "empty ping request");
    }
    PingResponse response;
    response.message = "pong:" + request.message;
    return zrpc::rpc::Reply::Ok(EncodePingResponse(response));
  });

  server->RegisterProtocol(kLogin, [](const std::string& body) -> zrpc::rpc::Reply {
    LoginRequest request;
    if (!DecodeLoginRequest(body, &request)) {
      return zrpc::rpc::Reply::Error(zrpc::rpc::ErrorCode::kInvalidRequest,
                                     "invalid login request");
    }
    if (request.username == "alice" && request.password == "alice123") {
      LoginResponse response;
      response.token = "token-alice";
      response.player_id = "10001";
      return zrpc::rpc::Reply::Ok(EncodeLoginResponse(response));
    }
    return zrpc::rpc::Reply::Error(zrpc::rpc::ErrorCode::kInvalidRequest,
                                   "invalid username or password");
  });
}

}  // namespace direct_example
