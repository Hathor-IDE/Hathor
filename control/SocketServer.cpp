// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SocketServer.cpp — Unix-socket accept/read loop implementation.
 *
 * Requirements: Phase 2.5 H0
 */

#include "SocketServer.hpp"

#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <mutex>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hathor::control {

namespace {

/// Write @p s to @p fd in full, looping over short writes / EINTR.
static bool writeAll(int fd, const std::string& s)
{
    std::size_t off = 0;
    while (off < s.size())
    {
        const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false; // client disconnected
        }
        if (n == 0)
            return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

/// Shared, per-command response slot so a late async callback (e.g. the
/// WorkerThread finishing a set-pattern) can never touch freed accept-loop
/// stack state even after the loop has been torn down.
struct Outcome {
    std::mutex               mtx;
    std::condition_variable  cv;
    std::string              response;
    bool                     done = false;
};

/// One accepted connection: its fd and any partially-read line.
struct Connection {
    int         fd  = -1;
    std::string buf;
};

/// Dispatch every complete (newline-terminated) line in @p c.buf, writing each
/// response back to @p c.fd.  Partial lines are retained in @p c.buf.
/// Returns false if the connection must be closed (write failure).
static bool processLines(Connection& c,
                         const std::atomic<bool>& stop,
                         const SocketDispatcher& dispatcher)
{
    std::size_t nl;
    while ((nl = c.buf.find('\n')) != std::string::npos)
    {
        std::string line = c.buf.substr(0, nl);
        c.buf.erase(0, nl + 1);

        // Trim trailing CR / spaces so `\r\n` terminates cleanly.
        std::size_t end = line.size();
        while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t'
                           || line[end - 1] == '\r'))
            --end;
        line.resize(end);

        if (line.empty())
            continue;

        auto outcome = std::make_shared<Outcome>();

        dispatcher(line, [outcome](std::string resp) {
            std::lock_guard<std::mutex> lock(outcome->mtx);
            outcome->response = std::move(resp);
            outcome->done     = true;
            outcome->cv.notify_all();
        });

        // Wait for the response.  Synchronous commands deliver immediately;
        // set-pattern is delivered later by the WorkerThread.  Bounded so a
        // driver that never responds cannot block shutdown forever.
        std::string response;
        {
            std::unique_lock<std::mutex> lock(outcome->mtx);
            outcome->cv.wait_for(lock, std::chrono::milliseconds(5000),
                [&] { return outcome->done || stop.load(); });
            if (outcome->done)
                response = outcome->response;
        }

        if (!response.empty() && !stop.load())
        {
            response += '\n';
            if (!writeAll(c.fd, response))
                return false; // client gone
        }
    }
    return true;
}

} // anonymous namespace

void runSocketAcceptLoop(int listenerFd,
                         const std::atomic<bool>& stop,
                         SocketDispatcher dispatcher)
{
    std::vector<Connection> clients;

    while (!stop.load())
    {
        std::vector<struct pollfd> pfds;
        pfds.reserve(clients.size() + 1);
        pfds.push_back({listenerFd, POLLIN, 0});
        for (const Connection& c : clients)
            pfds.push_back({c.fd, POLLIN, 0});

        int rc;
        do {
            rc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 200);
        } while (rc < 0 && errno == EINTR);

        if (rc < 0)
            break; // listener closed or poll error -> exit

        if (rc == 0)
            continue; // timeout; re-check the stop flag for prompt shutdown

        // Listener readable -> accept one new client.
        if ((pfds[0].revents & POLLIN) != 0)
        {
            const int fd = ::accept(listenerFd, nullptr, nullptr);
            if (fd >= 0)
            {
                // Disable SIGPIPE on this connection so a client disconnecting
                // between command and response is a recoverable write error
                // rather than an unexpected process termination.
#ifdef SO_NOSIGPIPE
                const int on = 1;
                ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
                clients.push_back({fd, {}});
            }
            else if (errno == EBADF || errno == EINVAL || errno == ENOENT)
                break; // listener closed under us
        }

        // Service each client, accounting for indices after accept() may have
        // grown pfds (we rebuild the poll set each loop, so re-gather here from
        // listener-relative offsets is avoided by re-polling each iteration).
        // We iterate over the connection fds directly instead of pfds offsets.
        std::vector<Connection> remaining;
        remaining.reserve(clients.size());
        for (Connection& c : clients)
        {
            struct pollfd pfd { c.fd, POLLIN, 0 };
            do {
                rc = ::poll(&pfd, 1, 0);
            } while (rc < 0 && errno == EINTR);

            const short rev = (rc > 0) ? pfd.revents : 0;
            if ((rev & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
            {
                char chunk[8192];
                const ssize_t n = ::read(c.fd, chunk, sizeof(chunk));
                if (n > 0)
                {
                    c.buf.append(chunk, static_cast<std::size_t>(n));
                    processLines(c, stop, dispatcher);
                    remaining.push_back(std::move(c));
                }
                else
                {
                    // EOF or error -> dispatch any trailing partial line, then
                    // close and drop the client.
                    processLines(c, stop, dispatcher);
                    ::close(c.fd);
                }
            }
            else
            {
                remaining.push_back(std::move(c));
            }
        }
        clients = std::move(remaining);
    }

    // Best-effort clean close of every remaining connection on teardown.
    for (Connection& c : clients)
    {
        ::shutdown(c.fd, SHUT_RDWR);
        ::close(c.fd);
    }
}

} // namespace hathor::control