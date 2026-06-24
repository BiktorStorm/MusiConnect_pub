#pragma once
#include <string>
#include <cstdint>

struct Address
{
    std::string ip;
    uint16_t    port;

    Address() : ip( "" ), port( 0 ) {}
    Address( const std::string& ip, uint16_t port ) : ip( ip ), port( port ) {}

    bool isEmpty() const { return ip.empty() || port == 0; }
};
