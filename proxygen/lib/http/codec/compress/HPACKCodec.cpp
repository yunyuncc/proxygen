/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <proxygen/lib/http/codec/compress/HPACKCodec.h>

#include <algorithm>
#include <folly/ThreadLocal.h>
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include <proxygen/lib/http/codec/compress/HPACKHeader.h>
#include <proxygen/lib/http/codec/HeaderConstants.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <iosfwd>

using folly::IOBuf;
using folly::io::Cursor;
using proxygen::compress::Header;
using std::unique_ptr;
using std::vector;

namespace proxygen {

namespace compress {
uint32_t prepareHeaders(const vector<Header>& headers,
                        vector<HPACKHeader>& converted) {
  // convert to HPACK API format
  uint32_t uncompressed = 0;
  converted.clear();
  converted.reserve(headers.size());
  for (const auto& h : headers) {
    // HPACKHeader automatically lowercases
    converted.emplace_back(*h.name, *h.value);
    auto& header = converted.back();
    uncompressed += header.name.size() + header.value.size() + 2;
  }
  return uncompressed;
}
}

HPACKCodec::HPACKCodec(TransportDirection /*direction*/)
    : encoder_(true, HPACK::kTableSize),
      decoder_(HPACK::kTableSize, maxUncompressed_) {}

unique_ptr<IOBuf> HPACKCodec::encode(vector<Header>& headers) noexcept {
  folly::ThreadLocal<std::vector<HPACKHeader>> preparedTL;
  auto& prepared = *preparedTL.get();
  encodedSize_.uncompressed = compress::prepareHeaders(headers, prepared);
  auto buf = encoder_.encode(prepared, encodeHeadroom_);
  recordCompressedSize(buf.get());
  return buf;
}

void HPACKCodec::encode(
  vector<Header>& headers, folly::IOBufQueue& writeBuf) noexcept {
  folly::ThreadLocal<vector<HPACKHeader>> preparedTL;
  auto& prepared = *preparedTL.get();
  encodedSize_.uncompressed = compress::prepareHeaders(headers, prepared);
  auto prevSize = writeBuf.chainLength();
  encoder_.encode(prepared, writeBuf);
  recordCompressedSize(writeBuf.chainLength() - prevSize);
}

void HPACKCodec::encodeHTTP(
  const HTTPMessage& msg, folly::IOBufQueue& writeBuf) noexcept {
  auto prevSize = writeBuf.chainLength();
  encoder_.startEncode(writeBuf);

  auto uncompressed = 0;
  if (msg.isRequest()) {
    if (msg.isEgressWebsocketUpgrade()) {
      uncompressed += encoder_.encodeHeader(
        HTTP_HEADER_COLON_METHOD,
        methodToString(HTTPMethod::CONNECT));
      uncompressed += encoder_.encodeHeader(HTTP_HEADER_COLON_PROTOCOL,
                                            headers::kWebsocketString);
    } else {
      uncompressed += encoder_.encodeHeader(HTTP_HEADER_COLON_METHOD,
                                            msg.getMethodString());
    }

    if (msg.getMethod() != HTTPMethod::CONNECT ||
        msg.isEgressWebsocketUpgrade()) {
      uncompressed += encoder_.encodeHeader(
        HTTP_HEADER_COLON_SCHEME,
        (msg.isSecure() ? headers::kHttps : headers::kHttp));
      uncompressed += encoder_.encodeHeader(
        HTTP_HEADER_COLON_PATH, msg.getURL());
    }
    const HTTPHeaders& headers = msg.getHeaders();
    const std::string& host = headers.getSingleOrEmpty(HTTP_HEADER_HOST);
    if (!host.empty()) {
      uncompressed += encoder_.encodeHeader(HTTP_HEADER_COLON_AUTHORITY, host);
    }
  } else {
    if (msg.isEgressWebsocketUpgrade()) {
      uncompressed += encoder_.encodeHeader(
        HTTP_HEADER_COLON_STATUS, headers::kStatus200);
    } else {
      uncompressed += encoder_.encodeHeader(
        HTTP_HEADER_COLON_STATUS,
        folly::to<std::string>(msg.getStatusCode()));
    }
    // HEADERS frames do not include a version or reason string.
  }

  bool hasDateHeader = false;
  // Add the HTTP headers supplied by the caller, but skip
  // any per-hop headers that aren't supported in HTTP/2.
  msg.getHeaders().forEachWithCode([&](HTTPHeaderCode code,
                                       const std::string& name,
                                       const std::string& value) {
    static const std::bitset<256> s_perHopHeaderCodes{[] {
      std::bitset<256> bs;
      // HTTP/1.x per-hop headers that have no meaning in HTTP/2
      bs[HTTP_HEADER_CONNECTION] = true;
      bs[HTTP_HEADER_HOST] = true;
      bs[HTTP_HEADER_KEEP_ALIVE] = true;
      bs[HTTP_HEADER_PROXY_CONNECTION] = true;
      bs[HTTP_HEADER_TRANSFER_ENCODING] = true;
      bs[HTTP_HEADER_UPGRADE] = true;
      bs[HTTP_HEADER_SEC_WEBSOCKET_KEY] = true;
      bs[HTTP_HEADER_SEC_WEBSOCKET_ACCEPT] = true;
      return bs;
    }()};

    if (s_perHopHeaderCodes[code] || name.empty() || name[0] == ':') {
      DCHECK(!name.empty()) << "Empty header";
      DCHECK_NE(name[0], ':') << "Invalid header=" << name;
      return;
    }
    // Note this code will not drop headers named by Connection.  That's the
    // caller's job

    // see HTTP/2 spec, 8.1.2
    DCHECK(name != "TE" || value == "trailers");
    if ((!name.empty() && name[0] != ':') && code != HTTP_HEADER_HOST) {
      if (code == HTTP_HEADER_OTHER) {
        uncompressed += encoder_.encodeHeader(name, value);
      } else {
        uncompressed += encoder_.encodeHeader(code, value);
      }
    }
    hasDateHeader =  (code == HTTP_HEADER_DATE);
  });


  if (msg.isResponse() && !hasDateHeader) {
    uncompressed += encoder_.encodeHeader(
      HTTP_HEADER_DATE, HTTPMessage::formatDateHeader());
  }

  encoder_.completeEncode();
  encodedSize_.uncompressed = uncompressed;
  recordCompressedSize(writeBuf.chainLength() - prevSize);
}


void HPACKCodec::recordCompressedSize(
  const IOBuf* stream) {
  encodedSize_.compressed = 0;
  if (stream) {
    auto streamDataLength = stream->computeChainDataLength();
    encodedSize_.compressed += streamDataLength;
    encodedSize_.compressedBlock += streamDataLength;
  }
  if (stats_) {
    stats_->recordEncode(Type::HPACK, encodedSize_);
  }
}

void HPACKCodec::recordCompressedSize(
  size_t size) {
  encodedSize_.compressed = size;
  encodedSize_.compressedBlock += size;
  if (stats_) {
    stats_->recordEncode(Type::HPACK, encodedSize_);
  }
}

void HPACKCodec::decodeStreaming(
    Cursor& cursor,
    uint32_t length,
    HPACK::StreamingCallback* streamingCb) noexcept {
  streamingCb->stats = stats_;
  decoder_.decodeStreaming(cursor, length, streamingCb);
}

void HPACKCodec::describe(std::ostream& stream) const {
  stream << "DecoderTable:\n" << decoder_;
  stream << "EncoderTable:\n" << encoder_;
}

std::ostream& operator<<(std::ostream& os, const HPACKCodec& codec) {
  codec.describe(os);
  return os;
}

}
