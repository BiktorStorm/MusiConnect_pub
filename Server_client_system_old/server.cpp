#include "udpsocket.h"
#include <iostream>
#include <string>

int main()
{
    const uint16_t SERVER_PORT = 22124;  // <-- must be 22124, not 0

    UdpSocket socket;
    if ( !socket.isValid() )
    {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    if ( !socket.bind( SERVER_PORT ) )
    {
        std::cerr << "Failed to bind\n";
        return 1;
    }

    std::cout << "Echo server listening on port " << SERVER_PORT << "\n";

    while ( true )
    {
        std::vector<uint8_t> data;
        Address              sender;

        int bytesReceived = socket.receiveFrom( data, sender );

        if ( bytesReceived <= 0 )
        {
            continue;
        }

        std::string message( data.begin(), data.end() );
        std::cout << "Received from " << sender.ip << ":" << sender.port
                  << " — \"" << message << "\"\n";

        socket.sendTo( data, sender );

        std::cout << "Echoed back.\n";
    }

    return 0;
}
