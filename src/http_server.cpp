// Must include winsock2.h before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include "http_server.h"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <iostream>

// Link ws2_32 library (MSVC: #pragma comment(lib, "ws2_32.lib"))
// For GCC/MinGW: use -lws2_32 in linker flags

HttpServer::HttpServer()
    : m_port(0)
    , m_running(false)
    , m_listen_socket(INVALID_SOCKET) {
}

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Start(int port, RequestHandler handler) {
    m_port = port;
    m_handler = std::move(handler);

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return false;
    }

    m_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen_socket == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(m_listen_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // Only localhost
    addr.sin_port = htons((u_short)port);

    if (bind(m_listen_socket, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(m_listen_socket);
        WSACleanup();
        return false;
    }

    if (listen(m_listen_socket, SOMAXCONN) != 0) {
        closesocket(m_listen_socket);
        WSACleanup();
        return false;
    }

    m_running = true;
    m_server_thread = std::thread(&HttpServer::ServerLoop, this);

    return true;
}

void HttpServer::Stop() {
    m_running = false;
    if (m_listen_socket != INVALID_SOCKET) {
        closesocket(m_listen_socket);
        m_listen_socket = INVALID_SOCKET;
    }
    if (m_server_thread.joinable()) {
        m_server_thread.join();
    }
    WSACleanup();
}

void HttpServer::ServerLoop() {
    while (m_running) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(m_listen_socket, &read_set);

        timeval timeout = {1, 0}; // 1 second timeout for checking m_running
        int result = select(0, &read_set, nullptr, nullptr, &timeout);

        if (result > 0 && FD_ISSET(m_listen_socket, &read_set)) {
            sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            SOCKET client_socket = accept(m_listen_socket,
                                          (sockaddr*)&client_addr,
                                          &addr_len);
            if (client_socket != INVALID_SOCKET) {
                HandleClient(client_socket);
            }
        }
    }
}

void HttpServer::HandleClient(SOCKET client_socket) {
    std::string method, path, body;
    std::string raw_request = ReadRequest(client_socket, method, path, body);

    if (raw_request.empty()) {
        closesocket(client_socket);
        return;
    }

    // Debug: log incoming request
    std::cout << "[HTTP] " << method << " " << path
              << " (body_len=" << body.size() << ")" << std::endl;
    if (!body.empty() && body.size() < 256) {
        std::cout << "[HTTP] body: " << body << std::endl;
    }

    // Handle CORS preflight
    if (method == "OPTIONS") {
        std::string response =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        send(client_socket, response.c_str(), (int)response.size(), 0);
        closesocket(client_socket);
        return;
    }

    // Call the request handler
    Response resp = m_handler(method, path, body);

    // Send response
    SendResponse(client_socket, resp.status_code, resp.status_text,
                 resp.content_type, resp.body);
    closesocket(client_socket);
}

std::string HttpServer::ReadRequest(SOCKET client_socket,
                                     std::string& method,
                                     std::string& path,
                                     std::string& body) {
    char buffer[65536];
    std::string request;
    int timeout_ms = 5000;

    // Set receive timeout
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&timeout_ms, sizeof(timeout_ms));

    // Read initial data
    int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) return "";

    buffer[bytes] = '\0';
    request = buffer;

    // Parse request line
    auto first_line_end = request.find("\r\n");
    if (first_line_end == std::string::npos) return "";

    std::string request_line = request.substr(0, first_line_end);
    auto first_space = request_line.find(' ');
    auto second_space = request_line.rfind(' ');

    if (first_space == std::string::npos || second_space == std::string::npos) {
        return "";
    }

    method = request_line.substr(0, first_space);
    path = request_line.substr(first_space + 1, second_space - first_space - 1);

    // Parse headers to find Content-Length
    size_t headers_end = request.find("\r\n\r\n");
    if (headers_end == std::string::npos) return request;

    std::string headers = request.substr(0, headers_end);

    size_t cl_pos = headers.find("Content-Length: ");
    if (cl_pos == std::string::npos) {
        cl_pos = headers.find("content-length: ");
    }

    if (cl_pos != std::string::npos) {
        size_t cl_end = headers.find("\r\n", cl_pos);
        std::string cl_str = headers.substr(cl_pos + 16, cl_end - cl_pos - 16);
        // Trim
        cl_str.erase(0, cl_str.find_first_not_of(" \t\r\n"));
        cl_str.erase(cl_str.find_last_not_of(" \t\r\n") + 1);

        int content_length = std::stoi(cl_str);
        body = request.substr(headers_end + 4);

        // Read remaining body if needed
        while ((int)body.size() < content_length) {
            bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            body += buffer;
        }
    }

    return request;
}

void HttpServer::SendResponse(SOCKET client_socket, int status_code,
                               const std::string& status_text,
                               const std::string& content_type,
                               const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response << "Access-Control-Allow-Headers: Content-Type\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;

    std::string response_str = response.str();
    send(client_socket, response_str.c_str(), (int)response_str.size(), 0);
}

std::map<std::string, std::string> HttpServer::ParseQueryString(
    const std::string& query) {
    std::map<std::string, std::string> params;
    std::istringstream stream(query);
    std::string pair;

    while (std::getline(stream, pair, '&')) {
        auto eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = UrlDecode(pair.substr(0, eq_pos));
            std::string value = UrlDecode(pair.substr(eq_pos + 1));
            params[key] = value;
        }
    }

    return params;
}

std::string HttpServer::UrlDecode(const std::string& encoded) {
    std::string result;
    result.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); i++) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int high = encoded[i + 1];
            int low = encoded[i + 2];
            if (isxdigit(high) && isxdigit(low)) {
                auto hex_to_val = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    return 0;
                };
                result += (char)((hex_to_val(high) << 4) | hex_to_val(low));
                i += 2;
                continue;
            }
        } else if (encoded[i] == '+') {
            result += ' ';
            continue;
        }
        result += encoded[i];
    }

    return result;
}
