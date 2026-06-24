#include "udpsocket.h"
#include "receiverthread.h"
#include "messagequeue.h"
#include <iostream>
#include <string>
#include <chrono>
#include <thread>


int main()
{
    const std::string SERVER_IP   = "172y.25.94.36"; //127.0.0.1 för localhost
    const uint16_t    SERVER_PORT = 22124;
    Address           serverAddr( SERVER_IP, SERVER_PORT );

    // --- setup ---
    UdpSocket    socket;
    MessageQueue queue;

    if ( !socket.isValid() )
    {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    socket.bind( 0 );

    // start the receive thread — from this point on,
    // all incoming packets are automatically pushed to queue
    ReceiveThread receiver( socket, queue );
    receiver.start();

    std::cout << "Receive thread started.\n";

    // --- main loop ---
    std::string input;
    while ( true )
    {
        std::cout << "Enter message (or 'quit'): ";
        std::getline( std::cin, input );

        if ( input == "quit" )
        {
            break;
        }

        // send
        std::vector<uint8_t> data( input.begin(), input.end() );
        socket.sendTo( data, serverAddr );
        std::cout << "Sent. Waiting for echo...\n";

        // instead of blocking on recvfrom(), we poll the queue
        // polling = checking repeatedly in a loop
        // we wait up to 2 seconds for the echo to arrive
        const int MAX_WAIT_MS   = 2000;
        const int POLL_INTERVAL = 10;    // check every 10ms
        int       waited        = 0;
        bool      received      = false;

        while ( waited < MAX_WAIT_MS )
        {
            Packet packet;
            if ( queue.pop( packet ) )
            {
                // got something from the queue
                std::string reply( packet.data.begin(), packet.data.end() );
                std::cout << "Echo received from "
                          << packet.sender.ip << ":"
                          << packet.sender.port
                          << " — \"" << reply << "\"\n";
                received = true;
                break;
            }

            // nothing in queue yet, wait 10ms and try again
            std::this_thread::sleep_for( std::chrono::milliseconds( POLL_INTERVAL ) );
            waited += POLL_INTERVAL;
        }

        if ( !received )
        {
            std::cout << "No echo received after " << MAX_WAIT_MS << "ms\n";
        }
    }

    // stop() is also called automatically by ReceiveThread's destructor
    // but calling it explicitly here makes the intent clear
    receiver.stop();

    return 0;
}
