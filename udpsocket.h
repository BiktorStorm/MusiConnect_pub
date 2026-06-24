#pragma once

#include <sys/socket.h>   // socket(), bind(), sendto(), recvfrom()
#include <netinet/in.h>   // sockaddr_in, INADDR_ANY
#include <arpa/inet.h>    // inet_pton(), inet_ntop(), htons(), ntohs()
#include <unistd.h>       // close()

#include <string>
#include <vector>
#include <cstdint>
#include "address.h"

class UdpSocket
{
public:
    UdpSocket();
    ~UdpSocket();

    bool bind( uint16_t port );
    bool sendTo( const std::vector<uint8_t>& data, const Address& dest );
    int  receiveFrom( std::vector<uint8_t>& data, Address& sender );

    bool isValid() const { return socketFd != -1; }

private:
    int socketFd;  // on Linux, a socket is just an int (file descriptor)

    sockaddr_in toSockAddr( const Address& addr ) const;
    Address     fromSockAddr( const sockaddr_in& addr ) const;
};
