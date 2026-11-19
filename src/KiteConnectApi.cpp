#include "zerotick/KiteApi.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zerotick {
namespace {

using nlohmann::json;

const wchar_t* kKiteHost = L"api.kite.trade";

class WinHttpHandle {
public:
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}

    ~WinHttpHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    HINTERNET get() const {
        return handle_;
    }

private:
    HINTERNET handle_;
};

std::string trim(const std::string& value) {
    const std::string::size_type first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const std::string::size_type last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::map<std::string, std::string> loadCredentials() {
    std::map<std::string, std::string> values;
    std::ifstream input(".env");
    std::string line;

    // Read simple KEY=VALUE pairs from the ignored local .env file.
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::string::size_type separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string name = trim(line.substr(0, separator));
        if (!name.empty()) {
            values[name] = trim(line.substr(separator + 1));
        }
    }

    // Process environment values take precedence, allowing secret managers or
    // shell environment setup to be used instead of the local file.
    const char* names[] = {
        "KITE_API_KEY", "KITE_API_SECRET",
        "KITE_ACCESS_TOKEN"
    };
    for (const char* name : names) {
        const char* value = std::getenv(name);
        if (value != nullptr) {
            values[name] = value;
        }
    }
    return values;
}

std::string urlEncode(const std::string& value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (unsigned char character : value) {
        if ((character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded << character;
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned int>(character);
        }
    }
    return encoded.str();
}

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Unable to initialize Windows sockets");
        }
    }

    ~WinsockSession() {
        WSACleanup();
    }

    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
};

class SocketHandle {
public:
    explicit SocketHandle(SOCKET socket) : socket_(socket) {}

    ~SocketHandle() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SOCKET get() const {
        return socket_;
    }

private:
    SOCKET socket_;
};

std::string percentDecode(const std::string& value) {
    std::string decoded;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded += ' ';
        } else if (value[index] == '%') {
            if (index + 2 >= value.size()) {
                throw std::runtime_error("Malformed Kite callback query");
            }
            const auto hexValue = [](char character) -> int {
                if (character >= '0' && character <= '9') {
                    return character - '0';
                }
                if (character >= 'A' && character <= 'F') {
                    return character - 'A' + 10;
                }
                if (character >= 'a' && character <= 'f') {
                    return character - 'a' + 10;
                }
                return -1;
            };
            const int high = hexValue(value[index + 1]);
            const int low = hexValue(value[index + 2]);
            if (high < 0 || low < 0) {
                throw std::runtime_error("Malformed Kite callback encoding");
            }
            decoded += static_cast<char>((high << 4) | low);
            index += 2;
        } else {
            decoded += value[index];
        }
    }
    return decoded;
}

std::map<std::string, std::string> parseQuery(
    const std::string& query) {
    std::map<std::string, std::string> values;
    std::size_t start = 0;
    while (start <= query.size()) {
        const std::size_t end = query.find('&', start);
        const std::string pair = query.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = pair.find('=');
        const std::string key = percentDecode(pair.substr(0, separator));
        if (!key.empty()) {
            values[key] = separator == std::string::npos
                ? std::string()
                : percentDecode(pair.substr(separator + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

void sendCallbackResponse(
    SOCKET client,
    const char* status,
    const std::string& text) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: text/plain; charset=utf-8\r\n"
             << "Content-Length: " << text.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << text;
    const std::string bytes = response.str();
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int result = send(
            client,
            bytes.data() + sent,
            static_cast<int>(bytes.size() - sent),
            0);
        if (result == SOCKET_ERROR || result == 0) {
            return;
        }
        sent += static_cast<std::size_t>(result);
    }
}

std::string receiveHttpRequest(SOCKET client) {
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() >= 8192) {
            throw std::runtime_error("Kite callback request is too large");
        }
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received == SOCKET_ERROR || received == 0) {
            throw std::runtime_error("Unable to read Kite callback request");
        }
        request.append(buffer, static_cast<std::size_t>(received));
    }
    return request;
}

