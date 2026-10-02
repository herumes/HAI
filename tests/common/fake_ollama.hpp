#pragma once
// A stand-in for Ollama's /api/chat on localhost, so tests can run the real AI code without a model.
// Include after pch.h. Scripted replies are answered in order; "STATUS:500" answers with an error.
#include "common.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <deque>

// ------------------------------------------------------------------------------------------------
// The fake Ollama
// ------------------------------------------------------------------------------------------------
struct Fake {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::thread th;
    std::atomic<bool> stop{ false };
    std::mutex m;
    std::deque<std::string> script;          // replies in order; "STATUS:500" answers with an error
    std::vector<std::string> requests;
    std::string fallback;
    std::atomic<int> delayMs{ 0 };

    void Start() {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(ls, (sockaddr*)&a, sizeof(a));
        int len = sizeof(a);
        getsockname(ls, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        listen(ls, 8);
        th = std::thread([this] { Loop(); });
    }
    void Stop() {
        stop = true;
        if (th.joinable()) th.join();
        closesocket(ls);
    }
    void Reset() {
        std::lock_guard lk(m);
        script.clear();
        requests.clear();
        fallback = Reply("ok");
        delayMs = 0;
    }
    size_t RequestCount() { std::lock_guard lk(m); return requests.size(); }
    std::string Request(size_t i) { std::lock_guard lk(m); return i < requests.size() ? requests[i] : std::string(); }

    static std::string Reply(const std::string& text) {
        return "{\"model\":\"fake\",\"message\":{\"role\":\"assistant\",\"content\":\"" + JsonEscape(text) +
               "\"},\"done\":true,\"prompt_eval_count\":42,\"eval_count\":7}";
    }
    static std::string Tool(const std::string& name, const std::string& argsObject) {
        return "{\"model\":\"fake\",\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"function\":{\"name\":\"" +
               name + "\",\"arguments\":" + argsObject + "}}]},\"done\":true}";
    }

    void Loop() {
        while (!stop) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            timeval tv{ 0, 100000 };
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::string req;
            char buf[8192];
            size_t need = 0, headerEnd = std::string::npos;
            for (;;) {
                const int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, (size_t)n);
                if (headerEnd == std::string::npos) {
                    headerEnd = req.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        const size_t cl = req.find("Content-Length:");
                        need = cl == std::string::npos ? 0 : (size_t)atoll(req.c_str() + cl + 15);
                    }
                }
                if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + need) break;
            }
            std::string body = headerEnd == std::string::npos ? "" : req.substr(headerEnd + 4);
            std::string answer;
            {
                std::lock_guard lk(m);
                requests.push_back(body);
                if (!script.empty()) { answer = script.front(); script.pop_front(); }
                else answer = fallback;
            }
            if (delayMs > 0) Sleep((DWORD)delayMs.load());
            std::string status = "200 OK";
            if (answer.rfind("STATUS:", 0) == 0) { status = answer.substr(7) + " Error"; answer = "{\"error\":\"boom\"}"; }
            const std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                     std::to_string(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer;
            send(c, resp.data(), (int)resp.size(), 0);
            shutdown(c, SD_SEND);
            closesocket(c);
        }
    }
};

