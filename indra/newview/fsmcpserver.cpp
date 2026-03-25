/**
 * @file fsmcpserver.cpp
 * @brief Firestorm MCP (Model Context Protocol) Server – implementation
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Phoenix Firestorm Project Source Code
 * Copyright (C) 2024, The Phoenix Firestorm Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA 94111 USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "fsmcpserver.h"

// Viewer APIs
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llchat.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "fsnearbychathub.h"

// LL common
#include "llsd.h"
#include "llsdjson.h"
#include "llversioninfo.h"

// Boost JSON (already a transitive dependency via llcommon)
#include <boost/json.hpp>

// Platform sockets -----------------------------------------------------------
#ifdef LL_WINDOWS
# include <winsock2.h>
# include <ws2tcpip.h>
  typedef SOCKET   socket_fd_t;
# define INVALID_SOCKET_FD INVALID_SOCKET
# define close_socket      closesocket
# pragma comment(lib, "ws2_32.lib")
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <unistd.h>
# include <fcntl.h>
# include <sys/select.h>
  typedef int  socket_fd_t;
# define INVALID_SOCKET_FD (-1)
# define close_socket      ::close
#endif

// Standard library
#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const std::string MCP_PROTOCOL_VERSION = "2024-11-05";
static const std::string MCP_SERVER_NAME       = "firestorm-mcp";
static const int         HTTP_READ_TIMEOUT_S   = 10;
static const int         TOOL_CALL_TIMEOUT_S   = 30;

// ---------------------------------------------------------------------------
// Helper: build a JSON-RPC 2.0 success response LLSD
// ---------------------------------------------------------------------------
static LLSD makeJSONRPCResult(const LLSD& id, const LLSD& result)
{
    LLSD resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = result;
    return resp;
}

// ---------------------------------------------------------------------------
// Helper: build a JSON-RPC 2.0 error response LLSD
// ---------------------------------------------------------------------------
static LLSD makeJSONRPCError(const LLSD& id, S32 code, const std::string& message)
{
    LLSD error;
    error["code"]    = LLSD::Integer(code);
    error["message"] = message;

    LLSD resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["error"]   = error;
    return resp;
}

// ---------------------------------------------------------------------------
// ServerThread
// ---------------------------------------------------------------------------
class FSMCPServer::ServerThread : public LLThread
{
public:
    ServerThread(FSMCPServer* server, U16 port)
        : LLThread("FSMCPServerThread")
        , mServer(server)
        , mPort(port)
        , mListenFd(INVALID_SOCKET_FD)
    {}

    virtual ~ServerThread() = default;

protected:
    void run() override;

private:
    struct HttpRequest
    {
        std::string verb;
        std::string path;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    bool createListenSocket();
    void closeListenSocket();

    bool readHttpRequest(socket_fd_t client_fd, HttpRequest& req);
    void sendHttpResponse(socket_fd_t     client_fd,
                          int             status_code,
                          const char*     status_text,
                          const std::string& body,
                          const char*     content_type = "application/json");
    void handleClient(socket_fd_t client_fd);

    FSMCPServer* mServer;
    U16          mPort;
    socket_fd_t  mListenFd;
};

// ---- ServerThread::run ----------------------------------------------------
void FSMCPServer::ServerThread::run()
{
#ifdef LL_WINDOWS
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    if (!createListenSocket())
    {
        LL_WARNS("MCP") << "FSMCPServer: failed to bind on port " << mPort << LL_ENDL;
        return;
    }

    LL_INFOS("MCP") << "FSMCPServer: listening on port " << mPort << LL_ENDL;

    while (!isQuitting())
    {
        // Use select() with a 1-second timeout so we can check isQuitting()
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(mListenFd, &read_fds);

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int ret = select((int)mListenFd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ret <= 0)
            continue; // timeout or error – loop and check isQuitting()

        struct sockaddr_in client_addr {};
#ifdef LL_WINDOWS
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        socket_fd_t client_fd = accept(mListenFd,
                                       reinterpret_cast<struct sockaddr*>(&client_addr),
                                       &addr_len);
        if (client_fd == INVALID_SOCKET_FD)
            continue;

        handleClient(client_fd);
        close_socket(client_fd);
    }

    closeListenSocket();

#ifdef LL_WINDOWS
    WSACleanup();
#endif

    LL_INFOS("MCP") << "FSMCPServer: thread stopped" << LL_ENDL;
}

// ---- createListenSocket ---------------------------------------------------
bool FSMCPServer::ServerThread::createListenSocket()
{
    mListenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mListenFd == INVALID_SOCKET_FD)
        return false;

    // Allow rapid restart without TIME_WAIT issues
    int opt = 1;
#ifdef LL_WINDOWS
    setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
    addr.sin_port        = htons(mPort);

    if (bind(mListenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        close_socket(mListenFd);
        mListenFd = INVALID_SOCKET_FD;
        return false;
    }

    if (listen(mListenFd, 8) < 0)
    {
        close_socket(mListenFd);
        mListenFd = INVALID_SOCKET_FD;
        return false;
    }

    return true;
}

// ---- closeListenSocket ----------------------------------------------------
void FSMCPServer::ServerThread::closeListenSocket()
{
    if (mListenFd != INVALID_SOCKET_FD)
    {
        close_socket(mListenFd);
        mListenFd = INVALID_SOCKET_FD;
    }
}

// ---- readHttpRequest ------------------------------------------------------
bool FSMCPServer::ServerThread::readHttpRequest(socket_fd_t client_fd,
                                                HttpRequest& req)
{
    // Apply a read timeout so slow clients do not stall the thread
#ifdef LL_WINDOWS
    DWORD timeout_ms = HTTP_READ_TIMEOUT_S * 1000;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec  = HTTP_READ_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Read until the end of HTTP headers ("\r\n\r\n")
    std::string raw;
    raw.reserve(4096);
    char buf[4096];

    while (raw.find("\r\n\r\n") == std::string::npos)
    {
        int n = recv(client_fd, buf, (int)sizeof(buf) - 1, 0);
        if (n <= 0)
            return false;
        buf[n] = '\0';
        raw.append(buf, n);
        if (raw.size() > 1024 * 1024) // guard against runaway headers
            return false;
    }

    size_t header_end = raw.find("\r\n\r\n");
    std::string headers_raw = raw.substr(0, header_end);
    std::string body_start  = raw.substr(header_end + 4);

    // Parse the request line
    size_t line_end = headers_raw.find("\r\n");
    if (line_end == std::string::npos)
        return false;

    std::istringstream rl(headers_raw.substr(0, line_end));
    std::string http_version;
    rl >> req.verb >> req.path >> http_version;

    // Parse header fields
    size_t pos = line_end + 2;
    while (pos < headers_raw.size())
    {
        size_t end = headers_raw.find("\r\n", pos);
        if (end == std::string::npos)
            end = headers_raw.size();

        size_t colon = headers_raw.find(':', pos);
        if (colon != std::string::npos && colon < end)
        {
            std::string name = headers_raw.substr(pos, colon - pos);
            std::string val  = headers_raw.substr(colon + 1, end - colon - 1);

            // Trim leading whitespace from value
            size_t vs = val.find_first_not_of(" \t");
            if (vs != std::string::npos) val = val.substr(vs);

            // Normalize name to lower-case
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });
            req.headers[name] = val;
        }

        pos = (end >= headers_raw.size()) ? end : end + 2;
    }

    // Read the request body based on Content-Length
    int content_length = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end())
    {
        try { content_length = std::stoi(it->second); }
        catch (...) { content_length = 0; }
    }

    req.body = body_start;
    while ((int)req.body.size() < content_length)
    {
        int n = recv(client_fd, buf, (int)sizeof(buf) - 1, 0);
        if (n <= 0)
            break;
        buf[n] = '\0';
        req.body.append(buf, n);
    }

    return true;
}

// ---- sendHttpResponse -----------------------------------------------------
void FSMCPServer::ServerThread::sendHttpResponse(socket_fd_t     client_fd,
                                                  int             status_code,
                                                  const char*     status_text,
                                                  const std::string& body,
                                                  const char*     content_type)
{
    std::ostringstream os;
    os << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
       << "Content-Type: " << content_type << "; charset=utf-8\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
       << "Connection: close\r\n"
       << "\r\n"
       << body;

    std::string resp = os.str();
    const char* data = resp.c_str();
    size_t      left = resp.size();
    while (left > 0)
    {
        int sent = send(client_fd, data, (int)left, 0);
        if (sent <= 0) break;
        data += sent;
        left -= sent;
    }
}

// ---- handleClient ---------------------------------------------------------
void FSMCPServer::ServerThread::handleClient(socket_fd_t client_fd)
{
    HttpRequest req;
    if (!readHttpRequest(client_fd, req))
    {
        sendHttpResponse(client_fd, 400, "Bad Request",
                         R"({"error":"Invalid HTTP request"})");
        return;
    }

    // CORS preflight
    if (req.verb == "OPTIONS")
    {
        sendHttpResponse(client_fd, 204, "No Content", "");
        return;
    }

    if (req.path != "/mcp")
    {
        sendHttpResponse(client_fd, 404, "Not Found",
                         R"({"error":"Not found. Use POST /mcp"})");
        return;
    }

    if (req.verb != "POST")
    {
        sendHttpResponse(client_fd, 405, "Method Not Allowed",
                         R"({"error":"Only POST is accepted"})");
        return;
    }

    if (req.body.empty())
    {
        sendHttpResponse(client_fd, 400, "Bad Request",
                         R"({"error":"Empty request body"})");
        return;
    }

    // Parse the JSON body
    boost::json::value json_req;
    try
    {
        json_req = boost::json::parse(req.body);
    }
    catch (const std::exception& e)
    {
        std::string err = std::string(R"({"error":"JSON parse error: ")") + e.what() + "\"}";
        sendHttpResponse(client_fd, 400, "Bad Request", err);
        return;
    }

    LLSD rpc = LlsdFromJson(json_req);

    std::string method = rpc["method"].asString();
    LLSD        params = rpc.has("params") ? rpc["params"] : LLSD();
    LLSD        id     = rpc.has("id")     ? rpc["id"]     : LLSD();

    if (method.empty())
    {
        std::string resp_str = boost::json::serialize(
            LlsdToJson(makeJSONRPCError(id, -32600, "Missing 'method' field")));
        sendHttpResponse(client_fd, 200, "OK", resp_str);
        return;
    }

    // Dispatch to main thread and wait for result
    std::future<LLSD> future = mServer->enqueueCall(method, params);
    LLSD result;

    auto status = future.wait_for(std::chrono::seconds(TOOL_CALL_TIMEOUT_S));
    if (status == std::future_status::ready)
    {
        result = future.get();
    }
    else
    {
        result = makeJSONRPCError(id, -32603, "Request timed out");
    }

    // Inject the JSON-RPC id and version if not already set
    if (!result.has("id"))     result["id"]      = id;
    if (!result.has("jsonrpc")) result["jsonrpc"] = "2.0";

    std::string resp_str = boost::json::serialize(LlsdToJson(result));
    sendHttpResponse(client_fd, 200, "OK", resp_str);
}

// ===========================================================================
// FSMCPServer – singleton implementation
// ===========================================================================

FSMCPServer::FSMCPServer() = default;

FSMCPServer::~FSMCPServer()
{
    stop();
}

// ---------------------------------------------------------------------------
void FSMCPServer::start(U16 port)
{
    if (mRunning)
    {
        LL_WARNS("MCP") << "FSMCPServer: already running on port " << mPort << LL_ENDL;
        return;
    }

    mPort = port;
    mServerThread = new ServerThread(this, port);
    mServerThread->start();
    mRunning = true;

    LL_INFOS("MCP") << "FSMCPServer: starting on port " << port << LL_ENDL;
}

// ---------------------------------------------------------------------------
void FSMCPServer::stop()
{
    if (!mRunning)
        return;

    mRunning = false;

    // Drain any calls sitting in the queue and fulfill their promises with an
    // error so the server thread can unblock from future.wait_for() quickly.
    {
        std::queue<std::shared_ptr<PendingCall>> drain;
        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            std::swap(drain, mCallQueue);
        }
        while (!drain.empty())
        {
            auto call = drain.front();
            drain.pop();
            try
            {
                call->result_promise.set_value(
                    makeJSONRPCError(LLSD(), -32603, "Server stopping"));
            }
            catch (...) {}
        }
    }

    if (mServerThread)
    {
        // shutdown() sets isQuitting() and waits up to 60 s for the thread.
        // After draining above, any blocked future.wait_for() will return
        // quickly, and the thread will exit at its next isQuitting() check.
        mServerThread->shutdown();
        delete mServerThread;
        mServerThread = nullptr;
    }

    LL_INFOS("MCP") << "FSMCPServer: stopped" << LL_ENDL;
}

// ---------------------------------------------------------------------------
bool FSMCPServer::isRunning() const
{
    return mRunning;
}

// ---------------------------------------------------------------------------
std::future<LLSD> FSMCPServer::enqueueCall(const std::string& method,
                                            const LLSD& params)
{
    auto call          = std::make_shared<PendingCall>();
    call->method       = method;
    call->params       = params;
    std::future<LLSD> future = call->result_promise.get_future();

    std::lock_guard<std::mutex> lock(mQueueMutex);
    mCallQueue.push(call);

    return future;
}

// ---------------------------------------------------------------------------
void FSMCPServer::processQueue()
{
    if (!mRunning)
        return;

    // Drain the queue under the lock, then process outside the lock so that
    // the server thread can enqueue new calls while we are working.
    std::queue<std::shared_ptr<PendingCall>> to_process;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        std::swap(to_process, mCallQueue);
    }

    while (!to_process.empty())
    {
        auto call = to_process.front();
        to_process.pop();

        LLSD result;
        try
        {
            result = dispatch(call->method, call->params);
        }
        catch (const std::exception& e)
        {
            result = makeJSONRPCError(LLSD(), -32603,
                                      std::string("Internal error: ") + e.what());
        }

        call->result_promise.set_value(result);
    }
}

// ---------------------------------------------------------------------------
LLSD FSMCPServer::dispatch(const std::string& method, const LLSD& params)
{
    if (method == "initialize")
        return handleInitialize(params);
    if (method == "tools/list")
        return handleToolsList();
    if (method == "tools/call")
        return handleToolsCall(params);

    LLSD result;
    result["error"]["code"]    = LLSD::Integer(-32601);
    result["error"]["message"] = "Method not found: " + method;
    return result;
}

// ===========================================================================
// MCP protocol handlers
// ===========================================================================

LLSD FSMCPServer::handleInitialize(const LLSD& /*params*/)
{
    LLSD server_info;
    server_info["name"]    = MCP_SERVER_NAME;
    server_info["version"] = LLVersionInfo::instance().getVersion();

    LLSD capabilities;
    capabilities["tools"]["listChanged"] = false;

    LLSD r;
    r["protocolVersion"] = MCP_PROTOCOL_VERSION;
    r["serverInfo"]      = server_info;
    r["capabilities"]    = capabilities;

    LLSD resp;
    resp["result"] = r;
    return resp;
}

