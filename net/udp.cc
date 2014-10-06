/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "udp.hh"

using namespace net;

namespace net {
namespace udp_v4_impl {

class native_datagram : public udp_datagram_impl {
private:
    const ipv4_address _src;
    const ipv4_address _dst;
    packet _p;
    udp_hdr *_hdr;
public:
    native_datagram(ipv4_address src, ipv4_address dst, packet p)
            : _src(src)
            , _dst(dst)
            , _p(std::move(p)) {
        _hdr = _p.get_header<udp_hdr>();
        ntoh(*_hdr);
        _p.trim_front(sizeof(*_hdr));
    }

    virtual ipv4_addr get_src() override {
        return to_ipv4_addr(_src, _hdr->src_port);
    };

    virtual ipv4_addr get_dst() override {
        return to_ipv4_addr(_dst, _hdr->dst_port);
    };

    virtual uint16_t get_dst_port() override {
        return _hdr->dst_port;
    }

    virtual packet& get_data() override {
        return _p;
    }
};

class native_channel : public udp_channel_impl {
private:
    udp_v4& _proto;
    udp_v4::registration _reg;
    bool _closed;
    shared_ptr<udp_channel_state> _state;
public:
    native_channel(udp_v4 &proto, udp_v4::registration reg, shared_ptr<udp_channel_state> state)
            : _proto(proto)
            , _reg(reg)
            , _closed(false)
            , _state(state)
    {}

    virtual future<udp_datagram> receive() override {
        return _state->_queue.pop_eventually();
    }

    virtual future<> send(ipv4_addr dst, const char* msg) override {
        return _proto.send(_reg.port(), dst, packet::from_static_data(msg, strlen(msg)));
    }

    virtual future<> send(ipv4_addr dst, packet p) override {
        return _proto.send(_reg.port(), dst, std::move(p));
    }

    virtual bool is_closed() const {
        return _closed;
    }

    virtual void close() override {
        _reg.unregister();
        _closed = true;
    }
};

} /* namespace udp_v4_impl */

using namespace net::udp_v4_impl;

const int udp_v4::default_queue_size = 1024;

udp_v4::udp_v4(ipv4& inet)
    : _inet(inet)
{
    _inet.register_l4(protocol_number, this);
}

void udp_v4::received(packet p, ipv4_address from, ipv4_address to)
{
    udp_datagram dgram(std::make_unique<native_datagram>(from, to, std::move(p)));

    auto chan_it = _channels.find(dgram.get_dst_port());
    if (chan_it != _channels.end()) {
        auto chan = chan_it->second;
        chan->_queue.push(std::move(dgram));
    }
}

future<> udp_v4::send(uint16_t src_port, ipv4_addr dst, packet &&p)
{
    auto hdr = p.prepend_header<udp_hdr>();
    hdr->src_port = src_port;
    hdr->dst_port = dst.port;
    hdr->len = p.len();
    hdr->cksum = 0; // TODO: calculate checksum
    hton(*hdr);
    return _inet.send(dst, protocol_number, std::move(p));
}

uint16_t udp_v4::next_port(uint16_t port) {
    return (port + 1) == 0 ? min_anonymous_port : port + 1;
}

udp_channel
udp_v4::make_channel(ipv4_addr addr) {
    if (!is_ip_unspecified(addr)) {
        throw std::runtime_error("Binding to specific IP not supported yet");
    }

    uint16_t bind_port;

    if (!is_port_unspecified(addr)) {
        if (_channels.count(addr.port)) {
            throw std::runtime_error("Address already in use");
        }
        bind_port = addr.port;
    } else {
        auto starting_port = _next_anonymous_port;
        while (_channels.count(_next_anonymous_port)) {
            _next_anonymous_port = next_port(_next_anonymous_port);
            if (starting_port == _next_anonymous_port) {
                throw std::runtime_error("No free port");
            }
        }

        bind_port = _next_anonymous_port;
        _next_anonymous_port = next_port(_next_anonymous_port);
    }

    auto chan_state = make_shared<udp_channel_state>(_queue_size);
    _channels[bind_port] = chan_state;
    return udp_channel(std::make_unique<native_channel>(*this, registration(*this, bind_port), chan_state));
}

} /* namespace net */