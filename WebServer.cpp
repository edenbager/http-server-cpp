// Single-threaded HTTP/1.1 server built on non-blocking sockets and select().
// Supports GET, HEAD, POST, PUT, DELETE, OPTIONS and TRACE.
// Builds with Winsock on Windows and with POSIX sockets on macOS / Linux.

#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "Ws2_32.lib")
    #endif
    typedef int socklen_t;
    const char PATH_SEP = '\\';
#else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <signal.h>
    typedef int SOCKET;
    const SOCKET INVALID_SOCKET = -1;
    const int SOCKET_ERROR = -1;
    const int WSAEWOULDBLOCK = EWOULDBLOCK;
    inline int closesocket(SOCKET s) { return close(s); }
    inline int WSAGetLastError() { return errno; }
    const char PATH_SEP = '/';
#endif

using namespace std;

// ─── Constants ────────────────────────────────────────────────────────────────
const int HTTP_PORT     = 8080;
const int MAX_SOCKETS   = 60;
const int BUFFER_SIZE   = 64 * 1024;  // largest request (headers + body) we accept
const int TIMEOUT_SEC   = 120;        // close idle connections after 2 minutes
string rootDir          = "www";      // folder the files are served from (argv[1] overrides)

// Socket states
const int EMPTY   = 0;
const int LISTEN  = 1;
const int RECEIVE = 2;
const int SEND    = 3;
const int IDLE    = 4;

// ─── SocketState ──────────────────────────────────────────────────────────────
struct SocketState
{
    SOCKET  id;
    int     recv;
    int     send;
    char    recvBuffer[BUFFER_SIZE];
    int     recvLen;
    string  sendBuffer;                 // grows to fit any response
    size_t  sendOffset;
    bool    closeAfterSend;
    time_t  lastActivity;
};

SocketState sockets[MAX_SOCKETS];
int socketsCount = 0;

// ─── Prototypes ───────────────────────────────────────────────────────────────
bool addSocket(SOCKET id, int what);
void removeSocket(int index);
void acceptConnection(int index);
void receiveMessage(int index);
void sendMessage(int index);
void handleHttpRequest(int index);
void checkTimeouts();

// ─── Helpers ──────────────────────────────────────────────────────────────────

