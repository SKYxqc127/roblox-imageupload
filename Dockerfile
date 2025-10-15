FROM ubuntu:22.04

RUN apt-get update && apt-get install -y g++ curl libcurl4-openssl-dev && rm -rf /var/lib/apt/lists/*

WORKDIR /app
# Fetch stb headers (legacy resize header)
RUN mkdir -p third_party/stb && \
    curl -L -o third_party/stb/stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h && \
    curl -L -o third_party/stb/stb_image_resize.h https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize.h

COPY . .

# Ignore write() unused-result warnings to avoid failing builds
RUN g++ -std=gnu++17 -O2 -pthread -Wno-unused-result server.cpp -lcurl -o server

EXPOSE 8787
CMD ["./server"]
