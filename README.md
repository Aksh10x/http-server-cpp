# C++ HTTP/1.1 Server

A lightweight, concurrent HTTP/1.1 server implementation written in C++17. This is a C++ port of a Go implementation that interacts directly with the TCP layer, handling raw byte streams to parse HTTP requests and generate compliant responses without relying on any high-level HTTP framework. It serves static files, echoes request data, and handles concurrent client connections via threads.

## Capabilities

The server listens on port `4221` and implements the following endpoints and behaviors:

- **`/`**: Returns a standard 200 OK status.
- **`/echo/{string}`**: Returns the path parameter as the response body.
  - Supports [Gzip compression](https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Encoding) if the `Accept-Encoding: gzip` header is present.
- **`/user-agent`**: Reads and returns the value of the `User-Agent` header from the client request.
- **`/files/{filename}`**:
  - `GET`: Reads the specified file from the server's configured directory and returns it with `application/octet-stream`.
  - `POST`: Creates a new file (or overwrites an existing one) with the request body data.
- **Persistent Connections**: Supports HTTP Keep-Alive by processing multiple requests on a single connection until a `Connection: close` header is detected.
- **Concurrency**: Spawns a new thread per accepted connection so the listener loop never blocks.

## Building

Requires a C++17 compiler and `zlib` (present by default on macOS/Linux).

```sh
make
```

This produces an `http-server` binary in the project root.

## Running

```sh
./http-server --directory /path/to/serve
```

- `--directory` (optional, default `./`): directory used to serve/store files for the `/files/{filename}` endpoint.

## Trying it out

```sh
./http-server --directory /tmp &

curl http://localhost:4221/
curl http://localhost:4221/echo/hello
curl -H "Accept-Encoding: gzip" http://localhost:4221/echo/hello --output - | gunzip
curl -A "my-client/1.0" http://localhost:4221/user-agent
curl -X POST --data "hello world" http://localhost:4221/files/test.txt
curl http://localhost:4221/files/test.txt
```

## Implementation Notes

- **Raw TCP Socket Management**: Uses POSIX sockets (`socket`, `bind`, `listen`, `accept`) directly rather than a higher-level HTTP library.
- **Manual Protocol Parsing**: A small buffered-reader class wraps each connection's file descriptor, reading the request line and headers line-by-line, then reading exactly `Content-Length` bytes for the body.
- **Compression Negotiation**: Checks `Accept-Encoding` for `gzip` and compresses response bodies on the fly using `zlib`'s gzip framing (`deflateInit2` with `windowBits = 15 | 16`).
- **Threading**: Each accepted connection is handled on its own detached `std::thread`.
- **SIGPIPE handling**: `SIGPIPE` is ignored so that writing to a peer that has already disconnected doesn't kill the process.

## Project Structure

```text
.
├── Makefile
└── src/
    └── main.cpp
```
