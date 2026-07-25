// SPDX-License-Identifier: MIT
#include "channel_manager.hpp"

#include <cstdio>

#include "util/logging.hpp"

namespace mxldl::channel
{
    namespace
    {
        std::string cardIdLabel(dl::ICard const& card)
        {
            char buf[16];
            ::snprintf(buf, sizeof(buf), "0x%08x", card.persistentId());
            return buf;
        }
    }

    ChannelManager::ChannelManager(config::Config const& cfg, dl::ICard& card, mxlbridge::Domain& domain, ops::Registry& metrics)
        : _globalCfg(cfg)
        , _card(card)
        , _domain(domain)
        , _metrics(metrics)
    {
        for (auto const& ch : cfg.channels)
        {
            _entries.emplace(ch.index, Entry{ch, nullptr, nullptr});
        }
    }

    ChannelManager::~ChannelManager()
    {
        stopAll();
    }

    void ChannelManager::startEntry(Entry& entry)
    {
        auto const idLabel = cardIdLabel(_card);
        auto& sub = _card.subDevice(static_cast<std::size_t>(entry.cfg.subdeviceIndex));
        if (entry.cfg.direction == config::Direction::Input)
        {
            entry.input = std::make_unique<InputChannel>(_globalCfg, entry.cfg, sub, _domain, _metrics, idLabel);
            entry.input->start();
        }
        else
        {
            entry.output = std::make_unique<OutputChannel>(_globalCfg, entry.cfg, sub, _domain, _metrics, idLabel);
            entry.output->start();
        }
    }

    void ChannelManager::stopEntry(Entry& entry)
    {
        if (entry.input)
        {
            entry.input->stop();
            entry.input.reset();
        }
        if (entry.output)
        {
            entry.output->stop();
            entry.output.reset();
        }
    }

    void ChannelManager::startAll()
    {
        std::lock_guard const lock{_mutex};
        for (auto& [idx, entry] : _entries)
        {
            if (!entry.input && !entry.output)
            {
                startEntry(entry);
            }
        }
        _started = true;
    }

    void ChannelManager::stopAll()
    {
        std::lock_guard const lock{_mutex};
        for (auto& [idx, entry] : _entries)
        {
            stopEntry(entry);
        }
        _started = false;
    }

    std::optional<std::string> ChannelManager::validateAgainstCard(config::ChannelConfig const& ch) const
    {
        if (static_cast<std::size_t>(ch.subdeviceIndex) >= _card.subDeviceCount())
        {
            return "channel " + std::to_string(ch.index) + ": sub-device index " + std::to_string(ch.subdeviceIndex) +
                   " does not exist on this card (" + std::to_string(_card.subDeviceCount()) + " sub-devices)";
        }
        auto const& sub = const_cast<dl::ICard&>(_card).subDevice(static_cast<std::size_t>(ch.subdeviceIndex));
        if (ch.direction == config::Direction::Input && !sub.info().supportsCapture)
        {
            return "channel " + std::to_string(ch.index) + ": sub-device does not support capture";
        }
        if (ch.direction == config::Direction::Output && !sub.info().supportsPlayback)
        {
            return "channel " + std::to_string(ch.index) + ": sub-device does not support playback";
        }
        if (ch.isAutoMode() && !sub.info().supportsInputFormatDetection)
        {
            return "channel " + std::to_string(ch.index) + ": CHx_VIDEO_MODE=auto requires input format detection support";
        }
        return std::nullopt;
    }

    std::variant<ChannelManager::ApplyResult, std::string> ChannelManager::applyChannels(std::vector<config::ChannelConfig> const& channels)
    {
        std::lock_guard const lock{_mutex};

        for (auto const& ch : channels)
        {
            if (auto const err = validateAgainstCard(ch))
            {
                return *err;
            }
        }

        ApplyResult result;

        // Removed channels.
        for (auto it = _entries.begin(); it != _entries.end();)
        {
            bool present = false;
            for (auto const& ch : channels)
            {
                present = present || ch.index == it->first;
            }
            if (!present)
            {
                log::info("channel_removed", {{"channel_index", it->first}, {"channel_label", it->second.cfg.label}});
                stopEntry(it->second);
                result.removed.push_back(it->first);
                it = _entries.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Added / changed channels.
        for (auto const& ch : channels)
        {
            auto const it = _entries.find(ch.index);
            if (it == _entries.end())
            {
                auto& entry = _entries.emplace(ch.index, Entry{ch, nullptr, nullptr}).first->second;
                if (_started)
                {
                    startEntry(entry);
                }
                log::info("channel_added", {{"channel_index", ch.index}, {"channel_label", ch.label}});
                result.added.push_back(ch.index);
            }
            else if (!(it->second.cfg == ch))
            {
                // §7.5.3: restart only the affected channel.
                log::info("channel_reconfigured", {{"channel_index", ch.index}, {"channel_label", ch.label}});
                stopEntry(it->second);
                it->second.cfg = ch;
                if (_started)
                {
                    startEntry(it->second);
                }
                result.restarted.push_back(ch.index);
            }
        }

        return result;
    }

    void ChannelManager::housekeeping()
    {
        std::lock_guard const lock{_mutex};
        for (auto& [idx, entry] : _entries)
        {
            if (entry.input)
            {
                entry.input->housekeeping();
            }
            if (entry.output)
            {
                entry.output->housekeeping();
            }
        }
    }

    std::vector<ChannelManager::ChannelView> ChannelManager::channels() const
    {
        std::lock_guard const lock{_mutex};
        std::vector<ChannelView> out;
        out.reserve(_entries.size());
        for (auto const& [idx, entry] : _entries)
        {
            ChannelView v;
            v.cfg = entry.cfg;
            Status const* status = nullptr;
            if (entry.input)
            {
                status = &entry.input->status();
            }
            else if (entry.output)
            {
                status = &entry.output->status();
            }
            if (status != nullptr)
            {
                v.state = status->state.load();
                v.signalLocked = status->signalLocked.load();
                v.lastFrameTaiNs = status->lastFrameTaiNs.load();
                v.framesTotal = status->framesTotal.load();
                v.framesDropped = status->framesDropped.load();
                v.reconnects = status->reconnects.load();
                v.grainsCommitted = status->grainsCommitted.load();
                v.activeVideoFlowId = status->activeVideoFlowId();
                v.activeModeName = status->activeModeName();
            }
            out.push_back(std::move(v));
        }
        return out;
    }

    int ChannelManager::healthyCount() const
    {
        int n = 0;
        for (auto const& v : channels())
        {
            if (v.state == State::Healthy)
            {
                ++n;
            }
        }
        return n;
    }

    std::size_t ChannelManager::totalCount() const
    {
        std::lock_guard const lock{_mutex};
        return _entries.size();
    }
}
