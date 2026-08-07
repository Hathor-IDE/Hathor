// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * HathorMcpServer.cpp — standalone hathor-mcp executable.
 *
 * Speaks MCP JSON-RPC stdio to the agent (MCP server role) and forwards
 * tool calls over a Unix domain socket to the Hathor process.
 *
 * Deliberately links NO JUCE modules (Req 31.1, 31.2).
 * Full implementation in Task 4.1.
 *
 * Requirements: 32.7
 */

// Stub main — real implementation in Task 4.1.
int main()
{
    // No JUCE, no audio. Full MCP server loop implemented in Task 4.1.
    return 0;
}
