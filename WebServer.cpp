#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <time.h>
using namespace std;

#pragma comment(lib, "Ws2_32.lib")
#include <winsock2.h>

// ─── Constants ────────────────────────────────────────────────────────────────
const int HTTP_PORT     = 8080;
const int MAX_SOCKETS   = 60;
const int BUFFER_SIZE   = 8192;
const int TIMEOUT_SEC   = 120;   // close idle connections after 2 minutes
const char* ROOT_DIR    = "c:\\temp";

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
    char    sendBuffer[BUFFER_SIZE * 4];   // responses can be larger
    int     sendLen;
    int     sendOffset;
    time_t  lastActivity;
};

struct SocketState sockets[MAX_SOCKETS] = {0};
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

// Read a file from disk, return contents as string. Empty string = not found.
string readFile(const string& path)
{
    ifstream f(path.c_str(), ios::binary);
    if (!f.is_open()) return "";
    ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Guess Content-Type from file extension
string getContentType(const string& path)
{
    if (path.size() >= 5 && path.substr(path.size()-5) == ".html") return "text/html; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".htm")  return "text/html; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".txt")  return "text/plain; charset=utf-8";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".jpg")  return "image/jpeg";
    if (path.size() >= 5 && path.substr(path.size()-5) == ".jpeg") return "image/jpeg";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".png")  return "image/png";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".css")  return "text/css";
    if (path.size() >= 3 && path.substr(path.size()-3) == ".js")   return "application/javascript";
    return "application/octet-stream";
}

