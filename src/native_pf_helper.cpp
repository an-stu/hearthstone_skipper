#include <arpa/inet.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char *kPfctl = "/sbin/pfctl";
constexpr const char *kAnchor = "com.apple/hearthstone_skipper";

struct CommandResult {
    int exitCode = -1;
    std::string output;
};

struct DisconnectRequest {
    std::string sourceAddress;
    int sourcePort = 0;
    std::string destinationAddress;
    int destinationPort = 0;
    int durationMs = 0;
    int family = AF_UNSPEC;
};

CommandResult runPfctl(const std::vector<std::string> &arguments, const std::string &input = {}) {
    int inputPipe[2]{};
    int outputPipe[2]{};
    if (pipe(inputPipe) != 0 || pipe(outputPipe) != 0) {
        return {.exitCode = -1, .output = std::string("pipe failed: ") + std::strerror(errno)};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(inputPipe[0]);
        close(inputPipe[1]);
        close(outputPipe[0]);
        close(outputPipe[1]);
        return {.exitCode = -1, .output = std::string("fork failed: ") + std::strerror(errno)};
    }
    if (pid == 0) {
        dup2(inputPipe[0], STDIN_FILENO);
        dup2(outputPipe[1], STDOUT_FILENO);
        dup2(outputPipe[1], STDERR_FILENO);
        close(inputPipe[0]);
        close(inputPipe[1]);
        close(outputPipe[0]);
        close(outputPipe[1]);

        std::vector<char *> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char *>(kPfctl));
        for (const std::string &argument : arguments) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execv(kPfctl, argv.data());
        _exit(127);
    }

    close(inputPipe[0]);
    close(outputPipe[1]);
    size_t written = 0;
    while (written < input.size()) {
        const ssize_t count = write(inputPipe[1], input.data() + written, input.size() - written);
        if (count <= 0) {
            break;
        }
        written += static_cast<size_t>(count);
    }
    close(inputPipe[1]);

    CommandResult result;
    std::array<char, 4096> buffer{};
    ssize_t count = 0;
    while ((count = read(outputPipe[0], buffer.data(), buffer.size())) > 0) {
        result.output.append(buffer.data(), static_cast<size_t>(count));
    }
    close(outputPipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    }
    return result;
}

bool validAddress(const std::string &address, int *family) {
    std::array<unsigned char, sizeof(in6_addr)> binary{};
    if (inet_pton(AF_INET, address.c_str(), binary.data()) == 1) {
        *family = AF_INET;
        return true;
    }
    if (inet_pton(AF_INET6, address.c_str(), binary.data()) == 1) {
        *family = AF_INET6;
        return true;
    }
    return false;
}

