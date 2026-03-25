/**
 * @file fsmcpserver.h
 * @brief Firestorm MCP (Model Context Protocol) Server
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

#pragma once

#include "llsingleton.h"
#include "llsd.h"
#include "llthread.h"

#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <map>
#include <string>
#include <vector>

/**
 * FSMCPServer
 *
 * Implements a localhost MCP server that exposes Firestorm viewer
 * functionality via JSON-RPC 2.0 over a local TCP transport.
 *
 * Enable via the FSMCPServerEnabled viewer setting.
 *
 * Supported MCP methods:
 *   initialize      – MCP handshake
 *   tools/list      – enumerate available tools
 *   tools/call      – invoke a tool
 *
 * Available tools:
 *   get_viewer_status   – login state, region, agent name
 *   get_agent_info      – position, flying, sitting, UUID
 *   get_nearby_avatars  – avatars within a configurable radius
 *   get_region_info     – region name, ID, origin
 *   send_chat           – local chat (normal / whisper / shout)
 *   teleport            – teleport via coordinates or landmark UUID
 *   navigate_to         – auto-pilot the avatar to world coordinates
 *   set_flying          – enable or disable flight
 *   stand_up            – stand up if currently sitting
 *   open_inventory      – open the My Inventory floater
 */
class FSMCPServer final : public LLSingleton<FSMCPServer>
{
    LLSINGLETON(FSMCPServer);
    ~FSMCPServer();

public:
    /** Start the localhost MCP server thread. */
    void start(U16 port);

    /** Stop the HTTP listener.  Safe to call when already stopped. */
    void stop();

    /** @return true while the server thread is active. */
    bool isRunning() const;

    /**
     * Process any tool-call requests that arrived from the background thread.
     * Must be called from the main viewer thread, once per frame.
     */
    void processQueue();

private:
    typedef LLSD (FSMCPServer::*tool_handler_t)(const LLSD& args);

    // -----------------------------------------------------------------------
    // Internal types
    // -----------------------------------------------------------------------

    /** One pending JSON-RPC call waiting for a main-thread result. */
    struct PendingCall
    {
        std::string        method;
        LLSD               params;
        std::promise<LLSD> result_promise;
    };

    struct ToolDefinition
    {
        std::string   name;
        std::string   description;
        LLSD          input_schema;
        tool_handler_t handler;
    };

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /** Queue a call from the server thread; returns a future for the result. */
    std::future<LLSD> enqueueCall(const std::string& method, const LLSD& params);

    /** Dispatch a method to the appropriate handler (main thread). */
    LLSD dispatch(const std::string& method, const LLSD& params);

    // MCP protocol handlers -------------------------------------------------
    LLSD handleInitialize(const LLSD& params);
    LLSD handleToolsList();
    LLSD handleToolsCall(const LLSD& params);

    void registerTool(const std::string& name,
                      const std::string& description,
                      const LLSD& input_schema,
                      tool_handler_t handler);
    void registerBuiltInTools();

    // Viewer tool implementations (all called from the main thread) ----------
    LLSD toolGetViewerStatus(const LLSD& args);
    LLSD toolGetAgentInfo(const LLSD& args);
    LLSD toolGetWornAttachments(const LLSD& args);
    LLSD toolGetNearbyAvatars(const LLSD& args);
    LLSD toolGetRegionInfo(const LLSD& args);
    LLSD toolSendChat(const LLSD& args);
    LLSD toolTeleport(const LLSD& args);
    LLSD toolNavigateTo(const LLSD& args);
    LLSD toolSetFlying(const LLSD& args);
    LLSD toolStandUp(const LLSD& args);
    LLSD toolOpenInventory(const LLSD& args);
    LLSD toolGetInventoryContents(const LLSD& args);
    LLSD toolMoveInventoryObject(const LLSD& args);
    LLSD toolWearInventoryItem(const LLSD& args);
    LLSD toolFindSystemFolder(const LLSD& args);
    LLSD toolCreateInventoryFolder(const LLSD& args);
    LLSD toolGetFolderCreationResult(const LLSD& args);
    LLSD toolRenameInventoryObject(const LLSD& args);
    LLSD toolListViewerCommands(const LLSD& args);
    LLSD toolExecuteViewerCommand(const LLSD& args);
    LLSD toolExecuteRlvCommand(const LLSD& args);

    /** Build the full tools-list LLSD array. */
    LLSD buildToolsList() const;

    // -----------------------------------------------------------------------
    // Background server thread
    // -----------------------------------------------------------------------
    class ServerThread;
    friend class ServerThread;

    ServerThread* mServerThread { nullptr };

    // Shared state protected by mQueueMutex
    mutable std::mutex mQueueMutex;
    std::queue<std::shared_ptr<PendingCall>> mCallQueue;

    std::vector<ToolDefinition> mTools;
    std::map<std::string, U32>  mToolIndexByName;

    // Pending async folder creations (request_id -> result UUID, null while pending)
    mutable std::mutex            mFolderCreationMutex;
    std::map<std::string, LLUUID> mFolderCreationResults;

    bool mRunning { false };
    U16  mPort    { 18080 };
};