LLSD FSMCPServer::handleToolsList()
{
    LLSD r;
    r["tools"] = buildToolsList();

    LLSD resp;
    resp["result"] = r;
    return resp;
}

LLSD FSMCPServer::handleToolsCall(const LLSD& params)
{
    std::string tool_name = params["name"].asString();
    LLSD        tool_args = params.has("arguments") ? params["arguments"] : LLSD();

    LLSD tool_result;

    if      (tool_name == "get_viewer_status")  tool_result = toolGetViewerStatus(tool_args);
    else if (tool_name == "get_agent_info")     tool_result = toolGetAgentInfo(tool_args);
    else if (tool_name == "get_nearby_avatars") tool_result = toolGetNearbyAvatars(tool_args);
    else if (tool_name == "get_region_info")    tool_result = toolGetRegionInfo(tool_args);
    else if (tool_name == "send_chat")          tool_result = toolSendChat(tool_args);
    else if (tool_name == "teleport")           tool_result = toolTeleport(tool_args);
    else if (tool_name == "navigate_to")        tool_result = toolNavigateTo(tool_args);
    else if (tool_name == "set_flying")         tool_result = toolSetFlying(tool_args);
    else if (tool_name == "stand_up")           tool_result = toolStandUp(tool_args);
    else
    {
        LLSD resp;
        resp["error"]["code"]    = LLSD::Integer(-32601);
        resp["error"]["message"] = "Unknown tool: " + tool_name;
        return resp;
    }

    // Wrap the result in the MCP content envelope
    LLSD content_item;
    content_item["type"] = "text";
    content_item["text"] = boost::json::serialize(LlsdToJson(tool_result));

    LLSD content;
    content.append(content_item);

    LLSD r;
    r["content"] = content;

    LLSD resp;
    resp["result"] = r;
    return resp;
}

