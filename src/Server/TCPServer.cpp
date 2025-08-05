#include "Server/TCPServer.hpp"
#include "Server/RequestHandler.hpp"
#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <filesystem>
#include "Search/FileIndexer.hpp"
#include <sys/socket.h>
#include <filesystem>
namespace fs = std::filesystem;

TCPServer::TCPServer(const std::filesystem::path& base_path,FileIndexer& indexer_)
    : base_path(base_path), indexer(indexer_) {
    server_fd = -1;
}

void TCPServer::start(int port_) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    std::cout << "Server listening on port " << port_ << "..." << std::endl;

    while (true) {
        int client_socket = accept(server_fd, nullptr, nullptr);
        if (client_socket < 0) {
            perror("accept failed");
            continue;
        }

        const size_t CHUNK_SIZE = 16384; // 16KB per read
        std::string request;
        ssize_t bytes_read;
        long expected_content_length = -1;
        size_t header_end = std::string::npos;
        bool headers_parsed = false;

        while (true) {
            char buffer[CHUNK_SIZE];
            bytes_read = read(client_socket, buffer, CHUNK_SIZE);
            if (bytes_read <= 0) break;
            request.append(buffer, bytes_read);

            if (!headers_parsed) {
                header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    size_t content_length_pos = request.find("Content-Length: ");
                    if (content_length_pos != std::string::npos) {
                        size_t length_start = content_length_pos + 16;
                        size_t length_end = request.find("\r\n", length_start);
                        std::string length_str = request.substr(length_start, length_end - length_start);
                        expected_content_length = std::stol(length_str);
                        std::cout << "Expected Content-Length: " << expected_content_length << " bytes\n";
                    }
                    headers_parsed = true;
                }
            }

            if (headers_parsed && expected_content_length != -1) {
                size_t headers_size = header_end + 4;
                size_t body_size = request.size() - headers_size;
                std::cout << "Body received so far: " << body_size << " bytes\n";
                if (body_size >= (size_t)expected_content_length) {
                    break;
                }
            }
        }

        if (!request.empty()) {
            std::cout << "Total request size: " << request.size() << " bytes\n";
            RequestHandler requestHandler(base_path);
            requestHandler.handleClientRequest(client_socket, request, indexer);
        } else {
            std::cout << "No data received from client\n";
        }

        close(client_socket);
    }

    close(server_fd);
}