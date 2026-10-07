/// @file
/// A client for an OpenAI-compatible chat-completions endpoint: one request,
/// one reply, with retries.
///
/// ChatClientConfig names the endpoint, the model and the request limits. Its
/// defaults name the National University of Singapore (NUS) service, and
/// apiKey has no default, so a caller that needs a key sets one. A caller that
/// sets baseUrl and model names a different endpoint, for instance a model
/// served on the same machine.
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace utilities {

/// What one chat-completions request produced. requestModelReply leaves
/// content empty and writes the reason into error when a request failed.
/// promptTokens, completionTokens and totalTokens repeat what the endpoint
/// reported about the request, and stay at zero when the endpoint reported no
/// usage object.
struct ModelReply {
  std::string content;
  std::string error;
  long long promptTokens = 0;
  long long completionTokens = 0;
  long long totalTokens = 0;
};

struct ChatClientConfig {
  std::string baseUrl = "https://soclaas-api.comp.nus.edu.sg/v1";
  std::string apiKey;
  // The id the SoC endpoint answered with on 2026-10-05, so that the default
  // baseUrl and the default model name one endpoint between them. gpt-4o-mini
  // was left here when baseUrl still pointed at OpenAI, and that endpoint has
  // never served it.
  std::string model = "qwen3-coder-next";
  double temperature = 0.8;
  int maxTokens = 4096;
  int retries = 3;
  long timeoutSeconds = 180;
};

namespace chatClientDetail {

inline std::size_t appendToString(char* data, std::size_t size, std::size_t count,
                                   void* destination) {
  static_cast<std::string*>(destination)->append(data, size * count);
  return size * count;
}

struct EasyHandleDeleter {
  void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};

struct HeaderListDeleter {
  void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

using EasyHandle = std::unique_ptr<CURL, EasyHandleDeleter>;
using HeaderList = std::unique_ptr<curl_slist, HeaderListDeleter>;

}  // namespace chatClientDetail

inline ModelReply requestModelReply(const ChatClientConfig& config,
                                             const std::string& systemMessage,
                                             const std::string& userMessage) {
  const auto url = std::string{config.baseUrl + "/chat/completions"};
  const auto payload =
      nlohmann::json{
          {"model", config.model},
          {"temperature", config.temperature},
          {"max_tokens", config.maxTokens},
          {"messages", nlohmann::json::array({
                            {{"role", "system"}, {"content", systemMessage}},
                            {{"role", "user"}, {"content", userMessage}},
                        })},
      }
          .dump();  auto lastError = std::string{"requestModelReply sent no request, because retries is zero"};
  for (auto attempt = int{1}; attempt <= config.retries; ++attempt) {
    auto handle = chatClientDetail::EasyHandle{curl_easy_init()};
    if (!handle) return ModelReply{"", "curl_easy_init failed"};
    auto headers = chatClientDetail::HeaderList{
        curl_slist_append(nullptr, "Content-Type: application/json")};
    if (!config.apiKey.empty())
      headers.reset(curl_slist_append(headers.release(),
                                      ("Authorization: Bearer " + config.apiKey).c_str()));

    auto response = std::string{};
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, chatClientDetail::appendToString);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, config.timeoutSeconds);

    const auto performed = CURLcode{curl_easy_perform(handle.get())};
    auto status = long{0};
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);

    if (performed != CURLE_OK) {
      lastError = std::string("transport: ") + curl_easy_strerror(performed);
    } else if (status == 429 || status >= 500) {
      lastError = "http " + std::to_string(status) + ": " + response.substr(0, 300);
    } else if (status != 200) {
      // requestModelReply returns a 4xx other than 429 without retrying,
      // because the endpoint rejected the request itself rather than refusing
      // it for load.
      return ModelReply{
          "", "http " + std::to_string(status) + ": " + response.substr(0, 300)};
    } else {
      try {
        const auto parsed = nlohmann::json::parse(response);
        auto reply = ModelReply{};
        reply.content =
            parsed.at("choices").at(0).at("message").at("content").get<std::string>();
        // An endpoint that answers with no usage object leaves promptTokens,
        // completionTokens and totalTokens at zero, so a reader sees zero
        // rather than an estimate.
        if (parsed.contains("usage")) {
          const auto& usage = parsed.at("usage");
          reply.promptTokens = usage.value("prompt_tokens", 0LL);
          reply.completionTokens = usage.value("completion_tokens", 0LL);
          reply.totalTokens = usage.value("total_tokens", 0LL);
        }
        return reply;
      } catch (const std::exception& exception) {
        return ModelReply{"", std::string("malformed response: ") + exception.what()};
      }
    }
    if (attempt < config.retries) std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
  }
  return ModelReply{"", lastError};
}

}  // namespace utilities
