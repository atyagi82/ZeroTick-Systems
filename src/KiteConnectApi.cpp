#include "zerotick/KiteApi.h"
#include "zerotick/Log.h"
#include "zerotick/Risk.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <cmath>
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

std::vector<std::string> parseCsvRecord(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '"') {
            if (quoted && index + 1 < line.size() &&
                line[index + 1] == '"') {
                field += '"';
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else {
            field += character;
        }
    }
    if (quoted) {
        throw std::runtime_error("Malformed Kite instrument CSV record");
    }
    fields.push_back(field);
    return fields;
}

bool isValidIsoDate(const std::string& date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < date.size(); ++index) {
        if (index != 4 && index != 7 &&
            (date[index] < '0' || date[index] > '9')) {
            return false;
        }
    }

    const int year = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    const int day = std::stoi(date.substr(8, 2));
    if (year < 2000 || month < 1 || month > 12) {
        return false;
    }
    static const int monthDays[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int maximumDay = monthDays[month - 1];
    const bool leapYear =
        year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leapYear) {
        maximumDay = 29;
    }
    return day >= 1 && day <= maximumDay;
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
    zerotick::logLine("[kite_auth] browser_login_started");
    const HINSTANCE browserResult = ShellExecuteA(
        nullptr, "open", loginUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(browserResult) <= 32) {
        zerotick::logLine(
            "[kite_auth] browser_launch_failed; login_url_printed_to_console");
        std::cout << "Could not open a browser automatically. Open this URL:\n"
                  << loginUrl << '\n';
    }
    zerotick::logLine("[kite_auth] waiting_for_local_callback");

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
            zerotick::logLine("[kite_auth] callback_returned_error");
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
        zerotick::logLine("[kite_auth] callback_received");
        return token->second;
    }

    zerotick::logLine("[kite_auth] callback_timeout");
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

std::string orderPrice(double price) {
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(2) << price;
    return formatted.str();
}

std::string httpRequest(
    const char* operation,
    const wchar_t* method,
    const std::wstring& path,
    const std::string& body,
    const std::string& apiKey,
    const std::string& accessToken,
    bool jsonBody = false) {
    const auto requestStartedAt = std::chrono::steady_clock::now();
    const std::wstring wideMethod(method);
    std::string requestMethod;
    requestMethod.reserve(wideMethod.size());
    for (wchar_t character : wideMethod) {
        requestMethod.push_back(static_cast<char>(character));
    }
    zerotick::logLine(
        "[kite_api] request_start operation=" + std::string(operation) +
        " method=" + requestMethod);
    DWORD statusCode = 0;
    std::size_t responseBytes = 0;
    try {
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

    // Kite's instrument dump is gzip-compressed; enable transparent decoding
    // for that response while keeping the same request helper for JSON APIs.
    DWORD decompressionFlags = WINHTTP_DECOMPRESSION_FLAG_GZIP;
    if (!WinHttpSetOption(
            request.get(), WINHTTP_OPTION_DECOMPRESSION,
            &decompressionFlags, sizeof(decompressionFlags))) {
        throw std::runtime_error(
            "Unable to enable Kite HTTP gzip decompression");
    }

    std::wstring headers = L"X-Kite-Version: 3\r\n";
    if (!accessToken.empty()) {
        headers += widenAscii(
            "Authorization: token " + apiKey + ":" + accessToken);
        headers += L"\r\n";
    }
    if (std::wstring(method) == L"POST") {
        headers += jsonBody
            ? L"Content-Type: application/json\r\n"
            : L"Content-Type: application/x-www-form-urlencoded\r\n";
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
    responseBytes = response.size();

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
        throw KiteHttpError(statusCode, message.str());
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - requestStartedAt).count();
    zerotick::logLine(
        "[kite_api] request_complete operation=" + std::string(operation) +
        " method=" + requestMethod +
        " status=" + std::to_string(statusCode) +
        " response_bytes=" + std::to_string(responseBytes) +
        " duration_ms=" + std::to_string(duration));
    return response;
    } catch (const KiteHttpError& exception) {
        const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - requestStartedAt).count();
        zerotick::logLine(
            "[kite_api] request_failed operation=" + std::string(operation) +
            " method=" + requestMethod +
            " status=" + std::to_string(exception.statusCode()) +
            " response_bytes=" + std::to_string(responseBytes) +
            " duration_ms=" + std::to_string(duration));
        throw;
    } catch (const std::exception&) {
        const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - requestStartedAt).count();
        zerotick::logLine(
            "[kite_api] request_failed operation=" + std::string(operation) +
            " method=" + requestMethod +
            " status=" + std::to_string(statusCode) +
            " response_bytes=" + std::to_string(responseBytes) +
            " duration_ms=" + std::to_string(duration));
        throw;
    }
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

    // Intraday suitability fields used by StockSelectionEngine: traded volume
    // (for relative-volume / liquidity checks) and the OHLC range (for
    // volatility / intraday-range checks). Kite returns these when present;
    // they default to zero otherwise.
    snapshot.volume = quote.value("volume", std::uint64_t(0));
    const json& ohlc = quote.value("ohlc", json::object());
    snapshot.openPrice = ohlc.value("open", 0.0);
    snapshot.highPrice = ohlc.value("high", 0.0);
    snapshot.lowPrice = ohlc.value("low", 0.0);

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
        zerotick::logLine("[kite_auth] access_token_saved_to_local_env");
    }

    healthy_ = true;
}

