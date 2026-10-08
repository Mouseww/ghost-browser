#include "cdp.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace ghost {
namespace {

constexpr DWORD kPollMillis = 10;

}  // namespace

std::string cdp_last_error() {
  const DWORD code = GetLastError();
  char* text = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<char*>(&text), 0, nullptr);
  std::string message = "win32 error " + std::to_string(code);
  if (length != 0 && text != nullptr) {
    std::string detail(text, length);
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r' ||
                               detail.back() == ' ')) {
      detail.pop_back();
    }
    if (!detail.empty()) message += ": " + detail;
  }
  if (text != nullptr) LocalFree(text);
  return message;
}

CdpClient::~CdpClient() { close(); }

void CdpClient::adopt(HANDLE read_end, HANDLE write_end) {
  close();
  read_ = read_end;
  write_ = write_end;
  pending_.clear();
  next_id_ = 1;
  page_session_.clear();
}

void CdpClient::close() {
  if (read_ != nullptr) CloseHandle(read_);
  if (write_ != nullptr) CloseHandle(write_);
  read_ = nullptr;
  write_ = nullptr;
}

bool CdpClient::write_message(const std::string& text) {
  if (write_ == nullptr) return false;
  std::string framed = text;
  framed.push_back('\0');
  DWORD written = 0;
  const BOOL ok = WriteFile(write_, framed.data(),
                            static_cast<DWORD>(framed.size()), &written, nullptr);
  return ok != FALSE && written == framed.size();
}

bool CdpClient::read_message(std::string* text, int timeout_ms) {
  if (read_ == nullptr) return false;
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  for (;;) {
    // A whole message may already be buffered from an earlier read.
    const std::size_t terminator = pending_.find('\0');
    if (terminator != std::string::npos) {
      *text = pending_.substr(0, terminator);
      pending_.erase(0, terminator + 1);
      return true;
    }

    DWORD available = 0;
    if (PeekNamedPipe(read_, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
      return false;
    }
    if (available > 0) {
      char buffer[8192];
      DWORD chunk = available < sizeof(buffer) ? available : sizeof(buffer);
      DWORD got = 0;
      if (ReadFile(read_, buffer, chunk, &got, nullptr) == FALSE) return false;
      if (got > 0) pending_.append(buffer, got);
      continue;
    }
    if (GetTickCount64() >= deadline) return false;
    Sleep(kPollMillis);
  }
}

bool CdpClient::call(const std::string& method, const Json& params,
                     const std::string& session_id, Json* result, std::string* error,
                     int timeout_ms) {
  if (!connected()) {
    *error = "no DevTools pipe is attached to this session";
    return false;
  }
  const int id = next_id_++;
  Json request = Json::object();
  request.set("id", Json::integer(id));
  request.set("method", Json::string(method));
  if (params.is_object() && !params.fields().empty()) request.set("params", params);
  if (!session_id.empty()) request.set("sessionId", Json::string(session_id));

  if (!write_message(request.dump())) {
    *error = "the DevTools pipe refused a write: " + cdp_last_error();
    return false;
  }

  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      *error = "the browser did not answer " + method + " within " +
               std::to_string(timeout_ms) + " ms";
      return false;
    }
    std::string text;
    if (!read_message(&text, static_cast<int>(deadline - now))) {
      *error = "the DevTools pipe went quiet while waiting for " + method;
      return false;
    }
    if (text.empty()) continue;

    std::string parse_error;
    const Json message = Json::parse(text, &parse_error);
    if (!message.is_object()) continue;

    const Json* message_id = message.find("id");
    // An event carries no id. It is not the answer, and treating it as one would
    // hand the caller the wrong object.
    if (message_id == nullptr || !message_id->is_number()) continue;
    if (static_cast<int>(message_id->as_number(-1)) != id) continue;

    const Json* problem = message.find("error");
    if (problem != nullptr && problem->is_object()) {
      const Json* message_text = problem->find("message");
      *error = message_text != nullptr ? message_text->as_string("the browser refused")
                                       : std::string("the browser refused");
      return false;
    }
    const Json* payload = message.find("result");
    *result = payload != nullptr ? *payload : Json::object();
    return true;
  }
}

bool CdpClient::call(const std::string& method, Json* result, std::string* error,
                     int timeout_ms) {
  return call(method, Json::object(), std::string(), result, error, timeout_ms);
}

bool CdpClient::attach_to_page(bool force, std::string* error, int timeout_ms) {
  if (!force && !page_session_.empty()) return true;

  Json targets;
  if (!call("Target.getTargets", &targets, error, timeout_ms)) return false;
  const Json* list = targets.find("targetInfos");
  if (list == nullptr || !list->is_array()) {
    *error = "the browser listed no targets";
    return false;
  }

  std::string target_id;
  for (const Json& item : list->items()) {
    const Json* type = item.find("type");
    if (type == nullptr || type->as_string() != "page") continue;
    const Json* id = item.find("targetId");
    if (id == nullptr) continue;
    target_id = id->as_string();
    break;
  }
  if (target_id.empty()) {
    *error = "the browser has no page target yet";
    return false;
  }

  Json params = Json::object();
  params.set("targetId", Json::string(target_id));
  // flatten:true keeps the session on this one connection instead of opening a
  // second pipe per target.
  params.set("flatten", Json::boolean(true));
  Json attached;
  if (!call("Target.attachToTarget", params, std::string(), &attached, error, timeout_ms)) {
    return false;
  }
  const Json* session = attached.find("sessionId");
  if (session == nullptr) {
    *error = "the browser attached to the page without a session id";
    return false;
  }
  page_session_ = session->as_string();
  return true;
}

bool CdpClient::attach_to_page(std::string* error, int timeout_ms) {
  return attach_to_page(false, error, timeout_ms);
}

bool CdpClient::evaluate(const std::string& expression, std::string* value,
                         std::string* error, int timeout_ms) {
  if (page_session_.empty()) {
    if (!attach_to_page(error, timeout_ms)) return false;
  }
  Json params = Json::object();
  params.set("expression", Json::string(expression));
  // returnByValue keeps the answer in the reply instead of behind an object id
  // that would then need a second round trip to read.
  params.set("returnByValue", Json::boolean(true));
  params.set("awaitPromise", Json::boolean(true));

  Json reply;
  if (!call("Runtime.evaluate", params, page_session_, &reply, error, timeout_ms)) {
    // A navigation destroys the session, and the next command would fail the same
    // way. Re-attaching once turns a stale session into a retry rather than an
    // error the caller has to understand.
    if (!attach_to_page(true, error, timeout_ms)) return false;
    if (!call("Runtime.evaluate", params, page_session_, &reply, error, timeout_ms)) {
      return false;
    }
  }

  const Json* exception = reply.find("exceptionDetails");
  if (exception != nullptr) {
    const Json* text = exception->find("text");
    *error = "the page threw: " +
             (text != nullptr ? text->as_string("unknown") : std::string("unknown"));
    return false;
  }
  const Json* result = reply.find("result");
  if (result == nullptr) {
    *error = "the page returned nothing";
    return false;
  }
  const Json* inner = result->find("value");
  if (inner == nullptr) {
    *value = std::string();
    return true;
  }
  *value = inner->is_string() ? inner->as_string() : inner->dump();
  return true;
}

}  // namespace ghost
