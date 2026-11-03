#pragma once
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mining.h"
#include "pauseDiagnostics.h"
#include <mutex>

struct HttpJsonError {};
class BoundedHttpStream : public Stream {
  Stream &input_;
  size_t left_=32768;
  uint32_t started_=millis();
public:
  explicit BoundedHttpStream(Stream &input):input_(input){setTimeout(2000);}
  int available() override {return left_ && uint32_t(millis()-started_)<5000?input_.available():0;}
  int read() override {
    if(!left_ || uint32_t(millis()-started_)>=5000)return -1;
    const int c=input_.read();if(c>=0)--left_;return c;
  }
  int peek() override {return left_ && uint32_t(millis()-started_)<5000?input_.peek():-1;}
  void flush() override {}
  size_t write(uint8_t) override {return 0;}
};
static int streamingGet(HTTPClient &http, BackgroundFetch kind) {
  static const char *headers[]={"Transfer-Encoding","Content-Encoding"};
  http.useHTTP10(true);http.setReuse(false);
  http.collectHeaders(headers,2);
  http.addHeader("Accept-Encoding","identity");
  return measuredBackgroundGet(http,kind);
}
template<class Document, class... Options>
static void readHttpJson(HTTPClient &http, Document &doc, Options... options) {
  // Raw HTTPClient streams do not decode chunking. Request close framing
  // and fail cleanly if a server nevertheless sends an encoded response.
  if(http.header("Transfer-Encoding").length() ||
     (http.header("Content-Encoding").length() && http.header("Content-Encoding")!="identity"))throw HttpJsonError{};
  // TLS body reads can use SHA as well as the handshake.
  HW_SHA_REQUEST();std::lock_guard<std::mutex> sha(g_hwShaMutex);
  BoundedHttpStream stream(http.getStream());
  if(deserializeJson(doc,stream,options...))throw HttpJsonError{};
}
