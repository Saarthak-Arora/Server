#include "../../include/Server/RequestHandler.hpp"
#include "../../include/Search/Sorting.hpp"
#include "../../include/Search/FileIndexer.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <curl/curl.h>
#include <unistd.h>
#include <ctime>
#include "Cache/LRUCache.hpp"

LRUCache* cache_ = new LRUCache(5); 

RequestHandler::RequestHandler( const std::filesystem::path& basePath)
    : base_path_(basePath) {}



std::string RequestHandler::urlMethod(const std::string& req) {
    size_t start = req.find("GET ");
    if (start != std::string::npos) return "GET";
    start = req.find("POST ");
    if (start != std::string::npos) return "POST";
    start = req.find("PUT ");
    if (start != std::string::npos) return "PUT";
    start = req.find("DELETE ");
    if (start != std::string::npos) return "DELETE";

    return "/"; // Default to root if no method found
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string RequestHandler::fetchWebPage(const std::string& url) {
    CURL* curl;
    CURLcode res;
    std::string readBuffer;
    curl = curl_easy_init();

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");

        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
            readBuffer = "";
        }

        curl_easy_cleanup(curl);
    } else {
        std::cerr << "Failed to init curl\n";
    }

    return readBuffer;
}

std::string RequestHandler::googleFallback(const std::string& query, int client_fd) {
    std::cout << "Redirecting to Google search for: " << query << "\n";
    if (query.empty() || query == "/") {
        std::cout << "Please provide valid query" << std::endl;
        return "";
    }

    std::string search_url = "https://www.google.com/search?q=" + query;
    std::string webpage = fetchWebPage(search_url);

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " + std::to_string(webpage.size()) + "\r\n"
        "\r\n" + webpage;

    send(client_fd, response.c_str(), response.length(), 0);
    return webpage;
}

void RequestHandler::getRequestHandler(int client_fd, FileIndexer& indexer, std::string& path) {
    std::cout << "Parsed path: " << path << std::endl;

    if (path.rfind("/search?q=", 0) == 0 && path.size() > 10) {
        std::string query = path.substr(10);
        std::cout << "Searching for: " << query << "\n";

        std::vector<std::string> queryTokens = indexer.tokenize(query);
        if (queryTokens.empty()) {
            std::string cached = cache_->get(query);
            if (!cached.empty()) {
                std::cout << "Cache hit for: " << query << std::endl;
                std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: " + std::to_string(cached.size()) + "\r\n"
                    "\r\n" + cached;
                send(client_fd, response.c_str(), response.size(), 0);
            } else {
                std::string fetchedHtml = googleFallback(query, client_fd);
                cache_->put(query, fetchedHtml);
            }
            close(client_fd);
            return;
        }
        Sorting sorter(
            indexer.getInvertedIndex(),
            indexer.getDocumentLengths(),
            indexer.getTotalDocuments()
        );

        // Use either of these:
        std::vector<DocumentScore> rankedResults = sorter.rankByBM25(queryTokens);
        //std::vector<DocumentScore> rankedResults = sorter.rankByTFIDF(queryTokens);

        std::cout << "Results size: " << rankedResults.size() << std::endl;

        if (!rankedResults.empty()) {
            std::string html = "<html><body><h3>Search Results:</h3><ul>";
            for (const auto& result : rankedResults) {
                html += "<li><a href=\"/" + result.document + "\">" + result.document + "</a> (Score: " + std::to_string(result.score) + ")</li>";
            }
            html += "</ul></body></html>";
            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: " + std::to_string(html.size()) + "\r\n"
                "\r\n" + html;
            send(client_fd, response.c_str(), response.size(), 0);
        } else {
            // cache fallback
            std::string cached = cache_->get(query);
            if (!cached.empty()) {
                std::cout << "Cache hit for: " << query << std::endl;
                std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: " + std::to_string(cached.size()) + "\r\n"
                    "\r\n" + cached;
                send(client_fd, response.c_str(), response.size(), 0);
            } else {
                std::string fetchedHtml = googleFallback(query, client_fd);
                cache_->put(query, fetchedHtml);
            }
        }
    } else {
        std::string response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 9\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Not Found";
        send(client_fd, response.c_str(), response.size(), 0);
    }

    close(client_fd);
    std::cout << "Response sent to client.\n";
}