// ===========================================================================
// Viewer tool implementations (all executed on the main thread)
// ===========================================================================

LLSD FSMCPServer::toolGetViewerStatus(const LLSD& /*args*/)
{
    LLSD r;
    bool logged_in = (gAgent.getRegion() != nullptr);
    r["logged_in"] = logged_in;

    if (logged_in)
    {
        r["region_name"] = gAgent.getRegion()->getName();

        if (isAgentAvatarValid())
            r["agent_name"] = gAgentAvatarp->getFullname();
    }

    return r;
}

LLSD FSMCPServer::toolGetAgentInfo(const LLSD& /*args*/)
{
    LLSD r;

    if (!gAgent.getRegion())
    {
        r["error"] = "Not logged in";
        return r;
    }

    r["id"] = gAgent.getID().asString();

    if (isAgentAvatarValid())
    {
        r["name"]    = gAgentAvatarp->getFullname();
        r["sitting"] = gAgentAvatarp->isSitting();
    }

    LLVector3d pos = gAgent.getPositionGlobal();
    LLSD pos_map;
    pos_map["x"] = pos.mdV[VX];
    pos_map["y"] = pos.mdV[VY];
    pos_map["z"] = pos.mdV[VZ];
    r["position"] = pos_map;

    r["region"]  = gAgent.getRegion()->getName();
    r["flying"]  = gAgent.getFlying();

    return r;
}

