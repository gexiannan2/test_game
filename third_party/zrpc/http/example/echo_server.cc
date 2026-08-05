#include "zrpc/base/logger.h"
#include "zrpc/http/http_server.h"

#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

void OnMessageCallback(const std::shared_ptr<zrpc::TcpConnection>&,
  const zrpc::HttpRequest& request,
  zrpc::HttpResponse* response) {
  for (const auto& header : request.GetHeaders()) {
    LOG_INFO << header.first << " " << header.second;
  }

  LOG_INFO << "path:" << request.GetPath();
  LOG_INFO << "query:" << request.GetQuery();
  LOG_INFO << "body:" <<  request.GetBody();
	
  if (request.GetMethod() == zrpc::HttpRequest::kGet) {
    if (request.GetPath() == "/") {
      response->SetStatusCode(zrpc::HttpResponse::k200k);
      response->SetStatusMessage("OK");
      response->SetContentType("text/html");
      response->AddHeader("server", "http");
      response->SetBody(
        "<html><head><title>This is title</title></head>"
        "<body><h1>Hello</h1>Now is " +
        zrpc::TimeStamp::Now().ToFormattedString() + "</body></html>");
    }
    else if (request.GetPath() == "/favicon.ico") {
      response->SetStatusCode(zrpc::HttpResponse::k200k);
      response->SetStatusMessage("OK");
      response->SetContentType("image/png");
      response->SetBody("test_image");
    }
    else if (request.GetPath() == "/hello") {
      response->SetStatusCode(zrpc::HttpResponse::k200k);
      response->SetStatusMessage("OK");
      response->SetContentType("text/plain");
      response->AddHeader("server", "http");
      response->SetBody("hello, world!\n");
    }
    else if (request.GetPath() == "/test.txt") {
      std::string filename = "attachment;filename=test.txt";
      response->SetStatusCode(zrpc::HttpResponse::k200k);
      response->SetStatusMessage("OK");
      response->SetContentType("text/plain");
      response->AddHeader("Content-Disposition", filename);
      response->SetBody("test!\n");
    }
    else {
      response->SetStatusCode(zrpc::HttpResponse::k404NotFound);
      response->SetStatusMessage("Not Found");
      response->SetCloseConnection(true);
    }
  }
  else if (request.GetMethod() == zrpc::HttpRequest::kPost) {
    response->SetStatusCode(zrpc::HttpResponse::k200k);
    response->SetStatusMessage("OK");
  }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsaData;
  int iRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (iRet != 0) {
    LOG_WARN << "WSAStartup failed: " << iRet;
    return 1;
  }
#endif

  uint16_t port = 18080;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::atoi(argv[1]));
  }

  zrpc::HttpServer server("127.0.0.1", port);
  server.SetMessageCallback(OnMessageCallback);
  server.Start();
  LOG_INFO << "http echo server listening on 127.0.0.1:" << port;
  server.Run();
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
