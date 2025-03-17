# Web Image Scraper

## $TETSUO on Solana

**Contract Address**: `8i51XNNpGaKaj4G4nDdmQh95v4FKAxw8mhtaRoKd9tE8`

[![Twitter](https://img.shields.io/badge/Twitter-Follow%20%407etsuo-1DA1F2)](https://x.com/7etsuo)
[![Discord](https://img.shields.io/badge/Discord-Join%20Our%20Community-7289DA)](https://discord.gg/tetsuo-ai)

---

A fast, multi-threaded utility to extract and download all images from a webpage.

![image](https://github.com/user-attachments/assets/9339e9e9-d41f-4203-9489-5d5cdb5d0459)

## Features

- Extracts all image URLs from a target webpage
- Resolves relative URLs to absolute URLs
- Avoids duplicate downloads with O(1) lookup
- Uses multiple threads for parallel downloading
- Preserves original file extensions when possible

## Requirements

- libcurl (HTTP requests)
- libxml2 (HTML parsing)
- POSIX threads

## Installation

### Ubuntu/Debian
```bash
sudo apt-get install libcurl4-openssl-dev libxml2-dev
```

### Fedora/RHEL/CentOS
```bash
sudo dnf install libcurl-devel libxml2-devel
```

### macOS (with Homebrew)
```bash
brew install curl libxml2
```

## Compilation

```bash
gcc -o webscraper webscraper.c $(curl-config --cflags --libs) $(xml2-config --cflags --libs) -pthread
```

## Usage

```bash
./webscraper <url>
```

Example:
```bash
./webscraper https://example.com
```

Downloaded images will be saved in the `downloaded_images` directory.

## License

MIT
