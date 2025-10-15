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

// stb headers will be fetched during Docker build into third_party/stb/
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize.h" // legacy API (stable)

struct ImageData {
    int width;
    int height;
    std::vector<std::vector<std::vector<uint8_t>>> pixels;
};

// ---- helpers ----
static size_t CurlWriteToFile(void* contents, size_t size, size_t nmemb, void* userp) {
    std::ofstream* ofs = static_cast<std::ofstream*>(userp);
    ofs->write(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

bool downloadImage(const std::string& url, const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) { curl_easy_cleanup(curl); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RobloxImageProxy/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    ofs.close();
    return (res == CURLE_OK);
}

static std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v = 0;
            std::istringstream iss(s.substr(i + 1, 2));
            if (iss >> std::hex >> v) {
                out.push_back(static_cast<char>(v));
                i += 2;
            } else {
                out.push_back(s[i]);
            }
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

struct Query { std::string imageUrl; int resize = 64; };

static Query parseQuery(const std::string& req) {
    size_t lineEnd = req.find("\r\n");
    std::string firstLine = (lineEnd == std::string::npos) ? req : req.substr(0, lineEnd);
    size_t a = firstLine.find(' ');
    size_t b = firstLine.rfind(' ');
    if (a == std::string::npos || b == std::string::npos || b <= a) throw std::runtime_error("bad request");
    std::string path = firstLine.substr(a + 1, b - a - 1);

    Query q;
    size_t qm = path.find('?');
    if (qm == std::string::npos) return q;
    std::string qs = path.substr(qm + 1);

    std::istringstream iss(qs);
    std::string kv;
    while (std::getline(iss, kv, '&')) {
        size_t eq = kv.find('=');
        std::string k = (eq==std::string::npos) ? kv : kv.substr(0,eq);
        std::string v = (eq==std::string::npos) ? ""  : kv.substr(eq+1);
        k = urlDecode(k);
        v = urlDecode(v);
        if (k == "url") q.imageUrl = v;
        else if (k == "resize") {
            try { q.resize = std::max(8, std::min(256, std::stoi(v))); } catch (...) {}
        }
    }
    return q;
}

static std::string httpOkJson(const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: application/json\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    return oss.str();
}

static std::string httpErrJson(int code, const std::string& msg) {
    std::ostringstream body;
    body << "{\"error\":\"" << msg << "\"}";
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " Error\r\n"
        << "Content-Type: application/json\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.str().size() << "\r\n\r\n"
        << body.str();
    return oss.str();
}

ImageData loadImageToMatrix(const std::string& url, int resize) {
    std::string tmp = "/tmp/image.bin";
    if (!downloadImage(url, tmp)) throw std::runtime_error("download failed");

    int w,h,c;
    unsigned char* src = stbi_load(tmp.c_str(), &w, &h, &c, 3);
    if (!src) throw std::runtime_error("stbi_load failed");

    int target = resize > 0 ? resize : std::min(w,h);
    target = std::max(8, std::min(256, target));

    std::vector<unsigned char> pixels;
    if (w != target || h != target) {
        pixels.resize(target * target * 3);
        // legacy stb_image_resize API: specify num_channels=3
        int ok = stbir_resize_uint8(src, w, h, 0, pixels.data(), target, target, 0, 3);
        stbi_image_free(src);
        if (!ok) throw std::runtime_error("resize failed");
    } else {
        pixels.assign(src, src + w*h*3);
        stbi_image_free(src);
        target = w;
    }

    ImageData out;
    out.width = target;
    out.height = target;
    out.pixels.resize(target, std::vector<std::vector<uint8_t>>(target, std::vector<uint8_t>(3)));
    for (int y=0; y<target; ++y) {
        for (int x=0; x<target; ++x) {
            out.pixels[y][x][0] = pixels[(y*target + x)*3 + 0];
            out.pixels[y][x][1] = pixels[(y*target + x)*3 + 1];
            out.pixels[y][x][2] = pixels[(y*target + x)*3 + 2];
        }
    }
    return out;
}

std::string toJson(const ImageData& img) {
    std::ostringstream s;
    s << "{\"width\":" << img.width << ",\"height\":" << img.height << ",\"pixels\":[";
    for (int y=0; y<img.height; ++y) {
        s << "[";
        for (int x=0; x<img.width; ++x) {
            const auto &p = img.pixels[y][x];
            s << "[" << (int)p[0] << "," << (int)p[1] << "," << (int)p[2] << "]";
            if (x<img.width-1) s << ",";
        }
        s << "]";
        if (y<img.height-1) s << ",";
    }
    s << "]}";
    return s.str();
}

int main() {
    int port = 8787;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { std::cerr << "socket failed\n"; return 1; }

    int opt=1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) { std::cerr << "bind failed\n"; return 1; }
    if (listen(sockfd, 16) < 0) { std::cerr << "listen failed\n"; return 1; }

    std::cout << "Listening on :" << port << std::endl;

    while (true) {
        int client = accept(sockfd, nullptr, nullptr);
        if (client < 0) continue;

        char buf[8192];
        int n = read(client, buf, sizeof(buf)-1);
        if (n <= 0) { close(client); continue; }
        buf[n] = '\0';
        std::string req(buf);

        if (req.rfind("OPTIONS ", 0) == 0) {
            std::string res =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
            ssize_t _ = write(client, res.c_str(), res.size()); (void)_;
            close(client);
            continue;
        }

        try {
            Query q = parseQuery(req);
            if (q.imageUrl.empty()) {
                std::string body = "{\"message\":\"Use /?url=<encoded URL>&resize=32\"}";
                std::string res = httpOkJson(body);
                ssize_t _ = write(client, res.c_str(), res.size()); (void)_;
                close(client);
                continue;
            }
            auto img = loadImageToMatrix(q.imageUrl, q.resize);
            std::string body = toJson(img);
            std::string res = httpOkJson(body);
            ssize_t _ = write(client, res.c_str(), res.size()); (void)_;
        } catch (const std::exception& e) {
            std::string res = httpErrJson(500, e.what());
            ssize_t _ = write(client, res.c_str(), res.size()); (void)_;
        }

        close(client);
    }
    return 0;
}
