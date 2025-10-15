# roblox-imageproxy

C++ (libcurl + stb_image) web service that converts an image URL to a pixel matrix JSON for Roblox `EditableImage`.

## Endpoint
```
GET /?url=<url-encoded-image-url>&resize=32
```
- `resize` is clamped to 8..256 (default 64)
- CORS enabled
- Supports `OPTIONS` preflight

## Deploy on Render
1. Push this repo to GitHub (`SKYxqc127/roblox-imageupload` or any repo you choose).
2. On Render: **New → Web Service → Connect repo → Environment: Docker**.
3. Deploy. After success, you'll get a URL like:
```
https://roblox-imageproxy.onrender.com
```
Test:
```
https://roblox-imageproxy.onrender.com/?url=https%3A%2F%2Fi.imgur.com%2Fabcd.png&resize=32
```

## Local (Docker)
```bash
docker build -t roblox-imageproxy .
docker run -p 8787:8787 roblox-imageproxy
curl "http://localhost:8787/?url=https%3A%2F%2Fi.imgur.com%2Fabcd.png&resize=32"
```

## Roblox Module integration
Set your API URL to the Render URL:
```lua
local API_URL = "https://roblox-imageproxy.onrender.com" -- change to your Render URL
```
Then call your existing `ParseImageToEditableImage(url, resize)`.
