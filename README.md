# HTTP_Server

An asynchronous web server with support for **static** and **dynamic** file sharing over **TCP**.

## Technical Details

The server uses *I/O multiplexing* with **epoll** as an alternative to the traditional multi-threaded or multi-process client-server implementations. When the HTTP request header is received, it's firstly run through a parser which splits it up into blocks that are easily readable (e.g. REQUEST TYPE, PATH).

The files in the **/src/static** directory are transmitted to the client using *zero-copying* with the **sendfile** function.

**Dynamic** files require further post-processing; they are read from disk using an *asynchronous API*, then streamed to the clients using **non-blocking** sockets.

## Installation and Running

```sh
git clone https://github.com/amir-FM/HTTP_Server [installation directory]
cd [installation directory]
cd src
make
./aws

# Running
# Works with any http client
# Note: you need to create src/static and src/dynamic directories and populate them with files
wget http://localhost:8888/[static/dynamic]/[filename]
```