LLSD FSMCPServer::toolGetNearbyAvatars(const LLSD& args)
{
    LLSD r;

    if (!gAgent.getRegion())
    {
        r["error"] = "Not logged in";
        return r;
    }

    F32 radius = args.has("radius") ? (F32)args["radius"].asReal() : 96.0f;

    uuid_vec_t              ids;
    std::vector<LLVector3d> positions;
    LLWorld::instance().getAvatars(&ids, &positions, gAgent.getPositionGlobal(), radius);

    LLSD avatars;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        LLSD av;
        av["id"] = ids[i].asString();

        LLSD pos_map;
        pos_map["x"] = positions[i].mdV[VX];
        pos_map["y"] = positions[i].mdV[VY];
        pos_map["z"] = positions[i].mdV[VZ];
        av["position"] = pos_map;

        LLAvatarName av_name;
        if (LLAvatarNameCache::get(ids[i], &av_name))
            av["name"] = av_name.getCompleteName();
        else
            av["name"] = ids[i].asString();

        avatars.append(av);
    }

    r["avatars"] = avatars;
    r["count"]   = LLSD::Integer((S32)ids.size());
    return r;
}

LLSD FSMCPServer::toolGetRegionInfo(const LLSD& /*args*/)
{
    LLSD r;

    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        r["error"] = "Not in a region";
        return r;
    }

    r["name"] = region->getName();
    r["id"]   = region->getRegionID().asString();

    LLVector3d origin = region->getOriginGlobal();
    LLSD orig;
    orig["x"] = origin.mdV[VX];
    orig["y"] = origin.mdV[VY];
    r["origin"] = orig;

    return r;
}