bool setNonBlocking(SOCKET s)
{
#ifdef _WIN32
    unsigned long flag = 1;
    return ioctlsocket(s, FIONBIO, &flag) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags != -1 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Read a whole file. Returns false if it can't be opened (an empty file is fine).
bool readFile(const string& path, string& content)
{
    ifstream f(path.c_str(), ios::binary);
    if (!f.is_open()) return false;
    ostringstream ss;
    ss << f.rdbuf();
    content = ss.str();
    return true;
}

bool endsWith(const string& s, const string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Guess Content-Type from file extension
string getContentType(const string& path)
{
    if (endsWith(path, ".html") || endsWith(path, ".htm")) return "text/html; charset=utf-8";
    if (endsWith(path, ".txt"))  return "text/plain; charset=utf-8";
    if (endsWith(path, ".jpg") || endsWith(path, ".jpeg")) return "image/jpeg";
    if (endsWith(path, ".png"))  return "image/png";
    if (endsWith(path, ".css"))  return "text/css";
    if (endsWith(path, ".js"))   return "application/javascript";
    return "application/octet-stream";
}

string toLower(string s)
{
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

string trim(const string& s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// Escape text before echoing it inside an HTML page
string htmlEscape(const string& s)
{
    string out;
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;";  break;
        case '<': out += "&lt;";   break;
        case '>': out += "&gt;";   break;
        case '"': out += "&quot;"; break;
        default:  out += c;
        }
    }
    return out;
}

// A request path is only served if it stays inside the root folder
bool isSafePath(const string& path)
{
    if (path.empty() || path[0] != '/') return false;
    if (path.find("..") != string::npos) return false;
    if (path.find('\\') != string::npos || path.find(':') != string::npos) return false;
    return true;
}

// Map a URL path to a file under the root folder
string toFilePath(const string& urlPath)
{
    string p = rootDir + urlPath;
    for (char& c : p) if (c == '/') c = PATH_SEP;
    return p;
}

// Build a full HTTP response string.
// For HEAD requests pass includeBody = false: headers stay the same, body is dropped.
string buildResponse(int statusCode, const string& statusText,
                     const string& contentType, const string& body,
                     bool addAllow = false, bool includeBody = true,
                     bool keepAlive = true)
{
    ostringstream resp;
    resp << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    resp << "Content-Type: " << contentType << "\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    if (addAllow)
        resp << "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS, TRACE\r\n";
    resp << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n";
    resp << "\r\n";
    if (includeBody) resp << body;
    return resp.str();
}

// Parse query string into a map  e.g. "lang=he&foo=bar"
map<string,string> parseQueryString(const string& qs)
{
    map<string,string> params;
    istringstream ss(qs);
    string token;
    while (getline(ss, token, '&'))
    {
        size_t eq = token.find('=');
        if (eq != string::npos)
            params[token.substr(0, eq)] = token.substr(eq + 1);
        else
            params[token] = "";
    }
    return params;
}

// Parse the request line  "GET /path?qs HTTP/1.1"
// Returns false if malformed
bool parseRequestLine(const string& line,
                      string& method, string& path, string& queryString)
{
    istringstream ss(line);
    string url, version;
    if (!(ss >> method >> url >> version)) return false;

    size_t qpos = url.find('?');
    if (qpos != string::npos)
    {
        path        = url.substr(0, qpos);
        queryString = url.substr(qpos + 1);
    }
    else
    {
        path        = url;
        queryString = "";
    }
    return true;
}

void queueResponse(int index, const string& response, bool closeAfter = false)
{
    sockets[index].sendBuffer     = response;
    sockets[index].sendOffset     = 0;
    sockets[index].closeAfterSend = closeAfter;
    sockets[index].send           = SEND;
}

// ─── HTTP Request Handler ─────────────────────────────────────────────────────
void handleHttpRequest(int index)
{
    // One response at a time: pipelined requests wait until the current one is sent
    if (sockets[index].send == SEND) return;

    string raw(sockets[index].recvBuffer, sockets[index].recvLen);

    // We need at least the end of headers (\r\n\r\n)
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == string::npos)
    {
        if (sockets[index].recvLen >= BUFFER_SIZE - 1)
        {
            queueResponse(index, buildResponse(431, "Request Header Fields Too Large",
                                               "text/plain", "Request headers too large", false, true, false), true);
            sockets[index].recvLen = 0;
        }
        return;   // not complete yet
    }

    // Parse first line
    size_t firstLineEnd = raw.find("\r\n");
    string requestLine  = raw.substr(0, firstLineEnd);

    string method, path, queryString;
    if (!parseRequestLine(requestLine, method, path, queryString))
    {
        queueResponse(index, buildResponse(400, "Bad Request", "text/plain", "400 Bad Request", false, true, false), true);
        sockets[index].recvLen = 0;
        return;
    }

    // Parse headers into a map. Header names are case-insensitive, so keys are lowercased.
    map<string,string> headers;
    if (headerEnd > firstLineEnd)
    {
        string headerBlock = raw.substr(firstLineEnd + 2, headerEnd - firstLineEnd - 2);
        istringstream hss(headerBlock);
        string hline;
        while (getline(hss, hline))
        {
            if (!hline.empty() && hline.back() == '\r') hline.pop_back();
            size_t colon = hline.find(':');
            if (colon != string::npos)
                headers[toLower(trim(hline.substr(0, colon)))] = trim(hline.substr(colon + 1));
        }
    }

    // Body (for POST/PUT)
    string body = "";
    long contentLength = 0;
    if (headers.count("content-length"))
        contentLength = atol(headers["content-length"].c_str());
    if (contentLength < 0 || headerEnd + 4 + contentLength > (size_t)BUFFER_SIZE - 1)
    {
        queueResponse(index, buildResponse(413, "Payload Too Large", "text/plain",
                                           "Request body too large", false, true, false), true);
        sockets[index].recvLen = 0;
        return;
    }
    if (contentLength > 0)
    {
        size_t bodyStart = headerEnd + 4;
        if (raw.size() - bodyStart < (size_t)contentLength) return; // incomplete body
        body = raw.substr(bodyStart, contentLength);
    }

    cout << "Request: " << method << " " << path;
    if (!queryString.empty()) cout << "?" << queryString;
    cout << endl;

    bool keepAlive = toLower(headers["connection"]) != "close";
    string response;

    // ── OPTIONS ──────────────────────────────────────────────────────────────
    if (method == "OPTIONS")
    {
        response = buildResponse(200, "OK", "text/plain", "", true, true, keepAlive);
    }
    // ── TRACE ────────────────────────────────────────────────────────────────
    else if (method == "TRACE")
    {
        // Echo the request back as the body
        string echoBody = raw.substr(0, headerEnd + 4);
        response = buildResponse(200, "OK", "message/http", echoBody, false, true, keepAlive);
    }
    // Every method below touches a file, so the path must stay inside the root folder
    else if (!isSafePath(path))
    {
        response = buildResponse(400, "Bad Request", "text/plain", "Invalid path",
                                 false, method != "HEAD", keepAlive);
    }
    // ── GET / HEAD ───────────────────────────────────────────────────────────
    else if (method == "GET" || method == "HEAD")
    {
        bool isHead = (method == "HEAD");

        // Default file
        if (path == "/") path = "/index.html";

        // ?lang=he|en|fr serves index_he.html etc. when that version exists,
        // otherwise the default page.
        map<string,string> params = parseQueryString(queryString);
        string fileContent, filePath;
        bool found = false;

        if (params.count("lang"))
        {
            string lang = params["lang"];
            if (lang == "he" || lang == "en" || lang == "fr")
            {
                size_t dot = path.rfind('.');
                string localized = (dot != string::npos)
                    ? path.substr(0, dot) + "_" + lang + path.substr(dot)
                    : path + "_" + lang;
                filePath = toFilePath(localized);
                found = readFile(filePath, fileContent);
            }
        }
        if (!found)
        {
            filePath = toFilePath(path);
            found = readFile(filePath, fileContent);
        }

        if (!found)
        {
            string errBody = "<html><body><h1>404 Not Found</h1></body></html>";
            response = buildResponse(404, "Not Found", "text/html", errBody, false, !isHead, keepAlive);
        }
        else
        {
            response = buildResponse(200, "OK", getContentType(filePath), fileContent,
                                     false, !isHead, keepAlive);
        }
    }
    // ── POST ─────────────────────────────────────────────────────────────────
    else if (method == "POST")
    {
        // Print received strings/phrases to console
        cout << "POST body received: " << body << endl;
        string respBody = "<html><body><h1>POST received</h1><p>" + htmlEscape(body) + "</p></body></html>";
        response = buildResponse(200, "OK", "text/html", respBody, false, true, keepAlive);
    }
    // ── PUT ──────────────────────────────────────────────────────────────────
    else if (method == "PUT")
    {
        if (path == "/")
        {
            response = buildResponse(400, "Bad Request", "text/plain", "No path specified", false, true, keepAlive);
        }
        else
        {
            string filePath = toFilePath(path);
            string existing;
            bool existed = readFile(filePath, existing);

            ofstream f(filePath.c_str(), ios::binary);
            if (!f.is_open())
            {
                response = buildResponse(500, "Internal Server Error", "text/plain", "Could not write file", false, true, keepAlive);
            }
            else
            {
                f.write(body.c_str(), body.size());
                f.close();
                response = existed
                    ? buildResponse(200, "OK", "text/plain", "File updated", false, true, keepAlive)
                    : buildResponse(201, "Created", "text/plain", "File created", false, true, keepAlive);
            }
        }
    }
    // ── DELETE ───────────────────────────────────────────────────────────────
    else if (method == "DELETE")
    {
        if (remove(toFilePath(path).c_str()) == 0)
            response = buildResponse(200, "OK", "text/plain", "File deleted", false, true, keepAlive);
        else
            response = buildResponse(404, "Not Found", "text/plain", "File not found", false, true, keepAlive);
    }
    // ── Unknown method ───────────────────────────────────────────────────────
    else
    {
        response = buildResponse(501, "Not Implemented", "text/plain", "Method not supported", false, true, keepAlive);
    }

    queueResponse(index, response, !keepAlive);

    // Consume the processed request, keep anything after it (pipelined requests)
    int consumed = (int)(headerEnd + 4 + contentLength);
    if (consumed < sockets[index].recvLen)
    {
        memmove(sockets[index].recvBuffer,
                sockets[index].recvBuffer + consumed,
                sockets[index].recvLen - consumed);
        sockets[index].recvLen -= consumed;
    }
    else
    {
        sockets[index].recvLen = 0;
    }
}

// ─── Timeout check ────────────────────────────────────────────────────────────
void checkTimeouts()
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SOCKETS; i++)
    {
        if (sockets[i].recv == RECEIVE)
        {
            if (difftime(now, sockets[i].lastActivity) > TIMEOUT_SEC)
            {
                cout << "Closing idle connection (socket " << i << ")" << endl;
                closesocket(sockets[i].id);
                removeSocket(i);
            }
        }
    }
}

