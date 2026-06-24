#pragma once

#include <thread>
#include <atomic>
#include "udpsocket.h"
#include "messagequeue.h"

class ReceiveThread
{
public:
    ReceiveThread( UdpSocket& socket, MessageQueue& queue ) :
        socket( socket ),
        queue( queue ),
        running( false )
    {}

    ~ReceiveThread()
    {
        stop();
    }

    void start()
    {
        running = true;
        thread = std::thread( &ReceiveThread::loop, this );
    }

    void stop()
    {
        running = false;

        if ( thread.joinable() )
        {
            thread.join();
        }
    }

private:
    void loop()
    {
        while ( running )
        {
            std::vector<uint8_t> data;
            Address              sender;

            int bytesReceived = socket.receiveFrom( data, sender );

            if ( bytesReceived > 0 )
            {
                Packet packet;
                packet.data   = data;
                packet.sender = sender;

                queue.push( packet );
            }
        }
    }

    UdpSocket&        socket;
    MessageQueue&     queue;
    std::thread       thread;
    std::atomic<bool> running;
};