LLSD FSMCPServer::toolSendChat(const LLSD& args)
{
    LLSD r;

    if (!args.has("message") || args["message"].asString().empty())
    {
        r["success"] = false;
        r["error"]   = "Missing required parameter: message";
        return r;
    }

    std::string message  = args["message"].asString();
    std::string type_str = args.has("type") ? args["type"].asString() : "normal";

    EChatType chat_type = CHAT_TYPE_NORMAL;
    if      (type_str == "whisper") chat_type = CHAT_TYPE_WHISPER;
    else if (type_str == "shout")   chat_type = CHAT_TYPE_SHOUT;

    FSNearbyChat::instance().sendChatFromViewer(message, chat_type, false);

    r["success"] = true;
    return r;
}

LLSD FSMCPServer::toolTeleport(const LLSD& args)
{
    LLSD r;

    if (!gAgent.getRegion())
    {
        r["success"] = false;
        r["error"]   = "Not logged in";
        return r;
    }

    if (args.has("x") && args.has("y") && args.has("z"))
    {
        LLVector3d pos;
        pos.mdV[VX] = args["x"].asReal();
        pos.mdV[VY] = args["y"].asReal();
        pos.mdV[VZ] = args["z"].asReal();
        gAgent.teleportViaLocation(pos);
        r["success"] = true;
    }
    else if (args.has("landmark_id"))
    {
        LLUUID id(args["landmark_id"].asString());
        if (id.notNull())
        {
            gAgent.teleportViaLandmark(id);
            r["success"] = true;
        }
        else
        {
            r["success"] = false;
            r["error"]   = "Invalid landmark UUID";
        }
    }
    else if (args.has("home") && args["home"].asBoolean())
    {
        gAgent.teleportHome();
        r["success"] = true;
    }
    else
    {
        r["success"] = false;
        r["error"]   = "Provide x/y/z, landmark_id, or home:true";
    }

    return r;
}

