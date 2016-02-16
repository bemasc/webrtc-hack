/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/rtp_rtcp/source/rtcp_packet/fir.h"

#include "webrtc/base/checks.h"
#include "webrtc/base/logging.h"
#include "webrtc/modules/rtp_rtcp/source/byte_io.h"

using webrtc::RTCPUtility::RtcpCommonHeader;

namespace webrtc {
namespace rtcp {
// RFC 4585: Feedback format.
// Common packet format:
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P|   FMT   |       PT      |          length               |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                  SSRC of packet sender                        |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |             SSRC of media source (unused) = 0                 |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  :            Feedback Control Information (FCI)                 :
//  :                                                               :
// Full intra request (FIR) (RFC 5104).
// The Feedback Control Information (FCI) for the Full Intra Request
// consists of one or more FCI entries.
// FCI:
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                              SSRC                             |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  | Seq nr.       |    Reserved = 0                               |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
bool Fir::Parse(const RtcpCommonHeader& header, const uint8_t* payload) {
  RTC_CHECK(header.packet_type == kPacketType);
  RTC_CHECK(header.count_or_format == kFeedbackMessageType);

  // The FCI field MUST contain one or more FIR entries.
  if (header.payload_size_bytes < kCommonFeedbackLength + kFciLength) {
    LOG(LS_WARNING) << "Packet is too small to be a valid FIR packet.";
    return false;
  }

  if ((header.payload_size_bytes - kCommonFeedbackLength) % kFciLength != 0) {
    LOG(LS_WARNING) << "Invalid size for a valid FIR packet.";
    return false;
  }

  ParseCommonFeedback(payload);

  size_t number_of_fci_items =
      (header.payload_size_bytes - kCommonFeedbackLength) / kFciLength;
  const uint8_t* next_fci = payload + kCommonFeedbackLength;
  items_.resize(number_of_fci_items);
  for (Request& request : items_) {
    request.ssrc = ByteReader<uint32_t>::ReadBigEndian(next_fci);
    request.seq_nr = ByteReader<uint8_t>::ReadBigEndian(next_fci + 4);
    next_fci += kFciLength;
  }
  return true;
}

bool Fir::Create(uint8_t* packet,
                 size_t* index,
                 size_t max_length,
                 RtcpPacket::PacketReadyCallback* callback) const {
  RTC_DCHECK(!items_.empty());
  while (*index + BlockLength() > max_length) {
    if (!OnBufferFull(packet, index, callback))
      return false;
  }
  size_t index_end = *index + BlockLength();
  CreateHeader(kFeedbackMessageType, kPacketType, HeaderLength(), packet,
               index);
  RTC_DCHECK_EQ(Psfb::media_ssrc(), 0u);
  CreateCommonFeedback(packet + *index);
  *index += kCommonFeedbackLength;

  const uint32_t kReserved = 0;
  for (const Request& request : items_) {
    ByteWriter<uint32_t>::WriteBigEndian(packet + *index, request.ssrc);
    ByteWriter<uint8_t>::WriteBigEndian(packet + *index + 4, request.seq_nr);
    ByteWriter<uint32_t, 3>::WriteBigEndian(packet + *index + 5, kReserved);
    *index += kFciLength;
  }
  RTC_CHECK_EQ(*index, index_end);
  return true;
}
}  // namespace rtcp
}  // namespace webrtc