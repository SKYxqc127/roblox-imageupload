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

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize.h"

struct ImageData {
    int width;
    int height;
    std::vector<std::vector<std::vector<uint8_t>>> pixels;
};

// -----------------------------------------------------------------------------
// Download helper
// -----------------------------------------------------------------------------
static size_t CurlWriteToFile(void* contents, size_t size, size_t nmemb, void* userp) {
    std::ofstream* ofs = static_cast<std::ofstream*>(userp);
    ofs->write(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

bool downloadImage(const std::string& url, const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "❌ curl init failed\n";
        return false;
    }

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "❌ cannot open file: " << path << "\n";
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

    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"
        " AppleWebKit/537.36 (KHTML, like Gecko)"
        " Chrome/123.0 Safari/537.36");

    // Fix SSL issues on Render
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    ofs.close();

    if (res != CURLE_OK) {
        std::cerr << "❌ curl error: " << curl_easy_strerror(res) << "\n";
        return false;
    }

    std::ifstream check(path, std::ios::binary | std::ios::ate);
    if (!check.is_open() || check.tellg() <= 10) {
        std::cerr << "❌ empty/invalid download\n";
        return false;
    }

    std::cout << "✅ Download OK\n";
    return true;
}

// -----------------------------------------------------------------------------
// URL decode
// -----------------------------------------------------------------------------
static std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v = 0;
            std::istringstream hex(s.substr(i + 1, 2));
            if (hex >> std::hex >> v) {
                out.push_back((char)v);
                i += 2;
            }
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Query
// -----------------------------------------------------------------------------
struct Query {
    std::string imageUrl;
    int resize = 64;
};

static Query parseQuery(const std::string& req) {
    size_t lineEnd = req.find("\r\n");
    std::string first = req.substr(0, lineEnd);

    size_t a = first.find(' ');
    size_t b = first.rfind(' ');

    if (a == std::string::npos || b == std::string::npos)
        throw std::runtime_error("bad request");

    std::string path = first.substr(a + 1, b - a - 1);

    Query q;
    size_t qm = path.find('?');
    if (qm == std::string::npos) return q;

    std::string qs = path.substr(qm + 1);
    std::istringstream iss(qs);
    std::string kv;

    while (std::getline(iss, kv, '&')) {
        size_t eq = kv.find('=');
        std::string k = urlDecode(kv.substr(0, eq));
        std::string v = urlDecode(kv.substr(eq + 1));

        if (k == "url") q.imageUrl = v;
        else if (k == "resize") {
            try { q.resize = std::max(8, std::min(256, std::stoi(v))); } catch (...) {}
        }
    }

    return q;
}

// -----------------------------------------------------------------------------
// JSON response
// -----------------------------------------------------------------------------
static std::string httpOkJson(const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 200 OK\r\n"
      << "Content-Type: application/json\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n"
      << "Content-Length: " << body.size() << "\r\n\r\n"
      << body;
    return o.str();
}

static std::string httpErrJson(int code, const std::string& msg) {
    std::string b = "{\"error\":\"" + msg + "\"}";
    std::ostringstream o;
    o << "HTTP/1.1 " << code << " Error\r\n"
      << "Content-Type: application/json\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n"
      << "Content-Length: " << b.size() << "\r\n\r\n"
      << b;
    return o.str();
}

// -----------------------------------------------------------------------------
// Load + Resize
// -----------------------------------------------------------------------------
ImageData loadImageToMatrix(const std::string& url, int resize) {
    // --- allow only discord (security)
    if (!(url.rfind("https://cdn.discordapp.com/", 0) == 0 ||
          url.rfind("https://media.discordapp.net/", 0) == 0)) {
        throw std::runtime_error("Only Discord image URLs allowed");
    }

    // normalize format
    std::string fixed = url;
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("format=webp") != std::string::npos) {
        size_t pos = fixed.find("format=webp");
        if (pos != std::string::npos)
            fixed.replace(pos, strlen("format=webp"), "format=png");
    }

    // download
    std::string tmp = "/tmp/image.bin";
    if (!downloadImage(fixed, tmp))
        throw std::runtime_error("download failed");

    // decode
    int w, h, ch;
    unsigned char* src = stbi_load(tmp.c_str(), &w, &h, &ch, 3);
    if (!src) throw std::runtime_error("stbi_load failed");

    int target = std::max(8, std::min(256, resize));
    std::vector<unsigned char> outbuf(target * target * 3);

    if (!stbir_resize_uint8(src, w, h, 0, outbuf.data(), target, target, 0, 3)) {
        stbi_image_free(src);
        throw std::runtime_error("resize failed");
    }

    stbi_image_free(src);

    // build matrix
    ImageData img;
    img.width = target;
    img.height = target;
    img.pixels.resize(target, std::vector<std::vector<uint8_t>>(target, std::vector<uint8_t>(3)));

    for (int y = 0; y < target; ++y) {
        for (int x = 0; x < target; ++x) {
            int idx = (y * target + x) * 3;
            img.pixels[y][x][0] = outbuf[idx + 0];
            img.pixels[y][x][1] = outbuf[idx + 1];
            img.pixels[y][x][2] = outbuf[idx + 2];
        }
    }

    return img;
}

// -----------------------------------------------------------------------------
// JSON encode
// -----------------------------------------------------------------------------
std::string toJson(const ImageData& img) {
    std::ostringstream s;
    s << "{\"width\":" << img.width
      << ",\"height\":" << img.height
      << ",\"pixels\":[";

    for (int y = 0; y < img.height; ++y) {
        s << "[";
        for (int x = 0; x < img.width; ++x) {
            auto& p = img.pixels[y][x];
            s << "[" << (int)p[0] << "," << (int)p[1] << "," << (int)p[2] << "]";
            if (x + 1 < img.width) s << ",";
        }
        s << "]";
        if (y + 1 < img.height) s << ",";
    }

    s << "]}";
    return s.str();
}

// -----------------------------------------------------------------------------
// Main HTTP server
// -----------------------------------------------------------------------------
int main() {
    int port = 8787;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return 1;

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0) return 1;
    if (listen(server, 16) < 0) return 1;

    std::cout << "Listening on :" << port << "\n";

    while (true) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;

        char buf[8192];
        int n = read(client, buf, sizeof(buf)-1);
        if (n <= 0) { close(client); continue; }
        buf[n] = '\0';

        std::string req(buf);

        // CORS preflight
        if (req.rfind("OPTIONS", 0) == 0) {
            std::string res =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Content-Length: 0\r\n\r\n";
            write(client, res.c_str(), res.size());
            close(client);
            continue;
        }

        try {
            Query q = parseQuery(req);

            if (q.imageUrl.empty()) {
                std::string res = httpOkJson("{\"message\":\"Use /?url=<encoded>&resize=64\"}");
                write(client, res.c_str(), res.size());
                close(client);
                continue;
            }

            ImageData img = loadImageToMatrix(q.imageUrl, q.resize);
            std::string body = toJson(img);
            std::string res = httpOkJson(body);
            write(client, res.c_str(), res.size());
        }
        catch (const std::exception& e) {
            std::string res = httpErrJson(500, e.what());
            write(client, res.c_str(), res.size());
        }

        close(client);
    }

    return 0;
}