LLSD FSMCPServer::toolNavigateTo(const LLSD& args)
{
    LLSD r;

    if (!gAgent.getRegion())
    {
        r["success"] = false;
        r["error"]   = "Not logged in";
        return r;
    }

    if (!args.has("x") || !args.has("y") || !args.has("z"))
    {
        r["success"] = false;
        r["error"]   = "Missing required parameters: x, y, z";
        return r;
    }

    LLVector3d target;
    target.mdV[VX] = args["x"].asReal();
    target.mdV[VY] = args["y"].asReal();
    target.mdV[VZ] = args["z"].asReal();

    gAgent.startAutoPilotGlobal(target);
    r["success"] = true;
    return r;
}

LLSD FSMCPServer::toolSetFlying(const LLSD& args)
{
    LLSD r;

    if (!gAgent.getRegion())
    {
        r["success"] = false;
        r["error"]   = "Not logged in";
        return r;
    }

    bool flying = args.has("flying") ? args["flying"].asBoolean() : true;
    gAgent.setFlying(flying);

    r["success"] = true;
    r["flying"]  = flying;
    return r;
}

LLSD FSMCPServer::toolStandUp(const LLSD& /*args*/)
{
    LLSD r;

    if (!gAgent.getRegion())
    {
        r["success"] = false;
        r["error"]   = "Not logged in";
        return r;
    }

    gAgent.standUp();
    r["success"] = true;
    return r;
}