// Build a full HTTP response string
string buildResponse(int statusCode, const string& statusText,
                     const string& contentType, const string& body,
                     bool addAllow = false)
{
    ostringstream resp;
    resp << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    resp << "Content-Type: " << contentType << "\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    if (addAllow)
        resp << "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS, TRACE\r\n";
    resp << "Connection: keep-alive\r\n";
    resp << "\r\n";
    resp << body;
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

// ─── HTTP Request Handler ─────────────────────────────────────────────────────
void handleHttpRequest(int index)
{
    string raw(sockets[index].recvBuffer, sockets[index].recvLen);

    // We need at least the end of headers (\r\n\r\n)
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == string::npos) return;   // not complete yet

    // Parse first line
    size_t firstLineEnd = raw.find("\r\n");
    string requestLine  = raw.substr(0, firstLineEnd);

    string method, path, queryString;
    if (!parseRequestLine(requestLine, method, path, queryString))
    {
        string resp = buildResponse(400, "Bad Request", "text/plain", "400 Bad Request");
        memcpy(sockets[index].sendBuffer, resp.c_str(), resp.size());
        sockets[index].sendLen    = (int)resp.size();
        sockets[index].sendOffset = 0;
        sockets[index].send       = SEND;
        sockets[index].recvLen    = 0;
        return;
    }

    // Parse headers into a map
    map<string,string> headers;
    string headerBlock = raw.substr(firstLineEnd + 2, headerEnd - firstLineEnd - 2);
    istringstream hss(headerBlock);
    string hline;
    while (getline(hss, hline))
    {
        if (!hline.empty() && hline.back() == '\r') hline.pop_back();
        size_t colon = hline.find(':');
        if (colon != string::npos)
        {
            string key = hline.substr(0, colon);
            string val = hline.substr(colon + 2);   // skip ": "
            headers[key] = val;
        }
    }

    // Body (for POST/PUT)
    string body = "";
    int contentLength = 0;
    if (headers.count("Content-Length"))
        contentLength = atoi(headers["Content-Length"].c_str());
    if (contentLength > 0)
    {
        size_t bodyStart = headerEnd + 4;
        if ((int)(raw.size() - bodyStart) < contentLength) return; // incomplete body
        body = raw.substr(bodyStart, contentLength);
    }

    cout << "Request: " << method << " " << path;
    if (!queryString.empty()) cout << "?" << queryString;
    cout << endl;

    string response;

    // ── OPTIONS ──────────────────────────────────────────────────────────────
    if (method == "OPTIONS")
    {
        response = buildResponse(200, "OK", "text/plain", "", true);
    }
    // ── TRACE ────────────────────────────────────────────────────────────────
    else if (method == "TRACE")
    {
        // Echo the request back as the body
        string echoBody = raw.substr(0, headerEnd + 4);
        response = buildResponse(200, "OK", "message/http", echoBody);
    }
    // ── GET / HEAD ───────────────────────────────────────────────────────────
    else if (method == "GET" || method == "HEAD")
    {
        // Default file
        if (path == "/") path = "/index.html";

        // Handle ?lang= query parameter
        map<string,string> params = parseQueryString(queryString);
        string filePath;

        if (params.count("lang"))
        {
            string lang = params["lang"];
            // strip extension, insert _<lang>, re-add extension
            // e.g. /index.html + lang=he  →  c:\temp\index_he.html
            size_t dot = path.rfind('.');
            if (dot != string::npos)
            {
                string base = path.substr(0, dot);
                string ext  = path.substr(dot);
                filePath = string(ROOT_DIR) + base + "_" + lang + ext;
            }
            else
            {
                filePath = string(ROOT_DIR) + path + "_" + lang;
            }
        }
        else
        {
            filePath = string(ROOT_DIR) + path;
        }

        // Convert forward slashes to backslashes for Windows
        for (char& c : filePath) if (c == '/') c = '\\';

        string fileContent = readFile(filePath);
        if (fileContent.empty())
        {
            string errBody = "<html><body><h1>404 Not Found</h1></body></html>";
            response = buildResponse(404, "Not Found", "text/html", errBody);
        }
        else
        {
            string ct = getContentType(filePath);
            if (method == "HEAD")
            {
                // HEAD: headers only, no body
                ostringstream resp;
                resp << "HTTP/1.1 200 OK\r\n";
                resp << "Content-Type: " << ct << "\r\n";
                resp << "Content-Length: " << fileContent.size() << "\r\n";
                resp << "Connection: keep-alive\r\n";
                resp << "\r\n";
                response = resp.str();
            }
            else
            {
                response = buildResponse(200, "OK", ct, fileContent);
            }
        }
    }
    // ── POST ─────────────────────────────────────────────────────────────────
    else if (method == "POST")
    {
        // Print received strings/phrases to console
        cout << "POST body received: " << body << endl;
        string respBody = "<html><body><h1>POST received</h1><p>" + body + "</p></body></html>";
        response = buildResponse(200, "OK", "text/html", respBody);
    }
    // ── PUT ──────────────────────────────────────────────────────────────────
    else if (method == "PUT")
    {
        if (path == "/" || path.empty())
        {
            response = buildResponse(400, "Bad Request", "text/plain", "No path specified");
        }
        else
        {
            string filePath = string(ROOT_DIR) + path;
            for (char& c : filePath) if (c == '/') c = '\\';

            ofstream f(filePath.c_str(), ios::binary);
            if (!f.is_open())
            {
                response = buildResponse(500, "Internal Server Error", "text/plain", "Could not write file");
            }
            else
            {
                f.write(body.c_str(), body.size());
                f.close();
                response = buildResponse(201, "Created", "text/plain", "File created");
            }
        }
    }
    // ── DELETE ───────────────────────────────────────────────────────────────
    else if (method == "DELETE")
    {
        string filePath = string(ROOT_DIR) + path;
        for (char& c : filePath) if (c == '/') c = '\\';

        if (remove(filePath.c_str()) == 0)
            response = buildResponse(200, "OK", "text/plain", "File deleted");
        else
            response = buildResponse(404, "Not Found", "text/plain", "File not found");
    }
    // ── Unknown method ───────────────────────────────────────────────────────
    else
    {
        response = buildResponse(501, "Not Implemented", "text/plain", "Method not supported");
    }

    // Copy response to send buffer
    int respLen = (int)response.size();
    if (respLen > (int)sizeof(sockets[index].sendBuffer))
        respLen = (int)sizeof(sockets[index].sendBuffer);

    memcpy(sockets[index].sendBuffer, response.c_str(), respLen);
    sockets[index].sendLen    = respLen;
    sockets[index].sendOffset = 0;
    sockets[index].send       = SEND;

    // Clear recv buffer — consume the processed request
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
        if (sockets[i].recv == RECEIVE || sockets[i].send == SEND)
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
            sockets[i].id         = id;
            sockets[i].recv       = what;
            sockets[i].send       = IDLE;
            sockets[i].recvLen    = 0;
            sockets[i].sendLen    = 0;
            sockets[i].sendOffset = 0;
            sockets[i].lastActivity = time(NULL);
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
    sockets[index].sendLen = 0;
    socketsCount--;
}