// ─── Socket management ────────────────────────────────────────────────────────
bool addSocket(SOCKET id, int what)
{
    for (int i = 0; i < MAX_SOCKETS; i++)
    {
        if (sockets[i].recv == EMPTY)
        {
            sockets[i].id             = id;
            sockets[i].recv           = what;
            sockets[i].send           = IDLE;
            sockets[i].recvLen        = 0;
            sockets[i].sendBuffer.clear();
            sockets[i].sendOffset     = 0;
            sockets[i].closeAfterSend = false;
            sockets[i].lastActivity   = time(NULL);
            socketsCount++;
            return true;
        }
    }
    return false;
}

void removeSocket(int index)
{
    sockets[index].recv = EMPTY;
    sockets[index].send = EMPTY;
    sockets[index].recvLen = 0;
    sockets[index].sendBuffer.clear();
    socketsCount--;
}

void acceptConnection(int index)
{
    SOCKET id = sockets[index].id;
    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);

    SOCKET msgSocket = accept(id, (struct sockaddr*)&from, &fromLen);
    if (INVALID_SOCKET == msgSocket)
    {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK)
            cout << "Error at accept(): " << err << endl;
        return;
    }
    cout << "Client " << inet_ntoa(from.sin_addr) << ":" << ntohs(from.sin_port) << " connected." << endl;

    if (!setNonBlocking(msgSocket))
        cout << "Error making socket non-blocking: " << WSAGetLastError() << endl;

    if (!addSocket(msgSocket, RECEIVE))
    {
        cout << "Too many connections, dropping." << endl;
        closesocket(msgSocket);
    }
}