std::string requestTokenFromBrowser(const std::string& apiKey) {
    WinsockSession winsock;
    SocketHandle listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.get() == INVALID_SOCKET) {
        throw std::runtime_error("Unable to create Kite callback listener");
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(8765);
    if (bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == SOCKET_ERROR ||
        listen(listener.get(), 4) == SOCKET_ERROR) {
        throw std::runtime_error(
            "Unable to listen on 127.0.0.1:8765; check whether the port is in use");
    }

    const std::string loginUrl =
        "https://kite.zerodha.com/connect/login?v=3&api_key=" +
        urlEncode(apiKey);
    std::cout << "Starting Kite login in your browser...\n";
    const HINSTANCE browserResult = ShellExecuteA(
        nullptr, "open", loginUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(browserResult) <= 32) {
        std::cout << "Could not open a browser automatically. Open this URL:\n"
                  << loginUrl << '\n';
    }
    std::cout << "Waiting for the Kite login callback on localhost...\n";

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener.get(), &readable);
        timeval wait = {};
        wait.tv_sec = 1;
        const int ready = select(0, &readable, nullptr, nullptr, &wait);
        if (ready == SOCKET_ERROR) {
            throw std::runtime_error("Kite callback listener failed");
        }
        if (ready == 0) {
            continue;
        }

        sockaddr_in peer = {};
        int peerSize = sizeof(peer);
        SocketHandle client(accept(
            listener.get(),
            reinterpret_cast<sockaddr*>(&peer),
            &peerSize));
        if (client.get() == INVALID_SOCKET) {
            continue;
        }
        if ((ntohl(peer.sin_addr.s_addr) >> 24) != 127) {
            sendCallbackResponse(
                client.get(), "403 Forbidden", "Local requests only.");
            continue;
        }

        const std::string request = receiveHttpRequest(client.get());
        const std::size_t lineEnd = request.find("\r\n");
        const std::string requestLine = request.substr(0, lineEnd);
        const std::size_t firstSpace = requestLine.find(' ');
        const std::size_t secondSpace = requestLine.find(
            ' ', firstSpace == std::string::npos ? firstSpace : firstSpace + 1);
        if (firstSpace == std::string::npos ||
            secondSpace == std::string::npos ||
            requestLine.compare(0, 4, "GET ") != 0) {
            sendCallbackResponse(
                client.get(), "400 Bad Request", "Invalid callback request.");
            continue;
        }

        const std::string target = requestLine.substr(
            firstSpace + 1, secondSpace - firstSpace - 1);
        const std::size_t queryStart = target.find('?');
        if (target.substr(0, queryStart) != "/callback" ||
            queryStart == std::string::npos) {
            sendCallbackResponse(
                client.get(), "404 Not Found", "Not the Kite callback.");
            continue;
        }

        const std::map<std::string, std::string> query =
            parseQuery(target.substr(queryStart + 1));
        const auto status = query.find("status");
        if (status != query.end() && status->second == "error") {
            sendCallbackResponse(
                client.get(), "200 OK",
                "Kite login was not completed. You may close this page.");
            throw std::runtime_error("Kite login was not completed");
        }

        const auto token = query.find("request_token");
        if (status == query.end() || status->second != "success" ||
            token == query.end() || token->second.empty()) {
            sendCallbackResponse(
                client.get(), "400 Bad Request",
                "Missing Kite login result. You may close this page.");
            continue;
        }

        sendCallbackResponse(
            client.get(), "200 OK",
            "Kite login received. You may close this page.");
        std::cout << "Kite login callback received; exchanging token.\n";
        return token->second;
    }

    throw std::runtime_error("Timed out waiting for Kite login callback");
}