void KiteConnectApi::ensureInstrumentListLoaded() const {
    std::lock_guard<std::mutex> lock(instrumentMutex_);
    if (instrumentListLoaded_) {
        return;
    }

    const std::string response = httpRequest(
        "Kite NSE instrument list",
        L"GET", L"/instruments/NSE", std::string(), apiKey_, accessToken_);
    std::istringstream input(response);
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("Kite returned an empty NSE instrument list");
    }
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }

    const std::vector<std::string> headers = parseCsvRecord(line);
    const auto findColumn = [&headers](const std::string& name) {
        for (std::size_t index = 0; index < headers.size(); ++index) {
            if (trim(headers[index]) == name) {
                return index;
            }
        }
        throw std::runtime_error(
            "Kite instrument list is missing column " + name);
    };
    const std::size_t tokenColumn = findColumn("instrument_token");
    const std::size_t symbolColumn = findColumn("tradingsymbol");
    const std::size_t tickColumn = findColumn("tick_size");
    const std::size_t typeColumn = findColumn("instrument_type");
    const std::size_t exchangeColumn = findColumn("exchange");
    std::size_t requiredColumn = tokenColumn;
    if (symbolColumn > requiredColumn) {
        requiredColumn = symbolColumn;
    }
    if (tickColumn > requiredColumn) {
        requiredColumn = tickColumn;
    }
    if (typeColumn > requiredColumn) {
        requiredColumn = typeColumn;
    }
    if (exchangeColumn > requiredColumn) {
        requiredColumn = exchangeColumn;
    }

    std::map<std::string, InstrumentInfo> loaded;
    std::size_t invalidMetadataRows = 0;
    while (std::getline(input, line)) {
        if (trim(line).empty()) {
            continue;
        }
        const std::vector<std::string> fields = parseCsvRecord(line);
        if (fields.size() <= requiredColumn) {
            throw std::runtime_error(
                "Kite instrument list contains an incomplete row");
        }
        if (trim(fields[exchangeColumn]) != "NSE" ||
            trim(fields[typeColumn]) != "EQ") {
            continue;
        }

        const std::string symbol = trim(fields[symbolColumn]);
        const std::string token = trim(fields[tokenColumn]);
        const std::string tickText = trim(fields[tickColumn]);
        if (symbol.empty() || token.empty()) {
            ++invalidMetadataRows;
            continue;
        }

        std::size_t parsedLength = 0;
        double tickSize = 0.0;
        try {
            tickSize = std::stod(tickText, &parsedLength);
        } catch (const std::invalid_argument&) {
            ++invalidMetadataRows;
            continue;
        } catch (const std::out_of_range&) {
            ++invalidMetadataRows;
            continue;
        }
        if (parsedLength != tickText.size() ||
            !std::isfinite(tickSize) || tickSize <= 0.0) {
            ++invalidMetadataRows;
            continue;
        }
        InstrumentInfo instrument;
        instrument.token = token;
        instrument.tickSize = tickSize;
        loaded[symbol] = instrument;
    }
    if (invalidMetadataRows > 0) {
        logLine("[kite] instrument_metadata_rows_skipped=" +
                std::to_string(invalidMetadataRows));
    }
    if (loaded.empty()) {
        throw std::runtime_error(
            "Kite NSE instrument list contains no equity instruments");
    }

    nseInstruments_.swap(loaded);
    instrumentListLoaded_ = true;
    logLine("[kite_api] instruments_loaded count=" +
            std::to_string(nseInstruments_.size()) +
            " invalid_rows_skipped=" +
            std::to_string(invalidMetadataRows));
}

