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
#include "llappearancemgr.h"
#include "llavatarnamecache.h"
#include "llchat.h"
#include "llcommandmanager.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "rlvhandler.h"
#include "fsnearbychathub.h"
#include "llfloaterreg.h"
#include "lluictrl.h"

// LL common
#include "llsd.h"
#include "llsdjson.h"
#include "llversioninfo.h"

// Boost JSON (already a transitive dependency via llcommon)
#include <boost/json.hpp>

// Platform sockets -----------------------------------------------------------
#if defined(LL_WINDOWS) || defined(_WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
typedef SOCKET socket_fd_t;
# define INVALID_SOCKET_FD INVALID_SOCKET
# define close_socket closesocket
# pragma comment(lib, "ws2_32.lib")
#else
# include <arpa/inet.h>
# include <netinet/in.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <unistd.h>
typedef int socket_fd_t;
# define INVALID_SOCKET_FD (-1)
# define close_socket ::close
#endif

// Standard library
#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const std::string MCP_PROTOCOL_VERSION = "2024-11-05";
static const std::string MCP_SERVER_NAME       = "firestorm-mcp";
static const int         TOOL_CALL_TIMEOUT_S   = 30;
static const LLUUID      MCP_RLV_CONTROLLER_ID("5f16a640-1ad4-4f8e-95b2-76df5460b8d5");

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

static LLSD makeInputSchema(const LLSD& properties, const LLSD& required = LLSD())
{
    LLSD schema;
    schema["type"]       = "object";
    schema["properties"] = properties;
    if (!required.isUndefined())
    {
        schema["required"] = required;
    }
    return schema;
}

static LLSD strProp(const char* description);
static LLSD numProp(const char* description);
static LLSD boolProp(const char* description);

static std::string rlvResultName(ERlvCmdRet result)
{
    switch (result)
    {
        case RLV_RET_UNKNOWN:             return "unknown";
        case RLV_RET_RETAINED:            return "retained";
        case RLV_RET_SUCCESS:             return "success";
        case RLV_RET_SUCCESS_UNSET:       return "success_unset";
        case RLV_RET_SUCCESS_DUPLICATE:   return "success_duplicate";
        case RLV_RET_SUCCESS_DEPRECATED:  return "success_deprecated";
        case RLV_RET_SUCCESS_DELAYED:     return "success_delayed";
        case RLV_RET_FAILED:              return "failed";
        case RLV_RET_FAILED_SYNTAX:       return "failed_syntax";
        case RLV_RET_FAILED_OPTION:       return "failed_option";
        case RLV_RET_FAILED_PARAM:        return "failed_param";
        case RLV_RET_FAILED_LOCK:         return "failed_lock";
        case RLV_RET_FAILED_DISABLED:     return "failed_disabled";
        case RLV_RET_FAILED_UNKNOWN:      return "failed_unknown";
        case RLV_RET_FAILED_NOSHAREDROOT: return "failed_nosharedroot";
        case RLV_RET_FAILED_DEPRECATED:   return "failed_deprecated";
        case RLV_RET_FAILED_NOBEHAVIOUR:  return "failed_nobehaviour";
        case RLV_RET_FAILED_UNHELDBEHAVIOUR: return "failed_unheldbehaviour";
        case RLV_RET_FAILED_BLOCKED:      return "failed_blocked";
        case RLV_RET_FAILED_THROTTLED:    return "failed_throttled";
        case RLV_RET_NO_PROCESSOR:        return "no_processor";
    }

    return "unrecognized";
}

static std::string normalizeRlvCommand(std::string command)
{
    LLStringUtil::trim(command);
    if (!command.empty() && command.front() == RLV_CMD_PREFIX)
    {
        command.erase(command.begin());
    }
    return command;
}

static LLSD buildRlvExecutionResult(const LLUUID& source_id, const std::string& command, ERlvCmdRet result)
{
    LLSD entry;
    entry["source_id"] = source_id.asString();
    entry["command"] = command;
    entry["result_code"] = LLSD::Integer(result);
    entry["result_name"] = rlvResultName(result);
    entry["succeeded"] = RLV_RET_SUCCEEDED(result);
    return entry;
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
    bool createListenSocket();
    void closeListenSocket();
    bool waitForClient(socket_fd_t& client_fd);
    bool readMessage(socket_fd_t client_fd, std::string& body);
    bool writeMessage(socket_fd_t client_fd, const std::string& body);
    void handleMessage(socket_fd_t client_fd, const std::string& body);
    void handleClient(socket_fd_t client_fd);

    FSMCPServer* mServer;
    U16          mPort;
    socket_fd_t  mListenFd;
};

