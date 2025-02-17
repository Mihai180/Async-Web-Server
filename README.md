# Asynchronous Web Server

## Overview

This project is an implementation of an asynchronous web server designed to handle multiple client connections efficiently. The server leverages advanced I/O operations, including non-blocking sockets, zero-copy techniques, and asynchronous file handling, making it a high-performance solution for serving static and dynamically generated files over HTTP.

## Features

- **Multiplexed I/O Handling**: Uses the `epoll` API for efficient event-driven networking.
- **Non-Blocking Sockets**: Manages multiple connections concurrently without blocking operations.
- **Zero-Copy File Transfer**: Uses `sendfile` to transfer static files efficiently.
- **Asynchronous File I/O**: Reads dynamic content using asynchronous APIs to optimize performance.
- **HTTP Protocol Support**:
  - Serves files from a predefined root directory.
  - Returns `200 OK` for valid requests and `404 Not Found` for missing files.
  - Properly formats HTTP response headers with required directives (`Content-Length`, `Connection: close`).
- **State Machine for Connection Handling**: Manages client connections and tracks their state throughout the request-response cycle.
- **Error Handling**: Ensures graceful failure recovery with informative logging.

## Repository Structure

- `src/` - Core implementation of the web server.
- `skel/` - Skeleton code and necessary headers for implementation.
- `tests/` - Automated test suite to validate correctness and performance.

## Why This Project?

This project demonstrates expertise in network programming, asynchronous I/O, and high-performance system design. It showcases:

- Efficient handling of concurrent network connections.
- Practical use of Linux system calls for non-blocking and zero-copy operations.
- Implementation of a custom HTTP server with proper request processing.
- Strong problem-solving skills in designing scalable and efficient applications.

By working on this project, I have gained in-depth knowledge of system-level programming, socket management, and performance optimization techniques essential for developing high-performance network applications.

If you're looking for a developer with experience in networking and system programming, let's connect!

## License
This project is licensed under the **MIT License**. See `LICENSE` for details.
