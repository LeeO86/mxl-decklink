// SPDX-License-Identifier: MIT
#include "channel_manager.hpp"

#include <cstdio>

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
    {
        auto const idLabel = cardIdLabel(card);
        for (auto const& ch : cfg.channels)
        {
            auto& sub = card.subDevice(static_cast<std::size_t>(ch.subdeviceIndex));
            if (ch.direction == config::Direction::Input)
            {
                _inputs.push_back(std::make_unique<InputChannel>(cfg, ch, sub, domain, metrics, idLabel));
            }
            else
            {
                _outputs.push_back(std::make_unique<OutputChannel>(cfg, ch, sub, domain, metrics, idLabel));
            }
        }
    }

    void ChannelManager::startAll()
    {
        for (auto& ch : _inputs)
        {
            ch->start();
        }
        for (auto& ch : _outputs)
        {
            ch->start();
        }
    }

    void ChannelManager::stopAll()
    {
        for (auto& ch : _inputs)
        {
            ch->stop();
        }
        for (auto& ch : _outputs)
        {
            ch->stop();
        }
    }

    void ChannelManager::housekeeping()
    {
        for (auto& ch : _inputs)
        {
            ch->housekeeping();
        }
        for (auto& ch : _outputs)
        {
            ch->housekeeping();
        }
    }

    std::vector<ChannelManager::ChannelView> ChannelManager::channels() const
    {
        std::vector<ChannelView> out;
        out.reserve(_inputs.size() + _outputs.size());
        for (auto const& ch : _inputs)
        {
            out.push_back({&ch->channelConfig(), &ch->status()});
        }
        for (auto const& ch : _outputs)
        {
            out.push_back({&ch->channelConfig(), &ch->status()});
        }
        return out;
    }

    int ChannelManager::healthyCount() const
    {
        int n = 0;
        for (auto const& v : channels())
        {
            if (v.status->state.load() == State::Healthy)
            {
                ++n;
            }
        }
        return n;
    }

    std::size_t ChannelManager::totalCount() const
    {
        return _inputs.size() + _outputs.size();
    }
}