double KiteConnectApi::getInstrumentTickSize(
    const std::string& symbol) const {
    ensureInstrumentListLoaded();
    std::lock_guard<std::mutex> lock(instrumentMutex_);
    const auto found = nseInstruments_.find(symbol);
    if (found == nseInstruments_.end()) {
        throw std::runtime_error(
            "Kite instrument list has no NSE equity for " + symbol);
    }
    return found->second.tickSize;
}

std::vector<HistoricalCandle> KiteConnectApi::getHistoricalCandles(
    const std::string& symbol,
    const std::string& date) const {
    return getHistoricalCandles(symbol, date, date);
}

std::vector<HistoricalCandle> KiteConnectApi::getHistoricalCandles(
    const std::string& symbol,
    const std::string& fromDate,
    const std::string& toDate) const {
    if (!isValidIsoDate(fromDate) || !isValidIsoDate(toDate) ||
        fromDate > toDate) {
        throw std::invalid_argument(
            "Backtest range must contain valid ascending YYYY-MM-DD dates");
    }
    ensureInstrumentListLoaded();

    std::string token;
    {
        std::lock_guard<std::mutex> lock(instrumentMutex_);
        const auto found = nseInstruments_.find(symbol);
        if (found == nseInstruments_.end()) {
            throw std::runtime_error(
                "Kite instrument list has no NSE equity for " + symbol);
        }
        token = found->second.token;
    }

    const std::string from = fromDate + " 09:15:00";
    const std::string to = toDate + " 15:30:00";
    logLine("[kite_api] historical_request symbol=" + symbol +
            " from=" + fromDate + " to=" + toDate);
    const std::string requestPath =
        "/instruments/historical/" + token + "/minute?from=" +
        urlEncode(from) + "&to=" + urlEncode(to) + "&oi=0";
    const std::string response = httpRequest(
        "Kite historical candle fetch",
        L"GET", widenAscii(requestPath), std::string(), apiKey_, accessToken_);
    const json result = json::parse(response);
    if (result.value("status", std::string()) != "success") {
        throw std::runtime_error(
            "Kite historical candle request did not succeed for " + symbol);
    }

    const json& records = result.at("data").at("candles");
    if (!records.is_array()) {
        throw std::runtime_error(
            "Kite returned an invalid candle list for " + symbol);
    }

    std::vector<HistoricalCandle> candles;
    candles.reserve(records.size());
    std::string previousTimestamp;
    for (const json& record : records) {
        if (!record.is_array() || record.size() < 6) {
            throw std::runtime_error(
                "Kite returned a malformed candle for " + symbol);
        }
        HistoricalCandle candle;
        candle.timestamp = record.at(0).get<std::string>();
        if (candle.timestamp.size() < 10) {
            throw std::runtime_error(
                "Kite returned a malformed candle timestamp for " + symbol);
        }
        const std::string candleDate = candle.timestamp.substr(0, 10);
        candle.open = record.at(1).get<double>();
        candle.high = record.at(2).get<double>();
        candle.low = record.at(3).get<double>();
        candle.close = record.at(4).get<double>();
        candle.volume = record.at(5).get<std::uint64_t>();
        if (candleDate < fromDate || candleDate > toDate ||
            !std::isfinite(candle.open) || !std::isfinite(candle.high) ||
            !std::isfinite(candle.low) || !std::isfinite(candle.close) ||
            candle.open <= 0.0 || candle.close <= 0.0 ||
            candle.low <= 0.0 || candle.high < candle.low ||
            candle.high < candle.open || candle.high < candle.close ||
            candle.low > candle.open || candle.low > candle.close ||
            (!previousTimestamp.empty() &&
             candle.timestamp <= previousTimestamp)) {
            throw std::runtime_error(
                "Kite returned invalid or unordered candles for " + symbol);
        }
        previousTimestamp = candle.timestamp;
        candles.push_back(std::move(candle));
    }
    logLine("[kite_api] historical_result symbol=" + symbol +
            " from=" + fromDate + " to=" + toDate +
            " candles=" + std::to_string(candles.size()));
    return candles;
}