void receiveMessage(int index)
{
    SOCKET msgSocket = sockets[index].id;
    int len      = sockets[index].recvLen;
    int space    = BUFFER_SIZE - len - 1;
    if (space <= 0) return;   // buffer full; handleHttpRequest answers 413/431

    int bytesRecv = (int)recv(msgSocket, &sockets[index].recvBuffer[len], space, 0);

    if (bytesRecv == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return;   // nothing ready, not an error
        cout << "recv() error: " << err << endl;
        closesocket(msgSocket);
        removeSocket(index);
        return;
    }
    if (bytesRecv == 0)
    {
        cout << "Client disconnected." << endl;
        closesocket(msgSocket);
        removeSocket(index);
        return;
    }

    sockets[index].recvLen += bytesRecv;
    sockets[index].recvBuffer[sockets[index].recvLen] = '\0';
    sockets[index].lastActivity = time(NULL);

    handleHttpRequest(index);
}

void sendMessage(int index)
{
    SOCKET msgSocket = sockets[index].id;
    const string& buf = sockets[index].sendBuffer;

    int bytesSent = (int)send(msgSocket,
                              buf.data() + sockets[index].sendOffset,
                              (int)(buf.size() - sockets[index].sendOffset), 0);

    if (bytesSent == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return;
        cout << "send() error: " << err << endl;
        closesocket(msgSocket);
        removeSocket(index);
        return;
    }

    sockets[index].sendOffset += bytesSent;
    sockets[index].lastActivity = time(NULL);

    if (sockets[index].sendOffset >= buf.size())
    {
        // Done sending
        if (sockets[index].closeAfterSend)
        {
            closesocket(msgSocket);
            removeSocket(index);
            return;
        }
        sockets[index].send = IDLE;
        sockets[index].sendBuffer.clear();
        sockets[index].sendOffset = 0;

        // If there's still data in the recv buffer, handle it (pipelined requests)
        if (sockets[index].recvLen > 0)
            handleHttpRequest(index);
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    if (argc > 1) rootDir = argv[1];

#ifdef _WIN32
    WSAData wsaData;
    if (NO_ERROR != WSAStartup(MAKEWORD(2, 2), &wsaData))
    {
        cout << "WSAStartup() failed" << endl;
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);   // a client closing mid-send must not kill the server
#endif

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (INVALID_SOCKET == listenSocket)
    {
        cout << "socket() failed: " << WSAGetLastError() << endl;
        return 1;
    }

#ifndef _WIN32
    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in serverService;
    memset(&serverService, 0, sizeof(serverService));
    serverService.sin_family      = AF_INET;
    serverService.sin_addr.s_addr = INADDR_ANY;
    serverService.sin_port        = htons(HTTP_PORT);

    if (SOCKET_ERROR == ::bind(listenSocket, (sockaddr*)&serverService, sizeof(serverService)))
    {
        cout << "bind() failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        return 1;
    }

    if (SOCKET_ERROR == listen(listenSocket, 5))
    {
        cout << "listen() failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        return 1;
    }

    setNonBlocking(listenSocket);
    addSocket(listenSocket, LISTEN);
    cout << "HTTP Server listening on port " << HTTP_PORT << endl;
    cout << "Serving files from " << rootDir << endl;

    while (true)
    {
        fd_set waitRecv, waitSend;
        FD_ZERO(&waitRecv);
        FD_ZERO(&waitSend);
        SOCKET maxFd = 0;

        for (int i = 0; i < MAX_SOCKETS; i++)
        {
            if (sockets[i].recv == LISTEN || sockets[i].recv == RECEIVE)
                FD_SET(sockets[i].id, &waitRecv);
            if (sockets[i].send == SEND)
                FD_SET(sockets[i].id, &waitSend);
            if (sockets[i].recv != EMPTY && sockets[i].id > maxFd)
                maxFd = sockets[i].id;
        }

        // Wake up at least every 5 seconds to close idle connections
        timeval tv;
        tv.tv_sec  = 5;
        tv.tv_usec = 0;

        int nfd = select((int)maxFd + 1, &waitRecv, &waitSend, NULL, &tv);
        if (nfd == SOCKET_ERROR)
        {
            cout << "select() failed: " << WSAGetLastError() << endl;
            break;
        }

        checkTimeouts();

        for (int i = 0; i < MAX_SOCKETS; i++)
        {
            if (sockets[i].recv == EMPTY) continue;   // may have been closed above
            if (FD_ISSET(sockets[i].id, &waitRecv))
            {
                switch (sockets[i].recv)
                {
                case LISTEN:  acceptConnection(i); break;
                case RECEIVE: receiveMessage(i);   break;
                }
            }
        }

        for (int i = 0; i < MAX_SOCKETS; i++)
        {
            if (sockets[i].recv == EMPTY) continue;
            if (sockets[i].send == SEND && FD_ISSET(sockets[i].id, &waitSend))
                sendMessage(i);
        }
    }

    closesocket(listenSocket);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
