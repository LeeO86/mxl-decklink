// SPDX-License-Identifier: MIT
#include "ancwriter.hpp"

#include <cstring>
#include <stdexcept>

#include "util/logging.hpp"

namespace mxldl::mxlbridge
{
    AncWriter::AncWriter(Domain& domain, AncFlowParams const& params)
        : _domain(domain)
        , _flowIdString(params.id.toString())
    {
        auto const flowDef = buildAncFlowDef(params);
        bool created = false;
        auto const status = ::mxlCreateFlowWriter(domain.instance(), flowDef.c_str(), nullptr, &_writer, &_configInfo, &created);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlCreateFlowWriter (anc " + _flowIdString + ") failed with status " + std::to_string(status));
        }
        log::info("mxl_anc_flow_writer_created",
            {
                {"flow_id", _flowIdString},
                {"created", created},
            });
    }

    AncWriter::~AncWriter()
    {
        if (_writer != nullptr)
        {
            ::mxlReleaseFlowWriter(_domain.instance(), _writer);
        }
    }

    mxlStatus AncWriter::writePackets(std::uint64_t grainIndex, std::vector<util::AncPacket> const& packets)
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto status = ::mxlFlowWriterOpenGrain(_writer, grainIndex, &info, &payload);
        if (status != MXL_STATUS_OK)
        {
            return status;
        }

        auto const encoded = util::encodeRfc8331(packets, info.grainSize);
        if (encoded.packetsDropped > 0)
        {
            log::warn("anc_packets_dropped",
                {
                    {"flow_id", _flowIdString},
                    {"dropped", static_cast<std::uint64_t>(encoded.packetsDropped)},
                    {"grain_size", info.grainSize},
                });
        }
        std::memcpy(payload, encoded.payload.data(), encoded.payload.size());
        // Commit the grain as fully valid and zero-pad the tail: MXL treats a
        // grain as complete only when validSlices == totalSlices, and the
        // RFC 8331 Length field already carries the real payload size.
        if (encoded.payload.size() < info.grainSize)
        {
            std::memset(payload + encoded.payload.size(), 0, info.grainSize - encoded.payload.size());
        }

        info.flags = 0;
        info.validSlices = info.totalSlices;
        return ::mxlFlowWriterCommitGrain(_writer, &info);
    }
}