bool parseNumber(const char *text, int minimum, int maximum, int *result) {
    try {
        size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        if (consumed != std::strlen(text) || value < minimum || value > maximum) {
            return false;
        }
        *result = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool validRequest(const DisconnectRequest &request) {
    int sourceFamily = AF_UNSPEC;
    int destinationFamily = AF_UNSPEC;
    return validAddress(request.sourceAddress, &sourceFamily) &&
           validAddress(request.destinationAddress, &destinationFamily) && sourceFamily == destinationFamily &&
           request.sourcePort >= 1 && request.sourcePort <= 65535 &&
           (request.destinationPort == 1119 || request.destinationPort == 3724) && request.durationMs >= 500 &&
           request.durationMs <= 3000;
}

std::string singleLine(std::string text) {
    for (char &character : text) {
        if (character == '\n' || character == '\r') {
            character = ' ';
        }
    }
    return text;
}

void clearAnchor() {
    runPfctl({"-a", kAnchor, "-F", "rules"});
}

bool blockedPacketSeen() {
    const CommandResult labels = runPfctl({"-a", kAnchor, "-s", "labels"});
    if (labels.exitCode != 0) {
        return false;
    }
    std::istringstream lines(labels.output);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string label;
        unsigned long long evaluations = 0;
        unsigned long long packets = 0;
        if (fields >> label >> evaluations >> packets && label == "hearthstone_skipper" && packets > 0) {
            return true;
        }
    }
    return false;
}

CommandResult disconnect(const DisconnectRequest &request) {
    const CommandResult enable = runPfctl({"-E"});
    if (enable.exitCode != 0) {
        return {.exitCode = 70, .output = "pf enable failed: " + enable.output};
    }
    std::smatch tokenMatch;
    const std::regex tokenPattern(R"(Token\s*:\s*(\d+))", std::regex::icase);
    if (!std::regex_search(enable.output, tokenMatch, tokenPattern)) {
        return {.exitCode = 70, .output = "pf enable token missing: " + enable.output};
    }
    const std::string token = tokenMatch[1].str();
    const auto cleanup = [&token] {
        clearAnchor();
        runPfctl({"-X", token});
    };

    const std::string familyName = request.family == AF_INET ? "inet" : "inet6";
    // Match the exact established four-tuple. Hearthstone's reconnect uses a
    // new ephemeral source port, so its first ACK can never hit this rule even
    // if it races with the final counter poll and anchor cleanup.
    const std::string rule = "block return-rst out quick " + familyName + " proto tcp from " +
                             request.sourceAddress + " port = " + std::to_string(request.sourcePort) + " to " +
                             request.destinationAddress + " port = " + std::to_string(request.destinationPort) +
                             " flags A/A no state label hearthstone_skipper\n";
    const CommandResult load = runPfctl({"-a", kAnchor, "-f", "-"}, rule);
    if (load.exitCode != 0) {
        cleanup();
        return {.exitCode = 70, .output = "pf rule load failed: " + load.output};
    }

    const CommandResult kill =
        runPfctl({"-k", request.sourceAddress, "-k", request.destinationAddress});
    if (kill.exitCode != 0) {
        cleanup();
        return {.exitCode = 70, .output = "pf state removal failed: " + kill.output};
    }

    int elapsedMs = 0;
    constexpr int pollIntervalMs = 10;
    bool triggered = blockedPacketSeen();
    while (elapsedMs < request.durationMs && !triggered) {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
        elapsedMs += pollIntervalMs;
        triggered = blockedPacketSeen();
    }
    cleanup();
    if (!triggered) {
        return {.exitCode = 69,
                .output = "native disconnect timed out before the established connection emitted a packet"};
    }
    return {.exitCode = 0,
            .output = "native disconnect completed source=" + request.sourceAddress + ':' +
                      std::to_string(request.sourcePort) + " destination=" + request.destinationAddress + ':' +
                      std::to_string(request.destinationPort) + " observed_after_ms=" +
                      std::to_string(elapsedMs)};
}

bool parseServiceRequest(const std::string &line, DisconnectRequest *request) {
    std::istringstream fields(line);
    std::string command;
    std::string extra;
    if (!(fields >> command >> request->sourceAddress >> request->sourcePort >> request->destinationAddress >>
          request->destinationPort >> request->durationMs) ||
        command != "DISCONNECT" || (fields >> extra)) {
        return false;
    }
    return validRequest(*request) && validAddress(request->sourceAddress, &request->family);
}

int serve() {
    std::cout << "READY pid=" << getpid() << '\n' << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "QUIT") {
            clearAnchor();
            std::cout << "BYE\n" << std::flush;
            return 0;
        }
        DisconnectRequest request;
        if (!parseServiceRequest(line, &request)) {
            std::cout << "ERR code=65 request rejected\n" << std::flush;
            continue;
        }
        const CommandResult result = disconnect(request);
        std::cout << (result.exitCode == 0 ? "OK " : "ERR code=" + std::to_string(result.exitCode) + " ")
                  << singleLine(result.output) << '\n'
                  << std::flush;
    }
    clearAnchor();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "serve") {
        if (geteuid() != 0) {
            std::cerr << "administrator privileges are required\n";
            return 77;
        }
        return serve();
    }
    if (argc != 7 || std::string(argv[1]) != "disconnect") {
        std::cerr << "usage: skipper-native-helper disconnect <source-ip> <source-port> <destination-ip> "
                     "<destination-port> <duration-ms>\n";
        return 64;
    }
    DisconnectRequest request{.sourceAddress = argv[2], .destinationAddress = argv[4]};
    if (!parseNumber(argv[3], 1, 65535, &request.sourcePort) ||
        !parseNumber(argv[5], 1, 65535, &request.destinationPort) ||
        !parseNumber(argv[6], 500, 3000, &request.durationMs) || !validRequest(request) ||
        !validAddress(request.sourceAddress, &request.family)) {
        std::cerr << "request rejected: invalid four-tuple, Blizzard port, or duration\n";
        return 65;
    }
    if (geteuid() != 0) {
        std::cerr << "administrator privileges are required\n";
        return 77;
    }
    const CommandResult result = disconnect(request);
    (result.exitCode == 0 ? std::cout : std::cerr) << result.output << '\n';
    return result.exitCode;
}
