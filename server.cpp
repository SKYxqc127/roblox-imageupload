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
#include <sys/types.h>

// stb headers
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize.h"

// ===== MD5 header-only =====
#include <iomanip>
#include <cstdint>
#include <array>

struct MD5 {
    std::array<uint32_t, 4> h;
    static constexpr std::array<uint32_t, 64> k = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };

    static std::string digest(const std::string &input) {
        // Simplified MD5 implementation (public domain)
        // For brevity, using std::hash as placeholder (real MD5 library recommended)
        std::hash<std::string> hasher;
        size_t h = hasher(input);
        std::ostringstream oss;
        oss << std::hex << std::setw(16) << std::setfill('0') << h;
        return oss.str();
    }
};

// ---- helpers ----
struct ImageData {
    int width;
    int height;
    std::vector<std::vector<std::vector<uint8_t>>> pixels;
};

// ---- Cache helper ----
void ensureCacheDir() {
    struct stat st = {0};
    if (stat("/tmp/cache", &st) == -1) {
        mkdir("/tmp/cache", 0700);
        std::cout << "📁 Created /tmp/cache" << std::endl;
    }
}

std::string getCachePath(const std::string& url) {
    std::string name = MD5::digest(url) + ".bin";
    return "/tmp/cache/" + name;
}

// ---- curl helper ----
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
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    ofs.close();
    if (res != CURLE_OK) return false;

    std::ifstream check(path,std::ios::binary|std::ios::ate);
    if(!check.is_open() || check.tellg()==0) { check.close(); return false; }
    check.close();
    return true;
}

// ---- URL decode ----
static std::string urlDecode(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v = 0; std::istringstream iss(s.substr(i + 1, 2));
            if (iss >> std::hex >> v) { out.push_back(static_cast<char>(v)); i+=2; } else out.push_back(s[i]);
        } else if (s[i] == '+') out.push_back(' '); else out.push_back(s[i]);
    }
    return out;
}

// ---- HTTP helpers ----
struct Query { std::string imageUrl; int resize = 64; };

static Query parseQuery(const std::string& req) {
    size_t lineEnd = req.find("\r\n");
    std::string firstLine = (lineEnd == std::string::npos) ? req : req.substr(0, lineEnd);
    size_t a = firstLine.find(' ');
    size_t b = firstLine.rfind(' ');
    if (a == std::string::npos || b == std::string::npos || b <= a) throw std::runtime_error("bad request");
    std::string path = firstLine.substr(a+1, b-a-1);

    Query q; size_t qm = path.find('?'); if(qm==std::string::npos) return q;
    std::string qs = path.substr(qm+1);

    std::istringstream iss(qs); std::string kv;
    while (std::getline(iss, kv, '&')) {
        size_t eq = kv.find('=');
        std::string k = (eq==std::string::npos)?kv:kv.substr(0,eq);
        std::string v = (eq==std::string::npos)?"":kv.substr(eq+1);
        k = urlDecode(k); v = urlDecode(v);
        if(k=="url") q.imageUrl=v;
        else if(k=="resize") { try{ q.resize = std::max(8,std::min(256,std::stoi(v))); }catch(...){} }
    }
    return q;
}

static std::string httpOkJson(const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: " << body.size() << "\r\n\r\n" << body;
    return oss.str();
}

static std::string httpErrJson(int code, const std::string& msg) {
    std::ostringstream body; body << "{\"error\":\"" << msg << "\"}";
    std::ostringstream oss; oss << "HTTP/1.1 " << code << " Error\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: " << body.str().size() << "\r\n\r\n" << body.str();
    return oss.str();
}

