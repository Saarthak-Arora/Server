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

        char buffer[8192] = {0};  // Increased buffer size
        std::string request;
        ssize_t total_bytes_read = 0;
        ssize_t bytes_read;
        
        // Read the request in chunks
        while ((bytes_read = read(client_socket, buffer, sizeof(buffer))) > 0) {
            request.append(buffer, bytes_read);
            total_bytes_read += bytes_read;
            
            // Check if we have received the complete HTTP headers
            size_t header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                // Found end of headers, now check if we need to read more data
                size_t content_length_pos = request.find("Content-Length: ");
                if (content_length_pos != std::string::npos) {
                    size_t length_start = content_length_pos + 16;
                    size_t length_end = request.find("\r\n", length_start);
                    std::string length_str = request.substr(length_start, length_end - length_start);
                    long expected_content_length = std::stol(length_str);
                    
                    // Calculate how much body we have received
                    size_t headers_size = header_end + 4;
                    size_t body_size = request.size() - headers_size;
                    
                    std::cout << "Expected Content-Length: " << expected_content_length << " bytes\n";
                    std::cout << "Body received so far: " << body_size << " bytes\n";
                    
                    // If we have received all the expected content, break
                    if (body_size >= expected_content_length) {
                        break;
                    }
                } else {
                    // No Content-Length header, assume request is complete
                    break;
                }
            }
            
            // Clear buffer for next read
            memset(buffer, 0, sizeof(buffer));
        }

        if (total_bytes_read > 0) {
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