MarketSnapshot KiteConnectApi::getQuote(
    const std::string& symbol, std::chrono::seconds maxAge) {
    std::lock_guard<std::mutex> lock(quoteMutex_);
    const auto found = quotes_.find(symbol);
    if (found == quotes_.end()) {
        zerotick::logLine("[kite_api] quote_cache_miss symbol=" + symbol);
        throw std::runtime_error(
            "No Kite quote cached yet for " + symbol);
    }
    if (std::chrono::steady_clock::now() - found->second.receivedAt > maxAge) {
        zerotick::logLine("[kite_api] quote_cache_stale symbol=" + symbol);
        throw std::runtime_error("Kite quote is stale for " + symbol);
    }
    zerotick::logLine("[kite_api] quote_cache_hit symbol=" + symbol);
    return found->second;
}

std::vector<MarketSnapshot> KiteConnectApi::refreshQuotes(
    const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        throw std::invalid_argument("Kite quote refresh needs symbols");
    }

    // Kite's quote endpoint accepts multiple instrument query parameters.
    // Batch the whole watchlist to avoid one REST call per worker.
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
    std::vector<MarketSnapshot> refreshed;
    refreshed.reserve(symbols.size());
    std::size_t missing = 0;
    {
        // Seed with the current cache so chunked refreshes accumulate rather
        // than replace previously cached instruments.
        std::lock_guard<std::mutex> lock(quoteMutex_);
        updated = quotes_;
    }
    const json& data = result.at("data");
    for (const std::string& symbol : symbols) {
        const std::string key = "NSE:" + symbol;
        const auto it = data.find(key);
        if (it == data.end() || !it->is_object()) {
            // Kite did not return a quote for this instrument (e.g. not
            // tradable / delisted / unknown token). Skip rather than crash so
            // the rest of the universe can still be evaluated.
            bool firstMissing = false;
            {
                std::lock_guard<std::mutex> lock(quoteMutex_);
                firstMissing = missingQuoteSymbols_.insert(symbol).second;
            }
            if (firstMissing) {
                zerotick::logLine("[kite] quote_missing symbol=" + symbol);
            }
            ++missing;
            continue;
        }
        MarketSnapshot snapshot = parseQuote(*it, symbol);
        bool recovered = false;
        {
            std::lock_guard<std::mutex> lock(quoteMutex_);
            recovered = missingQuoteSymbols_.erase(symbol) > 0;
        }
        if (recovered) {
            zerotick::logLine("[kite] quote_restored symbol=" + symbol);
        }
        updated[symbol] = snapshot;
        refreshed.push_back(std::move(snapshot));
    }

    // Merge into the cache in one operation so readers never mix two batches.
    std::lock_guard<std::mutex> lock(quoteMutex_);
    quotes_.swap(updated);
    zerotick::logLine(
        "[kite_api] quote_refresh_complete requested=" +
        std::to_string(symbols.size()) +
        " returned=" + std::to_string(refreshed.size()) +
        " missing=" + std::to_string(missing));
    return refreshed;
}