// ---- Image loader ----
ImageData loadImageToMatrix(const std::string& url, int resize) {
    if(!(url.rfind("https://cdn.discordapp.com/",0)==0||url.rfind("https://media.discordapp.net/",0)==0))
        throw std::runtime_error("Only Discord image links allowed");

    std::string lower = url; std::transform(lower.begin(),lower.end(),lower.begin(),::tolower);
    if(!(lower.find(".png")!=std::string::npos || lower.find(".jpg")!=std::string::npos || lower.find(".jpeg")!=std::string::npos || lower.find("format=webp")!=std::string::npos))
        throw std::runtime_error("Only PNG/JPG/WebP allowed");

    std::string fixedUrl = url;
    size_t pos = fixedUrl.find("format=webp"); if(pos!=std::string::npos) fixedUrl.replace(pos,11,"format=png");

    ensureCacheDir();
    std::string cachePath = getCachePath(fixedUrl);
    std::string tmp;

    std::ifstream test(cachePath,std::ios::binary);
    if(test.good()){ std::cout<<"📀 Cache hit\n"; test.close(); tmp=cachePath; }
    else { if(!downloadImage(fixedUrl, cachePath)) throw std::runtime_error("download failed"); tmp=cachePath; }

    int w,h,c; unsigned char* src=stbi_load(tmp.c_str(),&w,&h,&c,3); if(!src) throw std::runtime_error("stbi_load failed");
    int target = resize>0?resize:std::min(w,h); target = std::max(8,std::min(256,target));
    std::vector<unsigned char> pixels;
    if(w!=target||h!=target){ pixels.resize(target*target*3); int ok=stbir_resize_uint8(src,w,h,0,pixels.data(),target,target,0,3); stbi_image_free(src); if(!ok) throw std::runtime_error("resize failed"); }
    else { pixels.assign(src,src+w*h*3); stbi_image_free(src); target=w; }

    ImageData out; out.width=target; out.height=target; out.pixels.resize(target,std::vector<std::vector<uint8_t>>(target,std::vector<uint8_t>(3)));
    for(int y=0;y<target;++y) for(int x=0;x<target;++x){
        out.pixels[y][x][0]=pixels[(y*target+x)*3+0];
        out.pixels[y][x][1]=pixels[(y*target+x)*3+1];
        out.pixels[y][x][2]=pixels[(y*target+x)*3+2];
    }
    return out;
}

std::string toJson(const ImageData& img){
    std::ostringstream s; s<<"{\"width\":"<<img.width<<",\"height\":"<<img.height<<",\"pixels\":[";
    for(int y=0;y<img.height;++y){ s<<"["; for(int x=0;x<img.width;++x){ const auto &p=img.pixels[y][x]; s<<"["<<(int)p[0]<<","<<(int)p[1]<<","<<(int)p[2]<<"]"; if(x<img.width-1)s<<","; } s<<"]"; if(y<img.height-1)s<<","; } s<<"]"; return s.str();
}

// ---- main server ----
int main(){
    ensureCacheDir();

    int port=8787;
    int sockfd=socket(AF_INET,SOCK_STREAM,0); if(sockfd<0) return 1;
    int opt=1; setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(port);
    if(bind(sockfd,(sockaddr*)&addr,sizeof(addr))<0) return 1;
    if(listen(sockfd,16)<0) return 1;
    std::cout<<"Listening on :"<<port<<std::endl;

    while(true){
        int client = accept(sockfd,nullptr,nullptr); if(client<0) continue;
        char buf[8192]; int n=read(client,buf,sizeof(buf)-1); if(n<=0){close(client); continue;} buf[n]='\0'; std::string req(buf);

        if(req.rfind("OPTIONS ",0)==0){
            std::string res="HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            write(client,res.c_str(),res.size()); close(client); continue;
        }

        try{
            Query q=parseQuery(req);
            if(q.imageUrl.empty()){ std::string body="{\"message\":\"Use /?url=<encoded URL>&resize=32\"}"; std::string res=httpOkJson(body); write(client,res.c_str(),res.size()); close(client); continue;}
            auto img=loadImageToMatrix(q.imageUrl,q.resize);
            std::string body=toJson(img); std::string res=httpOkJson(body); write(client,res.c_str(),res.size());
        }catch(const std::exception& e){
            std::string res=httpErrJson(500,e.what()); write(client,res.c_str(),res.size());
        }
        close(client);
    }
    return 0;
}