// ===========================================================================
// Tool schema builder
// ===========================================================================

static LLSD strProp(const char* description)
{
    LLSD p;
    p["type"]        = "string";
    p["description"] = description;
    return p;
}

static LLSD numProp(const char* description)
{
    LLSD p;
    p["type"]        = "number";
    p["description"] = description;
    return p;
}

static LLSD boolProp(const char* description)
{
    LLSD p;
    p["type"]        = "boolean";
    p["description"] = description;
    return p;
}

static LLSD makeTool(const char* name, const char* description,
                     const LLSD& properties, const LLSD& required = LLSD())
{
    LLSD schema;
    schema["type"]       = "object";
    schema["properties"] = properties;
    if (!required.isUndefined())
        schema["required"] = required;

    LLSD tool;
    tool["name"]        = name;
    tool["description"] = description;
    tool["inputSchema"] = schema;
    return tool;
}

LLSD FSMCPServer::buildToolsList() const
{
    LLSD tools;

    // get_viewer_status
    {
        LLSD props;
        tools.append(makeTool("get_viewer_status",
            "Get current viewer status: login state, region name, agent name.",
            props));
    }

    // get_agent_info
    {
        LLSD props;
        tools.append(makeTool("get_agent_info",
            "Get the agent's UUID, display name, global position, region, "
            "flying state, and sitting state.",
            props));
    }

    // get_nearby_avatars
    {
        LLSD props;
        props["radius"] = numProp("Search radius in meters (default: 96)");
        tools.append(makeTool("get_nearby_avatars",
            "List avatars within a given radius of the current agent.",
            props));
    }

    // get_region_info
    {
        LLSD props;
        tools.append(makeTool("get_region_info",
            "Get information about the current region: name, UUID, and origin.",
            props));
    }

    // send_chat
    {
        LLSD props;
        props["message"] = strProp("Text to send");

        LLSD type_enum;
        type_enum.append("normal");
        type_enum.append("whisper");
        type_enum.append("shout");
        LLSD type_prop;
        type_prop["type"]        = "string";
        type_prop["description"] = "Chat type: normal (default), whisper, or shout";
        type_prop["enum"]        = type_enum;
        props["type"]            = type_prop;

        LLSD req;
        req.append("message");
        tools.append(makeTool("send_chat",
            "Send a local chat message visible to nearby avatars.",
            props, req));
    }

    // teleport
    {
        LLSD props;
        props["x"]           = numProp("Global X coordinate");
        props["y"]           = numProp("Global Y coordinate");
        props["z"]           = numProp("Altitude (Z)");
        props["landmark_id"] = strProp("Landmark asset UUID (alternative to x/y/z)");
        props["home"]        = boolProp("Set to true to teleport home");
        tools.append(makeTool("teleport",
            "Teleport the agent to world coordinates, a landmark UUID, or home.",
            props));
    }

    // navigate_to
    {
        LLSD props;
        props["x"] = numProp("Global X coordinate");
        props["y"] = numProp("Global Y coordinate");
        props["z"] = numProp("Altitude (Z)");

        LLSD req;
        req.append("x");
        req.append("y");
        req.append("z");
        tools.append(makeTool("navigate_to",
            "Start the auto-pilot to walk/fly the agent to the given coordinates.",
            props, req));
    }

    // set_flying
    {
        LLSD props;
        props["flying"] = boolProp("true to enable flight, false to disable (default: true)");
        tools.append(makeTool("set_flying",
            "Enable or disable the agent's flight mode.",
            props));
    }

    // stand_up
    {
        LLSD props;
        tools.append(makeTool("stand_up",
            "Stand up if the agent is currently sitting.",
            props));
    }

    return tools;
}
