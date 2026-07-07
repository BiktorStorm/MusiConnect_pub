#include "udpsocket.h"
#include <iostream>
#include <cstring>

UdpSocket::UdpSocket()
{
    // AF_INET    = IPv4
    // SOCK_DGRAM = UDP
    // 0          = OS picks protocol (UDP for SOCK_DGRAM)
    socketFd = socket( AF_INET, SOCK_DGRAM, 0 );

    if ( socketFd == -1 )
    {
        std::cerr << "Failed to create socket\n";
    }
}

UdpSocket::~UdpSocket()
{
    if ( socketFd != -1 )
    {
        close( socketFd );
    }
}

bool UdpSocket::bind( uint16_t port )
{
    sockaddr_in localAddr;
    memset( &localAddr, 0, sizeof( localAddr ) );

    localAddr.sin_family      = AF_INET;
    localAddr.sin_port        = htons( port );
    localAddr.sin_addr.s_addr = INADDR_ANY;

    if ( ::bind( socketFd, (sockaddr*) &localAddr, sizeof( localAddr ) ) == -1 )
    {
        std::cerr << "bind() failed on port " << port << "\n";
        return false;
    }

    std::cout << "Socket bound to port " << port << "\n";
    return true;
}

bool UdpSocket::sendTo( const std::vector<uint8_t>& data, const Address& dest )
{
    sockaddr_in destAddr = toSockAddr( dest );

    int bytesSent = sendto( socketFd,
                            data.data(),
                            data.size(),
                            0,
                            (sockaddr*) &destAddr,
                            sizeof( destAddr ) );

    if ( bytesSent == -1 )
    {
        std::cerr << "sendto() failed\n";
        return false;
    }

    return true;
}

int UdpSocket::receiveFrom( std::vector<uint8_t>& data, Address& sender )
{
    const int  MAX_UDP_SIZE = 65535;
    sockaddr_in senderAddr;
    socklen_t   senderAddrLen = sizeof( senderAddr );

    data.resize( MAX_UDP_SIZE );

    int bytesReceived = recvfrom( socketFd,
                                  data.data(),
                                  MAX_UDP_SIZE,
                                  0,
                                  (sockaddr*) &senderAddr,
                                  &senderAddrLen );

    if ( bytesReceived == -1 )
    {
        std::cerr << "recvfrom() failed\n";
        return -1;
    }

    data.resize( bytesReceived );
    sender = fromSockAddr( senderAddr );

    return bytesReceived;
}

sockaddr_in UdpSocket::toSockAddr( const Address& addr ) const
{
    sockaddr_in sa;
    memset( &sa, 0, sizeof( sa ) );

    sa.sin_family = AF_INET;
    sa.sin_port   = htons( addr.port );
    inet_pton( AF_INET, addr.ip.c_str(), &sa.sin_addr );

    return sa;
}

Address UdpSocket::fromSockAddr( const sockaddr_in& sa ) const
{
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop( AF_INET, &sa.sin_addr, ipStr, sizeof( ipStr ) );
    return Address( ipStr, ntohs( sa.sin_port ) );
}