void RequestHandler::postRequestHandler(int clientSocket, const std::string& request) {
    std::cout << "Handling POST request.\n";
    std::cout << "Total request size: " << request.size() << " bytes\n";

    // Check Content-Length header
    size_t contentLengthPos = request.find("Content-Length: ");
    if (contentLengthPos != std::string::npos) {
        size_t lengthStart = contentLengthPos + 16; // Length of "Content-Length: "
        size_t lengthEnd = request.find("\r\n", lengthStart);
        std::string lengthStr = request.substr(lengthStart, lengthEnd - lengthStart);
        long expectedLength = std::stol(lengthStr);
        std::cout << "Expected Content-Length: " << expectedLength << " bytes\n";
    }

    // Find boundary from Content-Type
    std::string boundaryPrefix = "boundary=";
    size_t boundaryPos = request.find(boundaryPrefix);
    if (boundaryPos == std::string::npos) {
        std::cerr << "Boundary not found.\n";
        std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nBoundary not found in Content-Type\n";
        send(clientSocket, response.c_str(), response.size(), 0);
        return;
    }

    // Extract boundary value (remove any trailing characters like \r\n)
    std::string boundaryValue = request.substr(boundaryPos + boundaryPrefix.length());
    size_t boundaryEnd = boundaryValue.find_first_of("\r\n");
    if (boundaryEnd != std::string::npos) {
        boundaryValue = boundaryValue.substr(0, boundaryEnd);
    }
    std::string boundary = "--" + boundaryValue;
    std::cout << "Boundary: [" << boundary << "]\n";
    
    size_t endOfHeaders = request.find("\r\n\r\n");
    if (endOfHeaders == std::string::npos) {
        std::cerr << "End of headers not found.\n";
        std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nMalformed request headers\n";
        send(clientSocket, response.c_str(), response.size(), 0);
        return;
    }

    std::string body = request.substr(endOfHeaders + 4);
    std::cout << "Body length: " << body.length() << " bytes\n";
    
    // Don't print the entire body for large files, just show a preview
    if (body.length() > 200) {
        std::cout << "Body preview (first 100 chars): [" << body.substr(0, 100) << "...]\n";
        std::cout << "Body preview (last 100 chars): [..." << body.substr(body.length() - 100) << "]\n";
    } else {
        std::cout << "Body content: [" << body << "]\n";
    }
    
    if (body.empty()) {
        std::cerr << "Empty request body received. This might be due to incomplete request transmission.\n";
        std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nEmpty request body. Make sure the file exists and is readable.\n";
        send(clientSocket, response.c_str(), response.size(), 0);
        return;
    }
    
    // Check if this is a file upload with filename
    size_t fileStart = body.find("filename=");
    std::string filename;
    std::string fileContent;
    std::string folder;
    
    if (fileStart != std::string::npos) {
        // Handle file upload with filename
        std::cout << "Processing file upload with filename.\n";
        
        // Extract filename
        size_t startQuote = body.find("\"", fileStart);
        size_t endQuote = body.find("\"", startQuote + 1);
        filename = body.substr(startQuote + 1, endQuote - startQuote - 1);

        // Locate the beginning of the file data
        size_t contentStart = body.find("\r\n\r\n", endQuote);
        if (contentStart == std::string::npos) {
            std::cerr << "File content start not found.\n";
            return;
        }
        contentStart += 4;

        size_t contentEnd = body.find("\r\n" + boundary, contentStart);
        if (contentEnd == std::string::npos) {
            contentEnd = body.find(boundary, contentStart);
        }
        if (contentEnd != std::string::npos) {
            fileContent = body.substr(contentStart, contentEnd - contentStart);
            // Remove any trailing \r\n
            while (!fileContent.empty() && (fileContent.back() == '\r' || fileContent.back() == '\n')) {
                fileContent.pop_back();
            }
        } else {
            fileContent = body.substr(contentStart);
        }

        // Decide folder based on extension
        if (filename.find(".txt") != std::string::npos)
            folder = "disk/TEXT/";
        else if (filename.find(".html") != std::string::npos)
            folder = "disk/HTML/";
        else if (filename.find(".mp4") != std::string::npos || 
                 filename.find(".avi") != std::string::npos || 
                 filename.find(".mov") != std::string::npos || 
                 filename.find(".mkv") != std::string::npos || 
                 filename.find(".wmv") != std::string::npos || 
                 filename.find(".flv") != std::string::npos || 
                 filename.find(".webm") != std::string::npos || 
                 filename.find(".m4v") != std::string::npos)
            folder = "disk/VIDEOS/";
        else if (filename.find("big_buck_bunny") != std::string::npos || 
                 filename.find("video") != std::string::npos ||
                 filename.find("movie") != std::string::npos) {
            // Handle video files without extensions
            folder = "disk/VIDEOS/";
            filename = filename + ".mp4"; // Add mp4 extension
            std::cout << "📹 Detected video file without extension, adding .mp4: " << filename << "\n";
        }
        else {
            std::cerr << "Unsupported file type: " << filename << "\n";
            std::cout << "Supported formats: .txt, .html, .mp4, .avi, .mov, .mkv, .wmv, .flv, .webm, .m4v\n";
            std::cout << "Or files containing 'big_buck_bunny', 'video', 'movie' (will be treated as MP4)\n";
            std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nUnsupported file type. Supported: .txt, .html, .mp4, .avi, .mov, .mkv, .wmv, .flv, .webm, .m4v\nOr files containing 'big_buck_bunny', 'video', 'movie'\n";
            send(clientSocket, response.c_str(), response.size(), 0);
            return;
        }
    } else {
        // Handle simple POST data without filename
        std::cout << "Processing POST data without filename.\n";
        
        // Look for form field name (e.g., name="data" or name="content")
        size_t nameStart = body.find("name=\"");
        if (nameStart != std::string::npos) {
            size_t nameEndQuote = body.find("\"", nameStart + 6);
            std::string fieldName = body.substr(nameStart + 6, nameEndQuote - nameStart - 6);
            std::cout << "Found field: " << fieldName << "\n";
            
            // Get content after the headers
            size_t contentStart = body.find("\r\n\r\n", nameEndQuote);
            if (contentStart != std::string::npos) {
                contentStart += 4;
                size_t contentEnd = body.find("\r\n" + boundary, contentStart);
                if (contentEnd == std::string::npos) {
                    contentEnd = body.find(boundary, contentStart);
                }
                if (contentEnd != std::string::npos) {
                    fileContent = body.substr(contentStart, contentEnd - contentStart);
                    // Remove any trailing \r\n
                    while (!fileContent.empty() && (fileContent.back() == '\r' || fileContent.back() == '\n')) {
                        fileContent.pop_back();
                    }
                } else {
                    fileContent = body.substr(contentStart);
                }
                
                // Generate a default filename based on content type or field name
                if (fieldName == "video" || fieldName == "videofile") {
                    filename = fieldName + "_" + std::to_string(time(nullptr)) + ".mp4";
                    folder = "disk/VIDEOS/";
                } else if (fieldName == "html" || fieldName == "webpage") {
                    filename = fieldName + "_" + std::to_string(time(nullptr)) + ".html";
                    folder = "disk/HTML/";
                } else {
                    filename = fieldName + "_" + std::to_string(time(nullptr)) + ".txt";
                    folder = "disk/TEXT/";
                }
            }
        } else {
            // Fallback: treat entire body as text content
            std::cout << "No form field found, treating as raw text.\n";
            fileContent = body;
            filename = "upload_" + std::to_string(time(nullptr)) + ".txt";
            folder = "disk/TEXT/";
        }
    }

    if (filename.empty() || fileContent.empty()) {
        std::cerr << "Failed to extract filename or content.\n";
        std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nInvalid upload data\n";
        send(clientSocket, response.c_str(), response.size(), 0);
        return;
    }

    std::string fullPath = folder + filename;
    
    // Determine if this is a binary file (video files should be written in binary mode)
    bool isBinary = (folder == "disk/VIDEOS/");
    
    std::ofstream outFile;
    if (isBinary) {
        outFile.open(fullPath, std::ios::binary);
        std::cout << "Opening video file in binary mode: " << fullPath << "\n";
    } else {
        outFile.open(fullPath);
        std::cout << "Opening text file: " << fullPath << "\n";
    }
    
    if (!outFile) {
        std::cerr << "Failed to open file for writing: " << fullPath << "\n";
        std::string response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nFailed to save file\n";
        send(clientSocket, response.c_str(), response.size(), 0);
        return;
    }

    outFile << fileContent;
    outFile.close();

    std::cout << "File saved: " << fullPath << "\n";
    std::cout << "File size: " << fileContent.size() << " bytes\n";
    std::cout << "Saved to folder: " << folder << "\n";
    
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nFile uploaded successfully!\nFilename: " + filename + "\nLocation: " + folder + "\nSize: " + std::to_string(fileContent.size()) + " bytes\n";
    send(clientSocket, response.c_str(), response.size(), 0);
}


void RequestHandler::handleClientRequest(int client_fd, const std::string& rawRequest,
                                         FileIndexer& indexer) {
    std::cout << "Request received:\n" << rawRequest << "\n";
    std::string method = urlMethod(rawRequest);
    std::cout << "Parsed URL Request: " << method << "\n";

    if( method == "GET" ) {
        size_t start = rawRequest.find(" ") + 1;
        size_t end = rawRequest.find(" HTTP/", start);
        if (end == std::string::npos) {
            std::cout << "Invalid request format.\n";
            std::string response =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 11\r\n"
                "\r\n"
                "Bad Request";
            send(client_fd, response.c_str(), response.size(), 0);
            close(client_fd);
            return;
        }
        std::string path = rawRequest.substr(start, end - start);
        std::cout << "Parsed path: " << path << std::endl;
        getRequestHandler(client_fd, indexer, path);
    }
        else if (method == "POST") {
            std::cout << "Handling POST request.\n";
            postRequestHandler(client_fd, rawRequest);
            close(client_fd);
        } else {
        std::cout << "Unsupported request method: " << method << "\n";
        std::string response =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 11\r\n"
            "\r\n"
            "Bad Request";
        send(client_fd, response.c_str(), response.size(), 0);
        close(client_fd);
    }
}
