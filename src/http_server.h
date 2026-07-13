#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <thread>
#include <functional>
#include <map>

// Simple embedded HTTP server for REST API communication
class HttpServer {
public:
    struct Response {
        int status_code = 200;
        std::string status_text = "OK";
        std::string body;
        std::string content_type = "application/json";
    };

    using RequestHandler = std::function<Response(
        const std::string& method,
        const std::string& path,
        const std::string& body)>;

    HttpServer();
    ~HttpServer();

    bool Start(int port, RequestHandler handler);
    void Stop();
    bool IsRunning() const { return m_running; }
    int GetPort() const { return m_port; }

    // Parse URL-encoded query string (public for external use)
    static std::map<std::string, std::string> ParseQueryString(const std::string& query);
    static std::string UrlDecode(const std::string& encoded);

private:
    int m_port;
    bool m_running;
    std::thread m_server_thread;
    SOCKET m_listen_socket;
    RequestHandler m_handler;

    void ServerLoop();
    void HandleClient(SOCKET client_socket);
    std::string ReadRequest(SOCKET client_socket, std::string& method,
                            std::string& path, std::string& body);
    void SendResponse(SOCKET client_socket, int status_code,
                      const std::string& status_text,
                      const std::string& content_type,
                      const std::string& body);
};

// CORS headers helper
static const char* CORS_HEADERS =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type\r\n";