// ---- ServerThread::run ----------------------------------------------------
void FSMCPServer::ServerThread::run()
{
#if defined(LL_WINDOWS) || defined(_WIN32)
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    if (!createListenSocket())
    {
        LL_WARNS("MCP") << "FSMCPServer: failed to bind localhost port " << mPort << LL_ENDL;
#if defined(LL_WINDOWS) || defined(_WIN32)
        WSACleanup();
#endif
        return;
    }

    LL_INFOS("MCP") << "FSMCPServer: localhost transport active on port " << mPort << LL_ENDL;

    while (!isQuitting())
    {
        socket_fd_t client_fd = INVALID_SOCKET_FD;
        if (!waitForClient(client_fd))
            continue;

        handleClient(client_fd);
        close_socket(client_fd);
    }

    closeListenSocket();

#if defined(LL_WINDOWS) || defined(_WIN32)
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

    int opt = 1;
#if defined(LL_WINDOWS) || defined(_WIN32)
    setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(mPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(mListenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        closeListenSocket();
        return false;
    }

    if (listen(mListenFd, 8) < 0)
    {
        closeListenSocket();
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

// ---- waitForClient --------------------------------------------------------
bool FSMCPServer::ServerThread::waitForClient(socket_fd_t& client_fd)
{
    client_fd = INVALID_SOCKET_FD;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(mListenFd, &read_fds);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int ret = select(static_cast<int>(mListenFd) + 1, &read_fds, nullptr, nullptr, &tv);
    if (ret <= 0 || !FD_ISSET(mListenFd, &read_fds))
    {
        return false;
    }

    struct sockaddr_in client_addr;
    std::memset(&client_addr, 0, sizeof(client_addr));
#if defined(LL_WINDOWS) || defined(_WIN32)
    int addr_len = sizeof(client_addr);
#else
    socklen_t addr_len = sizeof(client_addr);
#endif
    client_fd = accept(mListenFd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    return client_fd != INVALID_SOCKET_FD;
}

// ---- readMessage ----------------------------------------------------------
bool FSMCPServer::ServerThread::readMessage(socket_fd_t client_fd, std::string& body)
{
    std::string headers_raw;
    headers_raw.reserve(256);

    while (headers_raw.find("\r\n\r\n") == std::string::npos)
    {
        char c = 0;
        int received = recv(client_fd, &c, 1, 0);
        if (received <= 0)
            return false;
        headers_raw.push_back(c);

        if (headers_raw.size() > 16 * 1024)
            return false;
    }

    const size_t headers_end = headers_raw.find("\r\n\r\n");
    std::string header_block = headers_raw.substr(0, headers_end);

    S32 content_length = -1;
    std::istringstream hs(header_block);
    std::string line;
    while (std::getline(hs, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;

        std::string name = line.substr(0, colon);
        std::string val  = line.substr(colon + 1);

        auto ltrim = [](std::string& s)
        {
            size_t start = s.find_first_not_of(" \t");
            s = (start == std::string::npos) ? std::string() : s.substr(start);
        };
        ltrim(name);
        ltrim(val);

        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });

        if (name == "content-length")
        {
            try { content_length = (S32)std::stol(val); }
            catch (...) { content_length = -1; }
        }
    }

    if (content_length < 0)
        return false;

    body.assign((size_t)content_length, '\0');
    if (content_length == 0)
        return true;

    size_t total_read = 0;
    while (total_read < static_cast<size_t>(content_length))
    {
        int received = recv(client_fd,
                            &body[0] + total_read,
                            static_cast<int>(content_length - total_read),
                            0);
        if (received <= 0)
            return false;
        total_read += static_cast<size_t>(received);
    }

    return true;
}

// ---- writeMessage ---------------------------------------------------------
bool FSMCPServer::ServerThread::writeMessage(socket_fd_t client_fd, const std::string& body)
{
    std::ostringstream os;
    os << "Content-Length: " << body.size() << "\r\n\r\n";
    os << body;

    const std::string frame = os.str();
    size_t total_sent = 0;
    while (total_sent < frame.size())
    {
        int sent = send(client_fd,
                        frame.data() + total_sent,
                        static_cast<int>(frame.size() - total_sent),
                        0);
        if (sent <= 0)
            return false;
        total_sent += static_cast<size_t>(sent);
    }

    return true;
}

// ---- handleMessage --------------------------------------------------------
void FSMCPServer::ServerThread::handleMessage(socket_fd_t client_fd, const std::string& body)
{
    if (body.empty())
        return;

    LLSD id;
    LLSD result;

    // Parse the JSON body
    boost::json::value json_req;
    try
    {
        json_req = boost::json::parse(body);
    }
    catch (const std::exception& e)
    {
        result = makeJSONRPCError(LLSD(), -32700,
                                  std::string("Parse error: ") + e.what());
        writeMessage(client_fd, boost::json::serialize(LlsdToJson(result)));
        return;
    }

    LLSD rpc = LlsdFromJson(json_req);

    std::string method = rpc["method"].asString();
    LLSD        params = rpc.has("params") ? rpc["params"] : LLSD();
    id                = rpc.has("id") ? rpc["id"] : LLSD();

    if (method.empty())
    {
        result = makeJSONRPCError(id, -32600, "Missing 'method' field");
        writeMessage(client_fd, boost::json::serialize(LlsdToJson(result)));
        return;
    }

    // Dispatch to main thread and wait for result
    std::future<LLSD> future = mServer->enqueueCall(method, params);

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
    if (!result.has("id"))      result["id"]      = id;
    if (!result.has("jsonrpc")) result["jsonrpc"] = "2.0";

    writeMessage(client_fd, boost::json::serialize(LlsdToJson(result)));
}

// ---- handleClient ---------------------------------------------------------
void FSMCPServer::ServerThread::handleClient(socket_fd_t client_fd)
{
    while (!isQuitting())
    {
        std::string body;
        if (!readMessage(client_fd, body))
        {
            break;
        }

        handleMessage(client_fd, body);
    }
}

// ===========================================================================
// FSMCPServer – singleton implementation
// ===========================================================================

FSMCPServer::FSMCPServer()
{
    registerBuiltInTools();
}

FSMCPServer::~FSMCPServer()
{
    stop();
}

// ---------------------------------------------------------------------------
void FSMCPServer::start(U16 port)
{
    if (mRunning)
    {
        LL_WARNS("MCP") << "FSMCPServer: already running" << LL_ENDL;
        return;
    }

    mPort = port;
    mServerThread = new ServerThread(this, port);
    mServerThread->start();
    mRunning = true;

    LL_INFOS("MCP") << "FSMCPServer: starting localhost server on port " << port << LL_ENDL;
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
        // waitForInput() uses a 1-second poll interval so shutdown should be
        // near-instant.
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

    std::map<std::string, U32>::const_iterator found = mToolIndexByName.find(tool_name);
    if (found == mToolIndexByName.end())
    {
        LLSD resp;
        resp["error"]["code"]    = LLSD::Integer(-32601);
        resp["error"]["message"] = "Unknown tool: " + tool_name;
        return resp;
    }

    const ToolDefinition& tool = mTools[found->second];
    LLSD tool_result = (this->*tool.handler)(tool_args);

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

void FSMCPServer::registerTool(const std::string& name,
                               const std::string& description,
                               const LLSD& input_schema,
                               tool_handler_t handler)
{
    if (!handler)
    {
        LL_WARNS("MCP") << "FSMCPServer: refusing to register tool with null handler: "
                         << name << LL_ENDL;
        return;
    }

    if (mToolIndexByName.find(name) != mToolIndexByName.end())
    {
        LL_WARNS("MCP") << "FSMCPServer: duplicate tool registration ignored: "
                         << name << LL_ENDL;
        return;
    }

    ToolDefinition tool;
    tool.name = name;
    tool.description = description;
    tool.input_schema = input_schema;
    tool.handler = handler;

    mTools.push_back(tool);
    mToolIndexByName[name] = static_cast<U32>(mTools.size() - 1);
}

void FSMCPServer::registerBuiltInTools()
{
    LLSD props;

    registerTool(
        "get_viewer_status",
        "Get current viewer status: login state, region name, agent name.",
        makeInputSchema(props),
        &FSMCPServer::toolGetViewerStatus);

    props = LLSD();
    registerTool(
        "get_agent_info",
        "Get the agent's UUID, display name, global position, region, flying state, and sitting state.",
        makeInputSchema(props),
        &FSMCPServer::toolGetAgentInfo);

    props = LLSD();
    props["name_filter"] = strProp("Optional substring filter applied to attachment item names");
    registerTool(
        "get_worn_attachments",
        "List currently worn attachments, including live object UUIDs and attachment item UUIDs.",
        makeInputSchema(props),
        &FSMCPServer::toolGetWornAttachments);

    props = LLSD();
    props["radius"] = numProp("Search radius in meters (default: 96)");
    registerTool(
        "get_nearby_avatars",
        "List avatars within a given radius of the current agent.",
        makeInputSchema(props),
        &FSMCPServer::toolGetNearbyAvatars);

    props = LLSD();
    registerTool(
        "get_region_info",
        "Get information about the current region: name, UUID, and origin.",
        makeInputSchema(props),
        &FSMCPServer::toolGetRegionInfo);

    props = LLSD();
    props["message"] = strProp("Text to send");
    {
        LLSD type_enum;
        type_enum.append("normal");
        type_enum.append("whisper");
        type_enum.append("shout");

        LLSD type_prop;
        type_prop["type"] = "string";
        type_prop["description"] = "Chat type: normal (default), whisper, or shout";
        type_prop["enum"] = type_enum;
        props["type"] = type_prop;
    }
    {
        LLSD required;
        required.append("message");
        registerTool(
            "send_chat",
            "Send a local chat message visible to nearby avatars.",
            makeInputSchema(props, required),
            &FSMCPServer::toolSendChat);
    }

    props = LLSD();
    props["x"] = numProp("Global X coordinate");
    props["y"] = numProp("Global Y coordinate");
    props["z"] = numProp("Altitude (Z)");
    props["landmark_id"] = strProp("Landmark asset UUID (alternative to x/y/z)");
    props["home"] = boolProp("Set to true to teleport home");
    registerTool(
        "teleport",
        "Teleport the agent to world coordinates, a landmark UUID, or home.",
        makeInputSchema(props),
        &FSMCPServer::toolTeleport);

    props = LLSD();
    props["x"] = numProp("Global X coordinate");
    props["y"] = numProp("Global Y coordinate");
    props["z"] = numProp("Altitude (Z)");
    {
        LLSD required;
        required.append("x");
        required.append("y");
        required.append("z");
        registerTool(
            "navigate_to",
            "Start the auto-pilot to walk/fly the agent to the given coordinates.",
            makeInputSchema(props, required),
            &FSMCPServer::toolNavigateTo);
    }

    props = LLSD();
    props["flying"] = boolProp("true to enable flight, false to disable (default: true)");
    registerTool(
        "set_flying",
        "Enable or disable the agent's flight mode.",
        makeInputSchema(props),
        &FSMCPServer::toolSetFlying);

    props = LLSD();
    registerTool(
        "stand_up",
        "Stand up if the agent is currently sitting.",
        makeInputSchema(props),
        &FSMCPServer::toolStandUp);

    props = LLSD();
    registerTool(
        "open_inventory",
        "Open the My Inventory floater.",
        makeInputSchema(props),
        &FSMCPServer::toolOpenInventory);

    props = LLSD();
    props["folder_id"] = strProp("Folder UUID to inspect (default: inventory root)");
    props["recursive"] = boolProp("If true, include all descendants (default: false)");
    props["name_filter"] = strProp("Optional substring filter applied to item/category names");
    props["limit"] = numProp("Maximum total results returned when recursive (default: unlimited)");
    props["include_trash"] = boolProp("If true, include trash descendants in recursive mode (default: false)");
    registerTool(
        "get_inventory_contents",
        "List inventory categories and items inside a folder.",
        makeInputSchema(props),
        &FSMCPServer::toolGetInventoryContents);

    props = LLSD();
    props["object_id"] = strProp("Inventory item or category UUID to move");
    props["target_folder_id"] = strProp("Destination folder UUID");
    {
        LLSD required;
        required.append("object_id");
        required.append("target_folder_id");
        registerTool(
            "move_inventory_object",
            "Move an inventory item or folder to a destination folder.",
            makeInputSchema(props, required),
            &FSMCPServer::toolMoveInventoryObject);
    }

    props = LLSD();
    props["item_id"] = strProp("Inventory item UUID to wear");
    props["replace"] = boolProp("If true, replace conflicting worn items instead of adding (default: false)");
    {
        LLSD required;
        required.append("item_id");
        registerTool(
            "wear_inventory_item",
            "Wear an inventory item on the avatar using the viewer's native appearance pipeline.",
            makeInputSchema(props, required),
            &FSMCPServer::toolWearInventoryItem);
    }

    // find_system_folder ----------------------------------------------------
    props = LLSD();
    {
        LLSD type_enum;
        for (const char* t : {"texture","sound","callingcard","landmark","clothing",
                               "object","notecard","script","bodypart","trash",
                               "snapshot","lost_and_found","animation","gesture","my_outfits"})
            type_enum.append(std::string(t));
        LLSD type_prop;
        type_prop["type"] = "string";
        type_prop["description"] = "System folder type. Omit to return all system folders.";
        type_prop["enum"] = type_enum;
        props["type"] = type_prop;
    }
    registerTool(
        "find_system_folder",
        "Return the UUID(s) of built-in system inventory folders by type. "
        "Pass a type name to get one folder; omit type to get all system folders.",
        makeInputSchema(props),
        &FSMCPServer::toolFindSystemFolder);

    // create_inventory_folder -----------------------------------------------
    props = LLSD();
    props["name"] = strProp("Name for the new folder");
    props["parent_folder_id"] = strProp("Parent folder UUID (default: inventory root)");
    {
        LLSD required;
        required.append("name");
        registerTool(
            "create_inventory_folder",
            "Create a new folder inside the inventory. "
            "Returns a request_id; poll get_folder_creation_result to obtain the new folder UUID.",
            makeInputSchema(props, required),
            &FSMCPServer::toolCreateInventoryFolder);
    }

    // get_folder_creation_result -------------------------------------------
    props = LLSD();
    props["request_id"] = strProp("request_id returned by create_inventory_folder");
    {
        LLSD required;
        required.append("request_id");
        registerTool(
            "get_folder_creation_result",
            "Poll for the result of a create_inventory_folder request. "
            "Returns {pending:true} while waiting, or {folder_id:\"...\",pending:false} when ready.",
            makeInputSchema(props, required),
            &FSMCPServer::toolGetFolderCreationResult);
    }

    // rename_inventory_object -----------------------------------------------
    props = LLSD();
    props["object_id"] = strProp("Item or category UUID to rename");
    props["new_name"] = strProp("New name for the item or folder");
    {
        LLSD required;
        required.append("object_id");
        required.append("new_name");
        registerTool(
            "rename_inventory_object",
            "Rename an inventory item or folder.",
            makeInputSchema(props, required),
            &FSMCPServer::toolRenameInventoryObject);
    }

    props = LLSD();
    props["include_non_executable"] = boolProp("Include commands with no execute callback (default: false)");
    registerTool(
        "list_viewer_commands",
        "List viewer command metadata loaded from commands.xml.",
        makeInputSchema(props),
        &FSMCPServer::toolListViewerCommands);

    props = LLSD();
    props["command_name"] = strProp("Viewer command name from commands.xml");
    {
        LLSD parameters_prop;
        parameters_prop["type"] = "object";
        parameters_prop["description"] = "Optional parameter overrides to pass to execute callback";
        props["parameters"] = parameters_prop;
    }
    props["override_parameters"] = boolProp("If true, parameters replace default execute_parameters; otherwise they merge as object overlays");
    props["call_if_enabled"] = boolProp("If true (default), run command's is_enabled callback before execute");
    {
        LLSD required;
        required.append("command_name");
        registerTool(
            "execute_viewer_command",
            "Execute a viewer command by name via the UI commit callback registry.",
            makeInputSchema(props, required),
            &FSMCPServer::toolExecuteViewerCommand);
    }

    props = LLSD();
    props["command"] = strProp("Single RLVa command to execute. Leading '@' is optional.");
    {
        LLSD commands_prop;
        commands_prop["type"] = "array";
        commands_prop["description"] = "Optional batch of RLVa commands to execute in order.";
        LLSD items_prop;
        items_prop["type"] = "string";
        commands_prop["items"] = items_prop;
        props["commands"] = commands_prop;
    }
    props["source_id"] = strProp("Optional controller UUID used as the RLVa command source. Defaults to a stable MCP controller UUID.");
    props["from_object"] = boolProp("If true, execute as though the command originated from an in-world object (default: false).");
    registerTool(
        "execute_rlv_command",
        "Execute one or more RLVa commands through the viewer's native RLVa parser.",
        makeInputSchema(props),
        &FSMCPServer::toolExecuteRlvCommand);
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

LLSD FSMCPServer::toolGetWornAttachments(const LLSD& args)
{
    LLSD r;

    if (!isAgentAvatarValid())
    {
        r["success"] = false;
        r["error"] = "Agent avatar not available";
        return r;
    }

    std::string name_filter = args.has("name_filter") ? args["name_filter"].asString() : std::string();
    if (!name_filter.empty())
    {
        LLStringUtil::toLower(name_filter);
    }

    LLSD attachments;
    S32 count = 0;

    for (LLVOAvatar::attachment_map_t::const_iterator it = gAgentAvatarp->mAttachmentPoints.begin();
         it != gAgentAvatarp->mAttachmentPoints.end(); ++it)
    {
        const LLViewerJointAttachment* attachment = it->second;
        if (!attachment)
        {
            continue;
        }

        for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator obj_it = attachment->mAttachedObjects.begin();
             obj_it != attachment->mAttachedObjects.end(); ++obj_it)
        {
            const LLViewerObject* object = *obj_it;
            if (!object)
            {
                continue;
            }

            const std::string item_name = object->getAttachmentItemName();
            if (!name_filter.empty())
            {
                std::string haystack = item_name;
                LLStringUtil::toLower(haystack);
                if (haystack.find(name_filter) == std::string::npos)
                {
                    continue;
                }
            }

            LLSD entry;
            entry["object_id"] = object->getID().asString();
            entry["item_id"] = object->getAttachmentItemID().asString();
            entry["item_name"] = item_name;
            entry["attachment_point"] = attachment->getName();
            entry["attachment_group"] = LLSD::Integer(attachment->getGroup());
            entry["is_hud"] = object->isHUDAttachment();
            attachments.append(entry);
            ++count;
        }
    }

    r["success"] = true;
    r["count"] = LLSD::Integer(count);
    r["attachments"] = attachments;
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

LLSD FSMCPServer::toolOpenInventory(const LLSD& /*args*/)
{
    LLSD r;

    LLFloater* inv_floater = LLFloaterReg::showInstance("inventory");
    if (!inv_floater)
    {
        r["success"] = false;
        r["error"]   = "Could not open inventory floater";
        return r;
    }

    r["success"] = true;
    return r;
}

namespace
{
class InventoryNameFilterCollector final : public LLInventoryCollectFunctor
{
public:
    InventoryNameFilterCollector(const std::string& name_filter, S32 limit)
    : mNameFilter(name_filter)
    , mLimit(limit)
    , mCount(0)
    {}

    bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
    {
        if (mLimit > 0 && mCount >= mLimit)
        {
            return false;
        }

        std::string name;
        if (cat)
        {
            name = cat->getName();
        }
        else if (item)
        {
            name = item->getName();
        }

        const bool passes = mNameFilter.empty() || (name.find(mNameFilter) != std::string::npos);
        if (passes)
        {
            ++mCount;
        }
        return passes;
    }

    bool exceedsLimit() override
    {
        return mLimit > 0 && mCount >= mLimit;
    }

private:
    std::string mNameFilter;
    S32 mLimit;
    S32 mCount;
};

LLSD inventoryCategoryToLLSD(const LLViewerInventoryCategory* cat)
{
    LLSD out;
    out["id"] = cat->getUUID().asString();
    out["name"] = cat->getName();
    out["parent_id"] = cat->getParentUUID().asString();
    out["type"] = LLFolderType::lookup(cat->getPreferredType());
    out["is_link"] = cat->getIsLinkType();
    return out;
}

LLSD inventoryItemToLLSD(const LLViewerInventoryItem* item)
{
    LLSD out;
    out["id"] = item->getUUID().asString();
    out["name"] = item->getName();
    out["parent_id"] = item->getParentUUID().asString();
    out["desc"] = item->getDescription();
    out["inv_type"] = LLInventoryType::lookup(item->getInventoryType());
    out["asset_type"] = LLAssetType::lookup(item->getType());
    out["asset_id"] = item->getAssetUUID().asString();
    out["is_link"] = item->getIsLinkType();
    out["linked_id"] = item->getLinkedUUID().asString();
    return out;
}
}

LLSD FSMCPServer::toolGetInventoryContents(const LLSD& args)
{
    LLSD r;

    LLUUID folder_id = gInventory.getRootFolderID();
    if (folder_id.isNull())
    {
        folder_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_ROOT_INVENTORY);
    }

    if (args.has("folder_id") && !args["folder_id"].asString().empty())
    {
        folder_id.set(args["folder_id"].asString(), false);
    }

    if (folder_id.isNull())
    {
        r["success"] = false;
        r["error"] = "Inventory root is unavailable (viewer may not be logged in yet).";
        return r;
    }

    LLViewerInventoryCategory* root_cat = gInventory.getCategory(folder_id);
    if (!root_cat)
    {
        r["success"] = false;
        r["error"] = "Folder not found: " + folder_id.asString();
        return r;
    }

    const bool recursive = args.has("recursive") ? args["recursive"].asBoolean() : false;
    const bool include_trash = args.has("include_trash") ? args["include_trash"].asBoolean() : false;
    const std::string name_filter = args.has("name_filter") ? args["name_filter"].asString() : std::string();
    const S32 limit = args.has("limit") ? std::max(args["limit"].asInteger(), 0) : 0;

    LLInventoryModel::cat_array_t categories;
    LLInventoryModel::item_array_t items;

    if (recursive)
    {
        InventoryNameFilterCollector collector(name_filter, limit);
        gInventory.collectDescendentsIf(
            folder_id,
            categories,
            items,
            include_trash ? LLInventoryModel::INCLUDE_TRASH : LLInventoryModel::EXCLUDE_TRASH,
            collector);
    }
    else
    {
        LLInventoryModel::cat_array_t* cat_ptr = nullptr;
        LLInventoryModel::item_array_t* item_ptr = nullptr;
        gInventory.getDirectDescendentsOf(folder_id, cat_ptr, item_ptr);

        if (cat_ptr)
        {
            categories = *cat_ptr;
        }
        if (item_ptr)
        {
            items = *item_ptr;
        }

        if (!name_filter.empty())
        {
            LLInventoryModel::cat_array_t filtered_categories;
            LLInventoryModel::item_array_t filtered_items;

            for (LLViewerInventoryCategory* cat : categories)
            {
                if (cat && cat->getName().find(name_filter) != std::string::npos)
                {
                    filtered_categories.push_back(cat);
                }
            }
            for (LLViewerInventoryItem* item : items)
            {
                if (item && item->getName().find(name_filter) != std::string::npos)
                {
                    filtered_items.push_back(item);
                }
            }

            categories.swap(filtered_categories);
            items.swap(filtered_items);
        }

        if (limit > 0)
        {
            S32 remaining = limit;
            if ((S32)categories.size() > remaining)
            {
                categories.resize(remaining);
                items.clear();
            }
            else
            {
                remaining -= (S32)categories.size();
                if ((S32)items.size() > remaining)
                {
                    items.resize(remaining);
                }
            }
        }
    }

    LLSD category_list;
    for (LLViewerInventoryCategory* cat : categories)
    {
        if (cat)
        {
            category_list.append(inventoryCategoryToLLSD(cat));
        }
    }

    LLSD item_list;
    for (LLViewerInventoryItem* item : items)
    {
        if (item)
        {
            item_list.append(inventoryItemToLLSD(item));
        }
    }

    r["success"] = true;
    r["folder_id"] = folder_id.asString();
    r["recursive"] = recursive;
    r["categories"] = category_list;
    r["items"] = item_list;
    r["category_count"] = LLSD::Integer((S32)category_list.size());
    r["item_count"] = LLSD::Integer((S32)item_list.size());
    return r;
}

LLSD FSMCPServer::toolMoveInventoryObject(const LLSD& args)
{
    LLSD r;

    if (!args.has("object_id") || !args.has("target_folder_id"))
    {
        r["success"] = false;
        r["error"] = "Missing required parameters: object_id, target_folder_id";
        return r;
    }

    LLUUID object_id(args["object_id"].asString());
    LLUUID target_folder_id(args["target_folder_id"].asString());

    if (object_id.isNull() || target_folder_id.isNull())
    {
        r["success"] = false;
        r["error"] = "Invalid UUID in object_id or target_folder_id";
        return r;
    }

    LLViewerInventoryCategory* target_folder = gInventory.getCategory(target_folder_id);
    if (!target_folder)
    {
        r["success"] = false;
        r["error"] = "Target folder not found: " + target_folder_id.asString();
        return r;
    }

    if (LLViewerInventoryItem* item = gInventory.getItem(object_id))
    {
        gInventory.changeItemParent(item, target_folder_id, false);
        r["success"] = true;
        r["object_type"] = "item";
        r["object_id"] = object_id.asString();
        r["target_folder_id"] = target_folder_id.asString();
        return r;
    }

    if (LLViewerInventoryCategory* cat = gInventory.getCategory(object_id))
    {
        gInventory.changeCategoryParent(cat, target_folder_id, false);
        r["success"] = true;
        r["object_type"] = "category";
        r["object_id"] = object_id.asString();
        r["target_folder_id"] = target_folder_id.asString();
        return r;
    }

    r["success"] = false;
    r["error"] = "Inventory object not found: " + object_id.asString();
    return r;
}

// ---------------------------------------------------------------------------
// System-folder type name → LLFolderType::EType helper
// ---------------------------------------------------------------------------
static bool folderTypeFromString(const std::string& name, LLFolderType::EType& out_type)
{
    static const std::map<std::string, LLFolderType::EType> sMap = {
        {"texture",        LLFolderType::FT_TEXTURE},
        {"sound",          LLFolderType::FT_SOUND},
        {"callingcard",    LLFolderType::FT_CALLINGCARD},
        {"landmark",       LLFolderType::FT_LANDMARK},
        {"clothing",       LLFolderType::FT_CLOTHING},
        {"object",         LLFolderType::FT_OBJECT},
        {"notecard",       LLFolderType::FT_NOTECARD},
        {"script",         LLFolderType::FT_LSL_TEXT},
        {"bodypart",       LLFolderType::FT_BODYPART},
        {"trash",          LLFolderType::FT_TRASH},
        {"snapshot",       LLFolderType::FT_SNAPSHOT_CATEGORY},
        {"lost_and_found", LLFolderType::FT_LOST_AND_FOUND},
        {"animation",      LLFolderType::FT_ANIMATION},
        {"gesture",        LLFolderType::FT_GESTURE},
        {"my_outfits",     LLFolderType::FT_MY_OUTFITS},
    };
    auto it = sMap.find(name);
    if (it == sMap.end())
        return false;
    out_type = it->second;
    return true;
}

LLSD FSMCPServer::toolWearInventoryItem(const LLSD& args)
{
    LLSD r;

    if (!args.has("item_id"))
    {
        r["success"] = false;
        r["error"] = "Missing required parameter: item_id";
        return r;
    }

    LLUUID item_id(args["item_id"].asString());
    if (item_id.isNull())
    {
        r["success"] = false;
        r["error"] = "Invalid item_id";
        return r;
    }

    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!item)
    {
        r["success"] = false;
        r["error"] = "Inventory item not found: " + item_id.asString();
        return r;
    }

    const bool replace = args.has("replace") ? args["replace"].asBoolean() : false;
    LLAppearanceMgr::instance().wearItemOnAvatar(item_id, true, replace);

    r["success"] = true;
    r["item_id"] = item_id.asString();
    r["item_name"] = item->getName();
    r["replace"] = replace;
    return r;
}

LLSD FSMCPServer::toolFindSystemFolder(const LLSD& args)
{
    static const std::vector<std::pair<std::string, LLFolderType::EType>> sAllTypes = {
        {"texture",        LLFolderType::FT_TEXTURE},
        {"sound",          LLFolderType::FT_SOUND},
        {"callingcard",    LLFolderType::FT_CALLINGCARD},
        {"landmark",       LLFolderType::FT_LANDMARK},
        {"clothing",       LLFolderType::FT_CLOTHING},
        {"object",         LLFolderType::FT_OBJECT},
        {"notecard",       LLFolderType::FT_NOTECARD},
        {"script",         LLFolderType::FT_LSL_TEXT},
        {"bodypart",       LLFolderType::FT_BODYPART},
        {"trash",          LLFolderType::FT_TRASH},
        {"snapshot",       LLFolderType::FT_SNAPSHOT_CATEGORY},
        {"lost_and_found", LLFolderType::FT_LOST_AND_FOUND},
        {"animation",      LLFolderType::FT_ANIMATION},
        {"gesture",        LLFolderType::FT_GESTURE},
        {"my_outfits",     LLFolderType::FT_MY_OUTFITS},
    };

    std::string type_name = args.has("type") ? args["type"].asString() : std::string();

    if (type_name.empty())
    {
        // Return all system folders
        LLSD folders;
        for (const auto& kv : sAllTypes)
        {
            LLUUID fid = gInventory.findCategoryUUIDForType(kv.second);
            if (fid.isNull())
                continue;
            LLSD entry;
            entry["type"] = kv.first;
            entry["folder_id"] = fid.asString();
            LLViewerInventoryCategory* cat = gInventory.getCategory(fid);
            entry["name"] = cat ? cat->getName() : kv.first;
            folders.append(entry);
        }
        LLSD r;
        r["success"] = true;
        r["folders"] = folders;
        return r;
    }

    LLFolderType::EType ft;
    if (!folderTypeFromString(type_name, ft))
    {
        LLSD r;
        r["success"] = false;
        r["error"] = "Unknown folder type: " + type_name;
        return r;
    }

    LLUUID fid = gInventory.findCategoryUUIDForType(ft);
    LLSD r;
    r["success"] = true;
    r["type"] = type_name;
    r["folder_id"] = fid.asString();
    if (!fid.isNull())
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(fid);
        r["name"] = cat ? cat->getName() : type_name;
    }
    return r;
}

LLSD FSMCPServer::toolCreateInventoryFolder(const LLSD& args)
{
    LLSD r;

    if (!args.has("name") || args["name"].asString().empty())
    {
        r["success"] = false;
        r["error"] = "Missing required parameter: name";
        return r;
    }

    std::string name = args["name"].asString();

    LLUUID parent_id = gInventory.getRootFolderID();
    if (args.has("parent_folder_id") && !args["parent_folder_id"].asString().empty())
    {
        LLUUID pid(args["parent_folder_id"].asString());
        if (!pid.isNull())
            parent_id = pid;
    }

    if (parent_id.isNull())
    {
        r["success"] = false;
        r["error"] = "Inventory root unavailable (not logged in?)";
        return r;
    }

    if (!gInventory.getCategory(parent_id))
    {
        r["success"] = false;
        r["error"] = "Parent folder not found: " + parent_id.asString();
        return r;
    }

    // Generate a unique request ID to allow polling for the result
    LLUUID request_uuid;
    request_uuid.generate();
    std::string request_id = request_uuid.asString();

    {
        std::lock_guard<std::mutex> lock(mFolderCreationMutex);
        mFolderCreationResults[request_id] = LLUUID::null; // null = pending
    }

    // Fire the async creation; callback will store the new folder UUID
    gInventory.createNewCategory(
        parent_id,
        LLFolderType::FT_NONE,
        name,
        [this, request_id](const LLUUID& new_folder_id)
        {
            std::lock_guard<std::mutex> lock(mFolderCreationMutex);
            auto it = mFolderCreationResults.find(request_id);
            if (it != mFolderCreationResults.end())
            {
                // Store a sentinel non-null UUID if creation failed so callers
                // can distinguish "still pending" from "failed"
                it->second = new_folder_id.isNull() ? LLUUID("ffffffff-ffff-ffff-ffff-ffffffffffff")
                                                     : new_folder_id;
            }
        });

    r["success"] = true;
    r["request_id"] = request_id;
    r["pending"] = true;
    r["note"] = "Poll get_folder_creation_result with the request_id to obtain the new folder UUID.";
    return r;
}

LLSD FSMCPServer::toolGetFolderCreationResult(const LLSD& args)
{
    std::string request_id = args.has("request_id") ? args["request_id"].asString() : std::string();

    LLSD r;
    if (request_id.empty())
    {
        r["success"] = false;
        r["error"] = "Missing required parameter: request_id";
        return r;
    }

    std::lock_guard<std::mutex> lock(mFolderCreationMutex);
    auto it = mFolderCreationResults.find(request_id);
    if (it == mFolderCreationResults.end())
    {
        r["success"] = false;
        r["error"] = "Unknown request_id (already consumed or never created)";
        return r;
    }

    if (it->second.isNull())
    {
        // Still pending
        r["success"] = true;
        r["pending"] = true;
        r["request_id"] = request_id;
        return r;
    }

    static const LLUUID FAILED_SENTINEL("ffffffff-ffff-ffff-ffff-ffffffffffff");
    bool failed = (it->second == FAILED_SENTINEL);
    LLUUID folder_id = failed ? LLUUID::null : it->second;
    mFolderCreationResults.erase(it); // consume

    r["success"] = !failed;
    r["pending"] = false;
    r["request_id"] = request_id;
    if (failed)
        r["error"] = "Folder creation failed on the server side";
    else
        r["folder_id"] = folder_id.asString();
    return r;
}

LLSD FSMCPServer::toolRenameInventoryObject(const LLSD& args)
{
    LLSD r;

    if (!args.has("object_id") || !args.has("new_name"))
    {
        r["success"] = false;
        r["error"] = "Missing required parameters: object_id, new_name";
        return r;
    }

    LLUUID object_id(args["object_id"].asString());
    std::string new_name = args["new_name"].asString();

    if (object_id.isNull() || new_name.empty())
    {
        r["success"] = false;
        r["error"] = "Invalid object_id or empty new_name";
        return r;
    }

    if (LLViewerInventoryItem* item = gInventory.getItem(object_id))
    {
        LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
        new_item->rename(new_name);
        new_item->updateServer(false);
        gInventory.updateItem(new_item);
        gInventory.notifyObservers();
        r["success"] = true;
        r["object_type"] = "item";
        r["object_id"] = object_id.asString();
        r["new_name"] = new_name;
        return r;
    }

    if (LLViewerInventoryCategory* cat = gInventory.getCategory(object_id))
    {
        LLPointer<LLViewerInventoryCategory> new_cat = new LLViewerInventoryCategory(cat);
        new_cat->rename(new_name);
        new_cat->updateServer(false);
        gInventory.updateCategory(new_cat);
        gInventory.notifyObservers();
        r["success"] = true;
        r["object_type"] = "category";
        r["object_id"] = object_id.asString();
        r["new_name"] = new_name;
        return r;
    }

    r["success"] = false;
    r["error"] = "Inventory object not found: " + object_id.asString();
    return r;
}

LLSD FSMCPServer::toolListViewerCommands(const LLSD& args)
{
    LLSD r;

    LLCommandManager& command_manager = LLCommandManager::instance();
    bool include_non_executable = args.has("include_non_executable")
        ? args["include_non_executable"].asBoolean()
        : false;

    LLSD commands;
    S32 count = 0;

    for (U32 i = 0; i < command_manager.commandCount(); ++i)
    {
        LLCommand* command = command_manager.getCommand(i);
        if (!command)
        {
            continue;
        }

        const std::string& execute_fn = command->executeFunctionName();
        if (!include_non_executable && execute_fn.empty())
        {
            continue;
        }

        LLSD item;
        item["name"] = command->name();
        item["label_ref"] = command->labelRef();
        item["tooltip_ref"] = command->tooltipRef();
        item["icon"] = command->icon();
        item["available_in_toybox"] = command->availableInToybox();
        item["execute_function"] = execute_fn;
        item["execute_parameters"] = command->executeParameters();
        item["is_enabled_function"] = command->isEnabledFunctionName();
        item["is_enabled_parameters"] = command->isEnabledParameters();
        item["is_running_function"] = command->isRunningFunctionName();
        item["is_running_parameters"] = command->isRunningParameters();
        item["control_name"] = command->controlVariableName();
        item["checkbox_control"] = command->checkboxControlVariableName();

        commands.append(item);
        ++count;
    }

    r["commands"] = commands;
    r["count"] = LLSD::Integer(count);
    return r;
}

LLSD FSMCPServer::toolExecuteViewerCommand(const LLSD& args)
{
    LLSD r;

    const std::string command_name = args.has("command_name") ? args["command_name"].asString() : std::string();
    if (command_name.empty())
    {
        r["success"] = false;
        r["error"] = "Missing required parameter: command_name";
        return r;
    }

    LLCommand* command = LLCommandManager::instance().getCommand(command_name);
    if (!command)
    {
        r["success"] = false;
        r["error"] = "Unknown command: " + command_name;
        return r;
    }

    const std::string& execute_function_name = command->executeFunctionName();
    if (execute_function_name.empty())
    {
        r["success"] = false;
        r["error"] = "Command has no execute_function: " + command_name;
        return r;
    }

    bool call_if_enabled = args.has("call_if_enabled") ? args["call_if_enabled"].asBoolean() : true;
    if (call_if_enabled)
    {
        const std::string& is_enabled_fn_name = command->isEnabledFunctionName();
        if (!is_enabled_fn_name.empty())
        {
            LLUICtrl::enable_callback_t* enabled_fn = LLUICtrl::EnableCallbackRegistry::getValue(is_enabled_fn_name);
            if (!enabled_fn)
            {
                r["success"] = false;
                r["error"] = "is_enabled callback not found: " + is_enabled_fn_name;
                return r;
            }

            const bool is_enabled = (*enabled_fn)(nullptr, command->isEnabledParameters());
            if (!is_enabled)
            {
                r["success"] = false;
                r["error"] = "Command currently disabled: " + command_name;
                return r;
            }
        }
    }

    LLUICtrl::commit_callback_t* execute_fn = LLUICtrl::CommitCallbackRegistry::getValue(execute_function_name);
    if (!execute_fn)
    {
        r["success"] = false;
        r["error"] = "execute callback not found: " + execute_function_name;
        return r;
    }

    LLSD execute_params = command->executeParameters();
    if (args.has("parameters"))
    {
        const LLSD supplied = args["parameters"];
        const bool override = args.has("override_parameters") ? args["override_parameters"].asBoolean() : false;

        if (override)
        {
            execute_params = supplied;
        }
        else if (execute_params.isMap() && supplied.isMap())
        {
            for (LLSD::map_const_iterator it = supplied.beginMap(); it != supplied.endMap(); ++it)
            {
                execute_params[it->first] = it->second;
            }
        }
        else
        {
            execute_params = supplied;
        }
    }

    try
    {
        (*execute_fn)(nullptr, execute_params);
    }
    catch (const std::exception& e)
    {
        r["success"] = false;
        r["error"] = std::string("execute callback threw exception: ") + e.what();
        return r;
    }

    r["success"] = true;
    r["command_name"] = command_name;
    r["execute_function"] = execute_function_name;
    r["parameters_used"] = execute_params;
    return r;
}

LLSD FSMCPServer::toolExecuteRlvCommand(const LLSD& args)
{
    LLSD r;

    if (!RlvHandler::isEnabled())
    {
        r["success"] = false;
        r["error"] = "RLVa is not enabled in the viewer";
        return r;
    }

    LLUUID source_id = MCP_RLV_CONTROLLER_ID;
    if (args.has("source_id"))
    {
        source_id = LLUUID(args["source_id"].asString());
        if (source_id.isNull())
        {
            r["success"] = false;
            r["error"] = "Invalid source_id";
            return r;
        }
    }

    const bool from_object = args.has("from_object") ? args["from_object"].asBoolean() : false;

    std::vector<std::string> commands;
    if (args.has("command"))
    {
        const std::string command = normalizeRlvCommand(args["command"].asString());
        if (!command.empty())
        {
            commands.push_back(command);
        }
    }
    if (args.has("commands"))
    {
        const LLSD command_list = args["commands"];
        if (!command_list.isArray())
        {
            r["success"] = false;
            r["error"] = "commands must be an array of strings";
            return r;
        }

        for (LLSD::array_const_iterator it = command_list.beginArray(); it != command_list.endArray(); ++it)
        {
            if (!it->isString())
            {
                r["success"] = false;
                r["error"] = "commands must contain only strings";
                return r;
            }

            const std::string command = normalizeRlvCommand(it->asString());
            if (!command.empty())
            {
                commands.push_back(command);
            }
        }
    }

    if (commands.empty())
    {
        r["success"] = false;
        r["error"] = "Provide command or commands";
        return r;
    }

    LLSD results;
    bool all_succeeded = true;
    for (std::vector<std::string>::const_iterator it = commands.begin(); it != commands.end(); ++it)
    {
        const std::string& command = *it;
        ERlvCmdRet result = RlvHandler::instance().processCommand(source_id, command, from_object);
        LLSD entry = buildRlvExecutionResult(source_id, command, result);
        results.append(entry);

        if (!RLV_RET_SUCCEEDED(result))
        {
            all_succeeded = false;
        }
    }

    r["success"] = all_succeeded;
    r["source_id"] = source_id.asString();
    r["from_object"] = from_object;
    r["count"] = LLSD::Integer(static_cast<S32>(commands.size()));
    r["note"] = "RLVa reply-channel payloads are executed by the viewer but are not captured by this MCP tool.";
    r["results"] = results;
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

LLSD FSMCPServer::buildToolsList() const
{
    LLSD tools;

    for (std::vector<ToolDefinition>::const_iterator it = mTools.begin(); it != mTools.end(); ++it)
    {
        LLSD tool;
        tool["name"] = it->name;
        tool["description"] = it->description;
        tool["inputSchema"] = it->input_schema;
        tools.append(tool);
    }

    return tools;
}