void acceptConnection(int index)
{
    SOCKET id = sockets[index].id;
    struct sockaddr_in from;
    int fromLen = sizeof(from);

    SOCKET msgSocket = accept(id, (struct sockaddr*)&from, &fromLen);
    if (INVALID_SOCKET == msgSocket)
    {
        cout << "Error at accept(): " << WSAGetLastError() << endl;
        return;
    }
    cout << "Client " << inet_ntoa(from.sin_addr) << ":" << ntohs(from.sin_port) << " connected." << endl;

    unsigned long flag = 1;
    if (ioctlsocket(msgSocket, FIONBIO, &flag) != 0)
        cout << "Error at ioctlsocket(): " << WSAGetLastError() << endl;

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

    int bytesRecv = recv(msgSocket, &sockets[index].recvBuffer[len], space, 0);

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

    int toSend = sockets[index].sendLen - sockets[index].sendOffset;
    int bytesSent = send(msgSocket,
                         sockets[index].sendBuffer + sockets[index].sendOffset,
                         toSend, 0);

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

    if (sockets[index].sendOffset >= sockets[index].sendLen)
    {
        // Done sending
        sockets[index].send       = IDLE;
        sockets[index].sendLen    = 0;
        sockets[index].sendOffset = 0;

        // If there's still data in the recv buffer, handle it (pipelined requests)
        if (sockets[index].recvLen > 0)
            handleHttpRequest(index);
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
void main()
{
    WSAData wsaData;
    if (NO_ERROR != WSAStartup(MAKEWORD(2, 2), &wsaData))
    {
        cout << "WSAStartup() failed" << endl;
        return;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (INVALID_SOCKET == listenSocket)
    {
        cout << "socket() failed: " << WSAGetLastError() << endl;
        WSACleanup();
        return;
    }

    sockaddr_in serverService;
    serverService.sin_family      = AF_INET;
    serverService.sin_addr.s_addr = INADDR_ANY;
    serverService.sin_port        = htons(HTTP_PORT);

    if (SOCKET_ERROR == bind(listenSocket, (SOCKADDR*)&serverService, sizeof(serverService)))
    {
        cout << "bind() failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return;
    }

    if (SOCKET_ERROR == listen(listenSocket, 5))
    {
        cout << "listen() failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return;
    }

    addSocket(listenSocket, LISTEN);
    cout << "HTTP Server listening on port " << HTTP_PORT << endl;
    cout << "Serving files from " << ROOT_DIR << endl;

    while (true)
    {
        fd_set waitRecv, waitSend;
        FD_ZERO(&waitRecv);
        FD_ZERO(&waitSend);

        for (int i = 0; i < MAX_SOCKETS; i++)
        {
            if (sockets[i].recv == LISTEN || sockets[i].recv == RECEIVE)
                FD_SET(sockets[i].id, &waitRecv);
            if (sockets[i].send == SEND)
                FD_SET(sockets[i].id, &waitSend);
        }

        // Timeout of 5 seconds so we can check for idle connections
        timeval tv;
        tv.tv_sec  = 5;
        tv.tv_usec = 0;

        int nfd = select(0, &waitRecv, &waitSend, NULL, &tv);
        if (nfd == SOCKET_ERROR)
        {
            cout << "select() failed: " << WSAGetLastError() << endl;
            WSACleanup();
            return;
        }

        checkTimeouts();

        for (int i = 0; i < MAX_SOCKETS && nfd > 0; i++)
        {
            if (FD_ISSET(sockets[i].id, &waitRecv))
            {
                nfd--;
                switch (sockets[i].recv)
                {
                case LISTEN:  acceptConnection(i); break;
                case RECEIVE: receiveMessage(i);   break;
                }
            }
        }

        for (int i = 0; i < MAX_SOCKETS && nfd > 0; i++)
        {
            if (FD_ISSET(sockets[i].id, &waitSend))
            {
                nfd--;
                if (sockets[i].send == SEND)
                    sendMessage(i);
            }
        }
    }

    closesocket(listenSocket);
    WSACleanup();
}
