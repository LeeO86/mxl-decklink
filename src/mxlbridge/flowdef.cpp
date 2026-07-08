// SPDX-License-Identifier: MIT
#include "flowdef.hpp"

#include "util/logging.hpp"

namespace mxldl::mxlbridge
{
    namespace
    {
        std::string esc(std::string const& s)
        {
            return log::jsonEscape(s);
        }

        std::string commonHead(util::Uuid const& id, std::string const& format, std::string const& label, std::string const& description,
            std::string const& groupHint)
        {
            std::string out;
            out += "\"id\":\"" + id.toString() + "\",";
            out += "\"format\":\"" + format + "\",";
            out += "\"label\":\"" + esc(label) + "\",";
            out += "\"description\":\"" + esc(description) + "\",";
            out += "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" + esc(groupHint) + "\"]},";
            out += "\"parents\":[],";
            return out;
        }

        std::string idFields(std::optional<util::Uuid> const& sourceId, std::optional<util::Uuid> const& deviceId)
        {
            std::string out;
            if (sourceId)
            {
                out += "\"source_id\":\"" + sourceId->toString() + "\",";
            }
            if (deviceId)
            {
                out += "\"device_id\":\"" + deviceId->toString() + "\",";
            }
            return out;
        }
    }

    std::string buildVideoFlowDef(VideoFlowParams const& p)
    {
        std::string out = "{";
        out += commonHead(p.id, "urn:x-nmos:format:video", p.label, p.description, p.groupHint);
        out += idFields(p.sourceId, p.deviceId);
        out += "\"media_type\":\"";
        out += p.withAlpha ? "video/v210a" : "video/v210";
        out += "\",";
        out += "\"grain_rate\":{\"numerator\":" + std::to_string(p.rateNumerator) + ",\"denominator\":" + std::to_string(p.rateDenominator) + "},";
        out += "\"frame_width\":" + std::to_string(p.width) + ",";
        out += "\"frame_height\":" + std::to_string(p.height) + ",";
        out += "\"interlace_mode\":\"";
        out += p.interlaced ? "interlaced_tff" : "progressive";
        out += "\",";
        out += "\"colorspace\":\"BT709\",";
        out += "\"components\":[";
        out += "{\"name\":\"Y\",\"width\":" + std::to_string(p.width) + ",\"height\":" + std::to_string(p.height) + ",\"bit_depth\":10},";
        out += "{\"name\":\"Cb\",\"width\":" + std::to_string(p.width / 2) + ",\"height\":" + std::to_string(p.height) + ",\"bit_depth\":10},";
        out += "{\"name\":\"Cr\",\"width\":" + std::to_string(p.width / 2) + ",\"height\":" + std::to_string(p.height) + ",\"bit_depth\":10}";
        if (p.withAlpha)
        {
            out += ",{\"name\":\"A\",\"width\":" + std::to_string(p.width) + ",\"height\":" + std::to_string(p.height) + ",\"bit_depth\":10}";
        }
        out += "]}";
        return out;
    }

    std::string buildAudioFlowDef(AudioFlowParams const& p)
    {
        std::string out = "{";
        out += commonHead(p.id, "urn:x-nmos:format:audio", p.label, p.description, p.groupHint);
        out += idFields(p.sourceId, p.deviceId);
        out += "\"media_type\":\"audio/float32\",";
        out += "\"sample_rate\":{\"numerator\":48000,\"denominator\":1},";
        out += "\"channel_count\":" + std::to_string(p.channelCount) + ",";
        out += "\"bit_depth\":32}";
        return out;
    }

    std::string buildAncFlowDef(AncFlowParams const& p)
    {
        std::string out = "{";
        out += commonHead(p.id, "urn:x-nmos:format:data", p.label, p.description, p.groupHint);
        out += idFields(p.sourceId, p.deviceId);
        out += "\"media_type\":\"video/smpte291\",";
        out += "\"grain_rate\":{\"numerator\":" + std::to_string(p.rateNumerator) + ",\"denominator\":" + std::to_string(p.rateDenominator) + "}}";
        return out;
    }

    std::string buildWriterOptions(int commitBatchHint)
    {
        return "{\"maxCommitBatchSizeHint\":" + std::to_string(commitBatchHint) + "}";
    }
}
