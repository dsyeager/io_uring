#pragma once

#include <string_view>

enum class http_11_action { WAIT, AGAIN, DONE, TERM, FAIL };

const char* to_str(http_11_action action)
{
    using enum http_11_action;
    switch (action) {
    case WAIT: return "WAIT";
    case AGAIN: return "AGAIN";
    case DONE: return "DONE";
    case TERM: return "TERM";
    case FAIL: return "FAIL";
    default: return "UNHANDLED";
    }
}

enum class http_11_state {
    CREATED,
    CONNECTING,
    CONNECTED,
    OPENING_GET_FILE,
    READING_GET_FILE,
    OPENING_PUT_FILE,
    WRITING_REQUEST,
    READING_RESPONSE_HEADERS,
    READING_RESPONSE_BODY,
    READING_REQUEST_HEADERS,
    READING_REQUEST_BODY,
    WRITING_RESPONSE_HEADERS,
    WRITING_RESPONSE_BODY,
    DONE,
    FAILED
};

const char* to_str(http_11_state st)
{
    using enum http_11_state;
    switch (st) {
    case CREATED: return "CREATED";
    case CONNECTING: return "CONNECTING";
    case CONNECTED: return "CONNECTED";
    case OPENING_GET_FILE: return "OPENING_GET_FILE";
    case READING_GET_FILE: return "READING_GET_FILE";
    case OPENING_PUT_FILE: return "OPENING_PUT_FILE";
    case WRITING_REQUEST: return "WRITING_REQUEST";
    case READING_RESPONSE_HEADERS: return "READING_RESPONSE_HEADERS";
    case READING_RESPONSE_BODY: return "READING_RESPONSE_BODY";
    case READING_REQUEST_HEADERS: return "READING_REQUEST_HEADERS";
    case READING_REQUEST_BODY: return "READING_REQUEST_BODY";
    case WRITING_RESPONSE_HEADERS: return "WRITING_RESPONSE_HEADERS";
    case WRITING_RESPONSE_BODY: return "WRITING_RESPONSE_BODY";
    case DONE: return "DONE";
    case FAILED: return "FAILED";
    default: return "UNHANDLED";
    }
}

enum class request_type { GET, PUT, UNHANDLED };

const char* to_str(request_type rt)
{
    using enum request_type;
    switch(rt) {
    case GET: return "GET";
    case PUT: return "PUT";
    default: return "UNHANDLED";
    };
}

request_type from_str(std::string_view str)
{
    using enum request_type;
    if (str == "GET") return GET;
    else if (str == "PUT") return PUT;
    return UNHANDLED;
}