std::string sha256(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD digestLength = 0;
    DWORD bytesWritten = 0;

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error("Unable to initialize SHA-256 provider");
    }

    status = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
        &bytesWritten, 0);
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
            &bytesWritten, 0);
    }
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Unable to query SHA-256 properties");
    }

    std::vector<BYTE> object(objectLength);
    std::vector<BYTE> digest(digestLength);
    status = BCryptCreateHash(
        algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptHashData(
            hash,
            reinterpret_cast<PUCHAR>(
                const_cast<char*>(value.data())),
            static_cast<ULONG>(value.size()), 0);
    }
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptFinishHash(hash, digest.data(), digestLength, 0);
    }

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error("Unable to calculate Kite checksum");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (BYTE byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

std::wstring widenAscii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

std::string safeErrorField(const json& response, const char* field) {
    const auto value = response.find(field);
    if (value == response.end() || !value->is_string()) {
        return std::string();
    }

    // Restrict output to a short, single-line API description.
    const std::string raw = value->get<std::string>();
    std::string safe;
    for (unsigned char character : raw) {
        if (safe.size() >= 240) {
            break;
        }
        safe += character >= 32 && character != 127
            ? static_cast<char>(character)
            : ' ';
    }
    return safe;
}

std::string httpRequest(
    const char* operation,
    const wchar_t* method,
    const std::wstring& path,
    const std::string& body,
    const std::string& apiKey,
    const std::string& accessToken) {
    WinHttpHandle session(WinHttpOpen(
        L"ZeroTick-Systems/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session.get() == nullptr) {
        throw std::runtime_error("WinHTTP session initialization failed");
    }
    if (!WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 10000)) {
        throw std::runtime_error("Unable to set Kite HTTP timeouts");
    }

    WinHttpHandle connection(WinHttpConnect(
        session.get(), kKiteHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connection.get() == nullptr) {
        throw std::runtime_error("Unable to connect to Kite API host");
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connection.get(), method, path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request.get() == nullptr) {
        throw std::runtime_error("Unable to initialize Kite HTTP request");
    }

    std::wstring headers = L"X-Kite-Version: 3\r\n";
    if (!accessToken.empty()) {
        headers += widenAscii(
            "Authorization: token " + apiKey + ":" + accessToken);
        headers += L"\r\n";
    }
    if (std::wstring(method) == L"POST") {
        headers += L"Content-Type: application/x-www-form-urlencoded\r\n";
    }

    const LPVOID requestBody = body.empty()
        ? WINHTTP_NO_REQUEST_DATA
        : const_cast<char*>(body.data());
    const DWORD bodySize = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(
            request.get(),
            headers.c_str(),
            static_cast<DWORD>(headers.length()),
            requestBody,
            bodySize,
            bodySize,
            0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error("Kite HTTP request failed");
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        throw std::runtime_error("Unable to read Kite HTTP status");
    }

    std::string response;
    DWORD available = 0;
    while (true) {
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            throw std::runtime_error(
                "Unable to query Kite HTTP response size");
        }
        if (available == 0) {
            break;
        }
        std::vector<char> buffer(available);
        DWORD received = 0;
        if (!WinHttpReadData(
                request.get(), buffer.data(), available, &received)) {
            throw std::runtime_error("Unable to read Kite HTTP response");
        }
        response.append(buffer.data(), received);
    }

    // Avoid echoing response bodies that may contain account/session details.
    if (statusCode < 200 || statusCode >= 300) {
        std::ostringstream message;
        message << operation << " returned HTTP status " << statusCode;
        const json error = json::parse(response, nullptr, false);
        if (!error.is_discarded() && error.is_object()) {
            const std::string errorType = safeErrorField(error, "error_type");
            const std::string description = safeErrorField(error, "message");
            if (!errorType.empty()) {
                message << " (" << errorType << ')';
            }
            if (!description.empty()) {
                message << ": " << description;
            }
        }
        throw std::runtime_error(message.str());
    }
    return response;
}

void persistAccessToken(const std::string& accessToken) {
    // The request token is one-time-use; replace it with the session access
    // token so later program starts do not attempt to exchange it again.
    std::ifstream input(".env");
    if (!input) {
        throw std::runtime_error(
            "A readable .env file is required to persist the Kite access token");
    }
    std::vector<std::string> lines;
    std::string line;
    bool foundAccessToken = false;

    while (std::getline(input, line)) {
        const std::string cleanLine = trim(line);
        if (cleanLine.compare(0, 18, "KITE_ACCESS_TOKEN=") == 0) {
            line = "KITE_ACCESS_TOKEN=" + accessToken;
            foundAccessToken = true;
        } else if (cleanLine.compare(0, 19, "KITE_REQUEST_TOKEN=") == 0) {
            line = "KITE_REQUEST_TOKEN=";
        }
        lines.push_back(line);
    }

    if (!foundAccessToken) {
        lines.push_back("KITE_ACCESS_TOKEN=" + accessToken);
    }
    std::ofstream output(".env", std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Unable to persist Kite access token to .env");
    }
    for (const std::string& savedLine : lines) {
        output << savedLine << '\n';
    }
    if (!output) {
        throw std::runtime_error("Failed while writing Kite access token");
    }
}

MarketSnapshot parseQuote(
    const json& quote,
    const std::string& symbol) {
    MarketSnapshot snapshot;
    snapshot.symbol = symbol;
    snapshot.lastPrice = quote.at("last_price").get<double>();
    snapshot.receivedAt = std::chrono::steady_clock::now();

    const json& depth = quote.at("depth");
    const json& buys = depth.at("buy");
    const json& sells = depth.at("sell");
    if (!buys.empty()) {
        snapshot.bidPrice = buys.at(0).at("price").get<double>();
    }
    if (!sells.empty()) {
        snapshot.askPrice = sells.at(0).at("price").get<double>();
    }
    // Retain every returned level so the strategy can weight near-touch
    // liquidity more strongly than depth farther from the best prices.
    for (std::size_t index = 0;
         index < buys.size() && index < snapshot.bidLevels.size();
         ++index) {
        snapshot.bidLevels[index].price =
            buys.at(index).at("price").get<double>();
        snapshot.bidLevels[index].quantity =
            buys.at(index).at("quantity").get<std::uint64_t>();
        snapshot.bidQuantity += snapshot.bidLevels[index].quantity;
    }
    for (std::size_t index = 0;
         index < sells.size() && index < snapshot.askLevels.size();
         ++index) {
        snapshot.askLevels[index].price =
            sells.at(index).at("price").get<double>();
        snapshot.askLevels[index].quantity =
            sells.at(index).at("quantity").get<std::uint64_t>();
        snapshot.askQuantity += snapshot.askLevels[index].quantity;
    }
    return snapshot;
}

} // namespace