void KiteConnectApi::ensureNoOpenMisExposure(
    const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        throw std::invalid_argument(
            "MIS exposure preflight needs at least one symbol");
    }

    logLine("[kite_api] live_preflight_started symbols=" +
            std::to_string(symbols.size()));
    const std::string positionsResponse = httpRequest(
        "Kite positions preflight",
        L"GET", L"/portfolio/positions", std::string(),
        apiKey_, accessToken_);
    const json positions = json::parse(positionsResponse);
    if (positions.value("status", std::string()) != "success") {
        throw std::runtime_error("Kite positions preflight did not succeed");
    }

    const json& netPositions = positions.at("data").at("net");
    for (const json& position : netPositions) {
        const std::string symbol =
            position.value("tradingsymbol", std::string());
        if (std::find(symbols.begin(), symbols.end(), symbol) !=
                symbols.end() &&
            position.value("product", std::string()) == "MIS" &&
            position.value("quantity", 0) != 0) {
            throw std::runtime_error(
                "Live trading blocked: an MIS position already exists for " +
                symbol);
        }
    }

    const std::string ordersResponse = httpRequest(
        "Kite pending-order preflight",
        L"GET", L"/orders", std::string(), apiKey_, accessToken_);
    const json orders = json::parse(ordersResponse);
    if (orders.value("status", std::string()) != "success") {
        throw std::runtime_error("Kite order preflight did not succeed");
    }
    for (const json& order : orders.at("data")) {
        const std::string status = order.value("status", std::string());
        const bool terminal =
            status == "COMPLETE" ||
            status == "REJECTED" ||
            status == "CANCELLED";
        const std::string symbol =
            order.value("tradingsymbol", std::string());
        if (!terminal &&
            std::find(symbols.begin(), symbols.end(), symbol) !=
                symbols.end() &&
            order.value("product", std::string()) == "MIS") {
            throw std::runtime_error(
                "Live trading blocked: an open MIS order already exists for " +
                symbol);
        }
    }
    logLine("[kite_api] live_preflight_completed");
}

CashAvailability KiteConnectApi::cashAvailability() {
    const std::string response = httpRequest(
        "Kite equity margin fetch",
        L"GET", L"/user/margins/equity", std::string(),
        apiKey_, accessToken_);
    const json result = json::parse(response);
    if (result.value("status", std::string()) != "success") {
        throw std::runtime_error("Kite margin request did not succeed");
    }

    const json& equity = result.at("data");
    const json& available = equity.at("available");
    const double cash = available.at("cash").get<double>();
    const double intradayPayin =
        available.at("intraday_payin").get<double>();
    const double adhocMargin =
        available.at("adhoc_margin").get<double>();
    const double liveBalance =
        available.at("live_balance").get<double>();
    const double collateral =
        available.at("collateral").get<double>();
    const double utilized =
        equity.at("utilised").at("debits").get<double>();
    zerotick::logLine("[kite_api] cash_availability_loaded");
    return {
        cash,
        intradayPayin,
        adhocMargin,
        liveBalance,
        utilized,
        collateral,
        calculateFreeCash(cash, intradayPayin, utilized, liveBalance)
    };
}

