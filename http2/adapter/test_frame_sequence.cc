#include "http2/adapter/test_frame_sequence.h"

#include "http2/adapter/http2_util.h"
#include "http2/adapter/oghttp2_util.h"
#include "spdy/core/hpack/hpack_encoder.h"
#include "spdy/core/spdy_framer.h"

namespace http2 {
namespace adapter {
namespace test {

std::vector<const Header> ToHeaders(
    absl::Span<const std::pair<absl::string_view, absl::string_view>> headers) {
  std::vector<const Header> out;
  for (const auto& header : headers) {
    out.push_back(
        std::make_pair(HeaderRep(header.first), HeaderRep(header.second)));
  }
  return out;
}

TestFrameSequence& TestFrameSequence::ClientPreface() {
  preface_ = spdy::kHttp2ConnectionHeaderPrefix;
  frames_.push_back(absl::make_unique<spdy::SpdySettingsIR>());
  return *this;
}

TestFrameSequence& TestFrameSequence::ServerPreface() {
  frames_.push_back(absl::make_unique<spdy::SpdySettingsIR>());
  return *this;
}

TestFrameSequence& TestFrameSequence::Data(Http2StreamId stream_id,
                                           absl::string_view payload,
                                           bool fin,
                                           absl::optional<int> padding_length) {
  auto data = absl::make_unique<spdy::SpdyDataIR>(stream_id, payload);
  data->set_fin(fin);
  if (padding_length) {
    data->set_padding_len(padding_length.value());
  }
  frames_.push_back(std::move(data));
  return *this;
}

TestFrameSequence& TestFrameSequence::RstStream(Http2StreamId stream_id,
                                                Http2ErrorCode error) {
  frames_.push_back(absl::make_unique<spdy::SpdyRstStreamIR>(
      stream_id, TranslateErrorCode(error)));
  return *this;
}

TestFrameSequence& TestFrameSequence::Settings(
    absl::Span<Http2Setting> values) {
  auto settings = absl::make_unique<spdy::SpdySettingsIR>();
  for (const Http2Setting& setting : values) {
    settings->AddSetting(setting.id, setting.value);
  }
  frames_.push_back(std::move(settings));
  return *this;
}

TestFrameSequence& TestFrameSequence::SettingsAck() {
  auto settings = absl::make_unique<spdy::SpdySettingsIR>();
  settings->set_is_ack(true);
  frames_.push_back(std::move(settings));
  return *this;
}

TestFrameSequence& TestFrameSequence::Ping(Http2PingId id) {
  frames_.push_back(absl::make_unique<spdy::SpdyPingIR>(id));
  return *this;
}

TestFrameSequence& TestFrameSequence::PingAck(Http2PingId id) {
  auto ping = absl::make_unique<spdy::SpdyPingIR>(id);
  ping->set_is_ack(true);
  frames_.push_back(std::move(ping));
  return *this;
}

TestFrameSequence& TestFrameSequence::GoAway(Http2StreamId last_good_stream_id,
                                             Http2ErrorCode error,
                                             absl::string_view payload) {
  frames_.push_back(absl::make_unique<spdy::SpdyGoAwayIR>(
      last_good_stream_id, TranslateErrorCode(error), std::string(payload)));
  return *this;
}

TestFrameSequence& TestFrameSequence::Headers(
    Http2StreamId stream_id,
    absl::Span<const std::pair<absl::string_view, absl::string_view>> headers,
    bool fin, bool add_continuation) {
  return Headers(stream_id, ToHeaders(headers), fin, add_continuation);
}

TestFrameSequence& TestFrameSequence::Headers(Http2StreamId stream_id,
                                              spdy::Http2HeaderBlock block,
                                              bool fin, bool add_continuation) {
  if (add_continuation) {
    // The normal intermediate representations don't allow you to represent a
    // nonterminal HEADERS frame explicitly, so we'll need to use
    // SpdyUnknownIRs. For simplicity, and in order not to mess up HPACK state,
    // the payload will be uncompressed.
    std::string encoded_block;
    spdy::HpackEncoder encoder;
    encoder.DisableCompression();
    encoder.EncodeHeaderSet(block, &encoded_block);
    const size_t pos = encoded_block.size() / 2;
    const uint8_t flags = fin ? 0x1 : 0x0;
    frames_.push_back(absl::make_unique<spdy::SpdyUnknownIR>(
        stream_id, static_cast<uint8_t>(spdy::SpdyFrameType::HEADERS), flags,
        encoded_block.substr(0, pos)));

    auto continuation = absl::make_unique<spdy::SpdyContinuationIR>(stream_id);
    continuation->set_end_headers(true);
    continuation->take_encoding(encoded_block.substr(pos));
    frames_.push_back(std::move(continuation));
  } else {
    auto headers =
        absl::make_unique<spdy::SpdyHeadersIR>(stream_id, std::move(block));
    headers->set_fin(fin);
    frames_.push_back(std::move(headers));
  }
  return *this;
}

TestFrameSequence& TestFrameSequence::Headers(Http2StreamId stream_id,
                                              absl::Span<const Header> headers,
                                              bool fin, bool add_continuation) {
  return Headers(stream_id, ToHeaderBlock(headers), fin, add_continuation);
}

TestFrameSequence& TestFrameSequence::WindowUpdate(Http2StreamId stream_id,
                                                   int32_t delta) {
  frames_.push_back(
      absl::make_unique<spdy::SpdyWindowUpdateIR>(stream_id, delta));
  return *this;
}

TestFrameSequence& TestFrameSequence::Priority(Http2StreamId stream_id,
                                               Http2StreamId parent_stream_id,
                                               int weight,
                                               bool exclusive) {
  frames_.push_back(absl::make_unique<spdy::SpdyPriorityIR>(
      stream_id, parent_stream_id, weight, exclusive));
  return *this;
}

TestFrameSequence& TestFrameSequence::Metadata(Http2StreamId stream_id,
                                               absl::string_view payload) {
  // Encode the payload using a header block.
  spdy::SpdyHeaderBlock block;
  block["example-payload"] = payload;
  spdy::HpackEncoder encoder;
  encoder.DisableCompression();
  std::string encoded_payload;
  encoder.EncodeHeaderSet(block, &encoded_payload);
  frames_.push_back(absl::make_unique<spdy::SpdyUnknownIR>(
      stream_id, kMetadataFrameType, kMetadataEndFlag,
      std::move(encoded_payload)));
  return *this;
}

std::string TestFrameSequence::Serialize() {
  std::string result;
  if (!preface_.empty()) {
    result = preface_;
  }
  spdy::SpdyFramer framer(spdy::SpdyFramer::ENABLE_COMPRESSION);
  for (const auto& frame : frames_) {
    spdy::SpdySerializedFrame f = framer.SerializeFrame(*frame);
    absl::StrAppend(&result, absl::string_view(f));
  }
  return result;
}

}  // namespace test
}  // namespace adapter
}  // namespace http2