KiteConnectApi::KiteConnectApi(bool forceLogin) {
    const std::map<std::string, std::string> values = loadCredentials();
    const auto apiKey = values.find("KITE_API_KEY");
    const auto apiSecret = values.find("KITE_API_SECRET");
    const auto accessToken = values.find("KITE_ACCESS_TOKEN");

    if (apiKey == values.end() || apiKey->second.empty()) {
        throw std::runtime_error("KITE_API_KEY is missing from .env");
    }
    apiKey_ = apiKey->second;

    if (!forceLogin &&
        accessToken != values.end() && !accessToken->second.empty()) {
        accessToken_ = accessToken->second;
    } else {
        if (apiSecret == values.end() || apiSecret->second.empty()) {
            throw std::runtime_error(
                "KITE_API_SECRET is missing from .env");
        }

        // Request tokens are short-lived and one-time-use, so obtain one from
        // the browser callback rather than persisting or prompting to paste it.
        const std::string tokenToExchange =
            requestTokenFromBrowser(apiKey_);
        const std::string checksum = sha256(
            apiKey_ + tokenToExchange + apiSecret->second);
        const std::string body =
            "api_key=" + urlEncode(apiKey_) +
            "&request_token=" + urlEncode(tokenToExchange) +
            "&checksum=" + checksum;
        const std::string response = httpRequest(
            "Kite session-token exchange",
            L"POST", L"/session/token", body, apiKey_, std::string());

        const json result = json::parse(response);
        if (result.value("status", std::string()) != "success") {
            throw std::runtime_error("Kite session-token exchange failed");
        }
        accessToken_ = result.at("data").at("access_token").get<std::string>();
        if (accessToken_.empty()) {
            throw std::runtime_error("Kite returned an empty access token");
        }
        persistAccessToken(accessToken_);
        std::cout << "Kite access token obtained and saved in local .env\n";
    }

    healthy_ = true;
}

MarketSnapshot KiteConnectApi::getQuote(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(quoteMutex_);
    const auto found = quotes_.find(symbol);
    if (found == quotes_.end()) {
        throw std::runtime_error(
            "No Kite quote cached yet for " + symbol);
    }
    if (std::chrono::steady_clock::now() - found->second.receivedAt >
        std::chrono::seconds(5)) {
        throw std::runtime_error("Kite quote is stale for " + symbol);
    }
    return found->second;
}

void KiteConnectApi::refreshQuotes(
    const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        throw std::invalid_argument("Kite quote refresh needs symbols");
    }

    // Kite's quote endpoint accepts multiple instrument query parameters.
    // Batch the whole watchlist to avoid one REST call per worker per second.
    std::wstring path = L"/quote?";
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        if (index != 0) {
            path += L"&";
        }
        const std::string parameter =
            "i=" + urlEncode("NSE:" + symbols[index]);
        path += widenAscii(parameter);
    }

    const std::string response = httpRequest(
        "Kite quote fetch",
        L"GET", path, std::string(), apiKey_, accessToken_);
    const json result = json::parse(response);
    if (result.value("status", std::string()) != "success") {
        throw std::runtime_error("Kite quote request did not succeed");
    }

    std::map<std::string, MarketSnapshot> updated;
    const json& data = result.at("data");
    for (const std::string& symbol : symbols) {
        updated[symbol] = parseQuote(data.at("NSE:" + symbol), symbol);
    }

    // Replace the cache in one operation so readers never mix two batches.
    std::lock_guard<std::mutex> lock(quoteMutex_);
    quotes_.swap(updated);
}

OrderResult KiteConnectApi::placeMarketOrder(
    const std::string& symbol,
    bool buy,
    int quantity) {
    // Keep live trading disabled until order-state reconciliation, risk
    // validation, and explicit live-trading authorization are implemented.
    (void)symbol;
    (void)buy;
    (void)quantity;
    return {false, {}, "Live Kite orders are disabled"};
}

OrderResult KiteConnectApi::exitPosition(
    const std::string& symbol,
    int quantity) {
    (void)symbol;
    (void)quantity;
    return {false, {}, "Live Kite orders are disabled"};
}

bool KiteConnectApi::isHealthy() const {
    return healthy_;
}

} // namespace zerotick
