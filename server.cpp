// ====================
//   Image Parser API
//   With Cache + Pixel JSON
// ====================

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <filesystem>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize.h"

namespace fs = std::filesystem;


// temp cache folder
const std::string CACHE_DIR = "/tmp/imgcache";


// ========== CURL download ==========
static size_t CurlWrite(void* contents, size_t size, size_t nmemb, void* userp) {
    std::vector<unsigned char>* buf = (std::vector<unsigned char>*)userp;
    buf->insert(buf->end(), (unsigned char*)contents, (unsigned char*)contents + size * nmemb);
    return size * nmemb;
}

bool downloadFile(const std::string& url, std::vector<unsigned char>& out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}


// ========== Convert image → pixel matrix & JSON ==========
std::string convertImageToJson(const std::vector<unsigned char>& data, int resize) {
    int w, h, ch;

    unsigned char* src = stbi_load_from_memory(
        data.data(), data.size(), &w, &h, &ch, 3
    );
    if (!src) throw std::runtime_error("decode failed");

    // Resize
    int t = resize;
    std::vector<unsigned char> out(t * t * 3);

    stbir_resize_uint8(src, w, h, 0, out.data(), t, t, 0, 3);
    stbi_image_free(src);

    // Build JSON string manually
    std::ostringstream json;

    json << "{";
    json << "\"width\":" << t << ",";
    json << "\"height\":" << t << ",";
    json << "\"pixels\":{";

    for (int y = 0; y < t; y++) {
        json << "\"" << (y + 1) << "\":{";

        for (int x = 0; x < t; x++) {
            int idx = (y * t + x) * 3;
            int r = out[idx];
            int g = out[idx + 1];
            int b = out[idx + 2];

            json << "\"" << (x + 1) << "\":[" << r << "," << g << "," << b << "]";
            if (x + 1 < t) json << ",";
        }

        json << "}";
        if (y + 1 < t) json << ",";
    }

    json << "}}";

    return json.str();
}


// ========== Cache helpers ==========
std::string urlToKey(const std::string& url) {
    std::hash<std::string> h;
    return std::to_string(h(url));
}

std::string getCachePath(const std::string& key) {
    return CACHE_DIR + "/" + key + ".json";
}

bool cacheExists(const std::string& key) {
    return fs::exists(getCachePath(key));
}

std::string readCache(const std::string& key) {
    std::ifstream f(getCachePath(key));
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void saveCache(const std::string& key, const std::string& json) {
    fs::create_directories(CACHE_DIR);
    std::ofstream f(getCachePath(key));
    f << json;
}


// ========== HTTP response helpers ==========
std::string httpJson(const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 200 OK\r\n"
      << "Content-Type: application/json\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n"
      << "Content-Length: " << body.size() << "\r\n\r\n"
      << body;
    return o.str();
}

std::string httpErr(const std::string& msg) {
    std::string b = "{\"error\":\"" + msg + "\"}";
    std::ostringstream o;
    o << "HTTP/1.1 500 Error\r\n"
      << "Content-Type: application/json\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n"
      << "Content-Length: " << b.size() << "\r\n\r\n"
      << b;
    return o.str();
}


// ========== MAIN SERVER ==========
int main() {
    int port = 8787;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;

    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server, (sockaddr*)&addr, sizeof(addr));
    listen(server, 10);

    std::cout << "Image API ready on :" << port << "\n";

    while (true) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;

        char buf[4096];
        int n = read(client, buf, sizeof(buf));
        if (n <= 0) {
            close(client);
            continue;
        }

        std::string req(buf, n);

        // parse ?url=
        size_t qs = req.find("?url=");
        if (qs == std::string::npos) {
            std::string msg = httpErr("missing url");
            write(client, msg.c_str(), msg.size());
            close(client);
            continue;
        }

        size_t end = req.find(" ", qs);
        std::string url = req.substr(qs + 5, end - (qs + 5));

        int resize = 64;

        // 1) CACHE CHECK
        std::string key = urlToKey(url);

        if (cacheExists(key)) {
            std::string body = readCache(key);
            std::string res = httpJson(body);
            write(client, res.c_str(), res.size());
            close(client);
            continue;
        }

        // 2) DOWNLOAD IMAGE
        std::vector<unsigned char> file;
        if (!downloadFile(url, file)) {
            std::string res = httpErr("download failed");
            write(client, res.c_str(), res.size());
            close(client);
            continue;
        }

        // 3) CONVERT TO JSON
        try {
            std::string json = convertImageToJson(file, resize);

            // save cache
            saveCache(key, json);

            std::string res = httpJson(json);
            write(client, res.c_str(), res.size());
        }
        catch (std::exception& e) {
            std::string res = httpErr(e.what());
            write(client, res.c_str(), res.size());
        }

        close(client);
    }

    return 0;
}
