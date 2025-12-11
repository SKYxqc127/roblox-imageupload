#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize.h"

// ---------------------------------------------------------------------
// Hash helper (simple)
// ---------------------------------------------------------------------
std::string simpleHash(const std::string& s) {
    unsigned long h = 5381;
    for (char c : s) h = ((h << 5) + h) + (unsigned char)c;
    return std::to_string(h);
}

// ---------------------------------------------------------------------
// Download helper
// ---------------------------------------------------------------------
static size_t CurlWriteToFile(void* contents, size_t size, size_t nmemb, void* userp) {
    std::ofstream* ofs = static_cast<std::ofstream*>(userp);
    ofs->write(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

bool downloadImage(const std::string& url, const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    ofs.close();

    if (res != CURLE_OK) return false;

    std::ifstream check(path, std::ios::binary | std::ios::ate);
    return check.is_open() && check.tellg() > 10;
}

// ---------------------------------------------------------------------
// Decode + resize
// ---------------------------------------------------------------------
std::string processImage(const std::string& srcPath, const std::string& cachePath, int resizeTo) {
    int w, h, ch;

    unsigned char* src = stbi_load(srcPath.c_str(), &w, &h, &ch, 3);
    if (!src) throw std::runtime_error("decode failed");

    std::vector<unsigned char> outbuf(resizeTo * resizeTo * 3);

    if (!stbir_resize_uint8(src, w, h, 0, outbuf.data(), resizeTo, resizeTo, 0, 3)) {
        stbi_image_free(src);
        throw std::runtime_error("resize failed");
    }

    stbi_image_free(src);

    // Save cache PNG
    std::ofstream out(cachePath, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("cannot write cache");

    // Write raw RGB (Roblox code expects raw pixels in JSON, not PNG)
    for (unsigned char c : outbuf) out.put(c);

    return cachePath;
}

// ---------------------------------------------------------------------
// Query parsing
// ---------------------------------------------------------------------
struct Query { std::string imageUrl; int resize = 64; };

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v;
            std::stringstream ss;
            ss << std::hex << s.substr(i + 1, 2);
            ss >> v;
            out.push_back((char)v);
            i += 2;
        } else if (s[i] == '+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

Query parseQuery(const std::string& req) {
    size_t a = req.find("GET ");
    size_t b = req.find(" HTTP/");
    std::string full = req.substr(a + 4, b - (a + 4));

    size_t qm = full.find('?');
    if (qm == std::string::npos) return {};

    std::string qs = full.substr(qm + 1);
    Query q;
    std::stringstream ss(qs);
    std::string kv;

    while (std::getline(ss, kv, '&')) {
        size_t eq = kv.find('=');
        std::string k = urlDecode(kv.substr(0, eq));
        std::string v = urlDecode(kv.substr(eq + 1));

        if (k == "url") q.imageUrl = v;
        else if (k == "resize") q.resize = std::stoi(v);
    }

    return q;
}

// ---------------------------------------------------------------------
// JSON response wrapper
// ---------------------------------------------------------------------
std::string jsonReply(const std::string& body) {
    std::stringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Content-Type: application/json\r\n"
       << "Connection: close\r\n"
       << "Content-Length: " << body.size()
       << "\r\n\r\n" << body;
    return ss.str();
}

// ---------------------------------------------------------------------
// MAIN server
// ---------------------------------------------------------------------
int main() {
    mkdir("/tmp/cache", 0777); // create cache folder

    int port = 8787;
    int server = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server, (sockaddr*)&addr, sizeof(addr));
    listen(server, 16);

    while (true) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;

        char buf[4096];
        int n = read(client, buf, 4095);
        if (n <= 0) { close(client); continue; }
        buf[n] = 0;

        try {
            Query q = parseQuery(buf);
            if (q.imageUrl.empty()) {
                std::string rep = jsonReply("{\"error\":\"no url\"}");
                write(client, rep.c_str(), rep.size());
                close(client);
                continue;
            }

            int size = std::max(8, std::min(256, q.resize));
            std::string key = simpleHash(q.imageUrl + "_" + std::to_string(size));
            std::string cachePath = "/tmp/cache/" + key + ".raw";

            // If cached → use it
            struct stat st;
            if (stat(cachePath.c_str(), &st) == 0) {
                std::string rep = jsonReply("{\"cache\":true,\"path\":\"" + cachePath + "\"}");
                write(client, rep.c_str(), rep.size());
                close(client);
                continue;
            }

            // Download fresh
            std::string tmp = "/tmp/dl.bin";
            if (!downloadImage(q.imageUrl, tmp))
                throw std::runtime_error("download failed");

            processImage(tmp, cachePath, size);

            std::string rep = jsonReply("{\"cache\":false,\"path\":\"" + cachePath + "\"}");
            write(client, rep.c_str(), rep.size());
        }
        catch (std::exception& e) {
            std::string rep = jsonReply(std::string("{\"error\":\"") + e.what() + "\"}");
            write(client, rep.c_str(), rep.size());
        }

        close(client);
    }

    return 0;
}
