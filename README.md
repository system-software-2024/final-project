# Final Project Benchmark Code

This is our sample code of HTTP/1.1 Server and TCP Echo Server with various I/O models. It is useful to compare the performance of differnet I/O model.

- The TCP Echo Server is based on the source code located at <https://github.com/frevib/io_uring-echo-server>.
- The HTTP/1.1 Server is based on the source code located at <https://github.com/shuveb/loti-examples>.

We tried hard to make sure that the server with different I/O models does the same thing to ensure a fair comparison. The HTTP/1.1 Server makes use of the same HTTP parser, picohttpparser, for different I/O models to ensure a fair comparison.