OrderResult KiteConnectApi::placeStopLimitEntry(
    const std::string& symbol,
    int quantity,
    double triggerPrice,
    double limitPrice,
    PositionSide side) {
    const bool invalidLimit = side == PositionSide::Long
        ? limitPrice < triggerPrice
        : limitPrice > triggerPrice;
    if (quantity <= 0 || triggerPrice <= 0.0 || limitPrice <= 0.0 ||
        invalidLimit) {
        throw std::invalid_argument("Invalid stop-limit entry parameters");
    }
    const std::string body =
        "tradingsymbol=" + urlEncode(symbol) +
        "&exchange=NSE&transaction_type=" +
        (side == PositionSide::Long ? "BUY" : "SELL") +
        "&order_type=SL" +
        "&quantity=" + std::to_string(quantity) +
        "&product=MIS&validity=DAY&trigger_price=" +
        orderPrice(triggerPrice) + "&price=" + orderPrice(limitPrice);
    const std::string response = httpRequest(
        "Kite MIS stop-limit entry",
        L"POST", L"/orders/regular", body, apiKey_, accessToken_);
    const json result = json::parse(response);
    if (result.value("status", std::string()) != "success") {
        throw std::runtime_error("Kite did not accept the entry order");
    }
    logLine("[kite_api] entry_order_accepted symbol=" + symbol +
            " side=" +
            (side == PositionSide::Long ? "LONG" : "SHORT") +
            " quantity=" + std::to_string(quantity));
    return {
        true,
        result.at("data").at("order_id").get<std::string>(),
        "MIS stop-limit entry submitted"
    };
}

OrderResult KiteConnectApi::placeLimitExit(
    const std::string& symbol,
    int quantity,
    double limitPrice,
    PositionSide positionSide) {
    if (quantity <= 0 || limitPrice <= 0.0) {
        throw std::invalid_argument("Invalid limit-exit parameters");
    }
    const std::string body =
        "tradingsymbol=" + urlEncode(symbol) +
        "&exchange=NSE&transaction_type=" +
        (positionSide == PositionSide::Long ? "SELL" : "BUY") +
        "&order_type=LIMIT" +
        "&quantity=" + std::to_string(quantity) +
        "&product=MIS&validity=DAY&price=" + orderPrice(limitPrice);
    const std::string response = httpRequest(
        "Kite MIS limit exit",
        L"POST", L"/orders/regular", body, apiKey_, accessToken_);
    const json result = json::parse(response);
    if (result.value("status", std::string()) != "success") {
        throw std::runtime_error("Kite did not accept the exit order");
    }
    logLine("[kite_api] exit_order_accepted symbol=" + symbol +
            " side=" +
            (positionSide == PositionSide::Long ? "LONG" : "SHORT") +
            " quantity=" + std::to_string(quantity));
    return {
        true,
        result.at("data").at("order_id").get<std::string>(),
        "MIS limit exit submitted"
    };
}

BrokerOrderStatus KiteConnectApi::getOrderStatus(
    const std::string& orderId) {
    const std::string response = httpRequest(
        "Kite order status fetch",
        L"GET", widenAscii("/orders/" + urlEncode(orderId)),
        std::string(), apiKey_, accessToken_);
    const json result = json::parse(response);
    if (result.value("status", std::string()) != "success") {
        throw std::runtime_error("Kite order status request did not succeed");
    }

    const json& history = result.at("data");
    if (!history.is_array() || history.empty()) {
        throw std::runtime_error("Kite returned empty order history");
    }
    const json& latest = history.back();
    const std::string status = latest.at("status").get<std::string>();
    logLine("[kite_api] order_status_loaded status=" + status +
            " filled_quantity=" +
            std::to_string(latest.value("filled_quantity", 0)));
    return {
        status,
        latest.value("filled_quantity", 0),
        latest.value("average_price", 0.0),
        status == "COMPLETE" ||
            status == "REJECTED" ||
            status == "CANCELLED"
    };
}

void KiteConnectApi::cancelOrder(const std::string& orderId) {
    const std::string response = httpRequest(
        "Kite order cancellation",
        L"DELETE",
        widenAscii("/orders/regular/" + urlEncode(orderId)),
        std::string(), apiKey_, accessToken_);
    const json result = json::parse(response);
    if (result.value("status", std::string()) != "success") {
        throw std::runtime_error("Kite did not confirm order cancellation");
    }
    logLine("[kite_api] order_cancellation_confirmed");
}

bool KiteConnectApi::isHealthy() const {
    return healthy_;
}

} // namespace zerotick
