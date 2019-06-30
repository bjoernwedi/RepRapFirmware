/*
 * Socket.cpp
 *
 *  Created on: 25 Dec 2016
 *      Author: David
 
 * SD:: Copied from W5500 and edited for FreeRTOS+TCP
 
 
 */

#include "RTOSPlusTCPEthernetSocket.h"
#include "Network.h"
#include "Networking/NetworkInterface.h"

#include "NetworkBuffer.h"
#include "RepRap.h"

#include "FreeRTOS_IP_Private.h"

#include "RTOSPlusTCPEthernetServerSocket.h"
#include "General/IP4String.h"


//***************************************************************************************************
// Socket class

const unsigned int MaxBuffersPerSocket = 1;
const unsigned int SocketShutdownTimeoutMillis = 50;//how long to wait before we force close the socket after last send

//function to check socket: (requires FreeRTOS_IP_Private.h)
bool isSocketClosing(Socket_t xSocket)
{
    FreeRTOS_Socket_t *pxSocket = ( FreeRTOS_Socket_t * ) xSocket;
    
    if(xSocket == nullptr) return true;

    switch( pxSocket->u.xTCP.ucTCPState )
    {
        //closing states
        case eCLOSING:
        case eTIME_WAIT:
        case eFIN_WAIT_1:
        case eFIN_WAIT_2:
        case eLAST_ACK:
            return true;
            break;

        default:
            break;
    }
    return false;
}

bool hasSocketGracefullyClosed(Socket_t xSocket)
{
    FreeRTOS_Socket_t *pxSocket = ( FreeRTOS_Socket_t * ) xSocket;
    
    if(xSocket == nullptr) return true;
    
    if( pxSocket->u.xTCP.ucTCPState == eCLOSED || pxSocket->u.xTCP.ucTCPState == eCLOSE_WAIT)
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool isSocketListening(Socket_t xSocket){
    FreeRTOS_Socket_t *pxSocket = ( FreeRTOS_Socket_t * ) xSocket;
    
    if(xSocket == nullptr) return false;

    if( pxSocket->u.xTCP.ucTCPState == eTCP_LISTEN )
    {
        return true;
    }
    else
    {
        return false;
    }

}


bool isSocketEstablished(Socket_t xSocket)
{
    return FreeRTOS_issocketconnected(xSocket);
    
}

bool isSocketEstablishing(Socket_t xSocket)
{
 
    FreeRTOS_Socket_t *pxSocket = ( FreeRTOS_Socket_t * ) xSocket;
    
    if( pxSocket->u.xTCP.ucTCPState == eSYN_FIRST || pxSocket->u.xTCP.ucTCPState == eSYN_RECEIVED )
    {
        return true;
    }
    else
    {
        return false;
    }

}




RTOSPlusTCPEthernetSocket::RTOSPlusTCPEthernetSocket(NetworkInterface *iface)
	: Socket(iface), receivedData(nullptr)
{
    xConnectedSocket = nullptr;
    xListeningSocket = nullptr;
}

// Initialise a TCP socket
void RTOSPlusTCPEthernetSocket::Init(SocketNumber skt, Port serverPort, NetworkProtocol p)
{
    socketNum = skt;
	localPort = serverPort;
	protocol = p;
    ReInit();
}

void RTOSPlusTCPEthernetSocket::TerminateAndDisable()
{
	MutexLocker lock(interface->interfaceMutex);

	Terminate(); // terminate client
	state = SocketState::disabled;
}

void RTOSPlusTCPEthernetSocket::ReInit()
{
	MutexLocker lock(interface->interfaceMutex);

    // Discard any received data
	while (receivedData != nullptr)
	{
		receivedData = receivedData->Release();
	}

    xConnectedSocket = nullptr;
    state = SocketState::inactive;

    //create server socket, or check if it was closed for some reason and attempt reopen
    xListeningSocket = RTOSPlusTCPEthernetServerSocket::Instance()->GetServerSocket(localPort, protocol);
    if( xListeningSocket != nullptr )
    {
        state = SocketState::listening;
    }
    else
    {
        //failed to create socket!!!
        if (reprap.Debug(moduleNetwork))
        {
            debugPrintf("Failed to create Server Socket. Disabling Socket\n");
        }
    }
}

// Close a connection when the last packet has been sent
void RTOSPlusTCPEthernetSocket::Close()
{
	MutexLocker lock(interface->interfaceMutex);
    
	if (state != SocketState::disabled && state != SocketState::inactive)
	{
        DiscardReceivedData();
        if (protocol == FtpDataProtocol)
        {
            localPort = 0;                    // don't re-listen automatically
        }
        
        //server initiated Close:
        //Send Shutdown request and gracefully shutdown
        FreeRTOS_shutdown( xConnectedSocket, FREERTOS_SHUT_RDWR );
        state = SocketState::closing;
	}
}

// Terminate a connection immediately
void RTOSPlusTCPEthernetSocket::Terminate()
{
	MutexLocker lock(interface->interfaceMutex);
	if (state != SocketState::disabled)
	{
        DiscardReceivedData();

        FreeRTOS_shutdown( xConnectedSocket, FREERTOS_SHUT_RDWR );
        FreeRTOS_closesocket( xConnectedSocket );
        xConnectedSocket = nullptr;
        state = SocketState::inactive;
	}
}

// Return true if there is or may soon be more data to read
bool RTOSPlusTCPEthernetSocket::CanRead() const
{
	return (state == SocketState::connected) ||
		(state == SocketState::clientDisconnecting && receivedData != nullptr && receivedData->TotalRemaining() != 0);
}

bool RTOSPlusTCPEthernetSocket::CanSend() const
{
	return state == SocketState::connected;
}

// Read 1 character from the receive buffers, returning true if successful
bool RTOSPlusTCPEthernetSocket::ReadChar(char& c)
{
	if (receivedData != nullptr)
	{
		MutexLocker lock(interface->interfaceMutex);

		const bool ret = receivedData->ReadChar(c);
		if (receivedData->IsEmpty())
		{
			receivedData = receivedData->Release();
		}
		return ret;
	}
    
    c = 0;
	return false;
}

// Return a pointer to data in a buffer and a length available
bool RTOSPlusTCPEthernetSocket::ReadBuffer(const uint8_t *&buffer, size_t &len)
{
    if (receivedData != nullptr)
	{
		len = receivedData->Remaining();
		buffer = receivedData->UnreadData();
		return true;
	}
	return false;
}

// Flag some data as taken from the receive buffers. We never take data from more than one buffer at a time.
void RTOSPlusTCPEthernetSocket::Taken(size_t len)
{
	if (receivedData != nullptr)
	{
		receivedData->Taken(len);
		if (receivedData->IsEmpty())
		{
			receivedData = receivedData->Release();		// discard empty buffer at head of chain
		}
	}
}

// Poll a socket to see if it needs to be serviced
void RTOSPlusTCPEthernetSocket::Poll(bool full)
{
	if (state != SocketState::disabled)
	{
		MutexLocker lock(interface->interfaceMutex);

        switch (state){
                
            case SocketState::inactive:
                ReInit();
                break;
                
            case SocketState::listening:
            {
                //check for client connections
                
                if(xConnectedSocket == nullptr)
                {
                    struct freertos_sockaddr xClient;
                    socklen_t xSize = sizeof( xClient );
                    
                    //socket is setup in non-blocking mode, so we will poll the accept for next client in the queue
                    //if there is no client, FreeRTOS_accept will return false.
                    xConnectedSocket = FreeRTOS_accept( xListeningSocket, &xClient, &xSize );
                    
                    if(xConnectedSocket == FREERTOS_INVALID_SOCKET) //server socket not valid or in not the listening state
                    {
                        ReInit();
                    }
                    else if(xConnectedSocket == nullptr)
                    {
                        //timed out waiting
                        // no clients waiting to connect
                        // nothing else to do
                        return;

                    }
                    else
                    {
                        whenConnected = millis();
                        
                        //Set the Remote Port and IP Address
                        remotePort = xClient.sin_port;
                        remoteIPAddress.SetV4LittleEndian(xClient.sin_addr);

                        if (reprap.Debug(moduleNetwork))
                        {
                            debugPrintf("Accepted Connection from %s:%d on Skt: %d\n", IP4String(remoteIPAddress).c_str(), remotePort, socketNum );
                        }
                    }
                    
                }
                else
                {
                    //we previously accepted a client, but were waiting for a responder
                    //dont attempt to accept any new connections
                    
                    //check socket to see if its closed before we got to initialise everything
                    if(FreeRTOS_issocketconnected(xConnectedSocket) != pdTRUE)
                    {
                        state = SocketState::closing;
                        if (reprap.Debug(moduleNetwork))
                        {
                            debugPrintf("Socket closed while setting up on Skt: %d\n", socketNum);
                        }
                    }
                }
                
                //check for a responder
                if(FreeRTOS_issocketconnected(xConnectedSocket) == pdTRUE && state == SocketState::listening)
                {
                    if (reprap.GetNetwork().FindResponder(this, protocol))
                    {
                        state = SocketState::connected;
                    }
                    else if (millis() - whenConnected >= FindResponderTimeout)
                    {
                        if (reprap.Debug(moduleNetwork))
                        {
                            debugPrintf("Timed out waiting for resonder on skt %d for port %u\n", socketNum, localPort);
                        }
                        Close(); // close the socket
                    }
                    else
                    {
                        //waiting for a responder, but not yet timed out
                    }
                }
            } break;

            case SocketState::connected:
            {
                
                if( isSocketEstablished(xConnectedSocket) == true)
                {
                    // See if the socket has received any data
                    ReceiveData();
                }
                else if( isSocketEstablishing(xConnectedSocket) == true) // socket is still establishing, wait until established
                {
                    //nothing to do
                }
                else if( isSocketClosing(xConnectedSocket) )
                {
                    FreeRTOS_debug_printf( ( "Socket is closing down\n" ) );
                    
                    ReceiveData(); //last check of buffers
                    state = SocketState::clientDisconnecting;
                }
                else
                {
                    if (reprap.Debug(moduleNetwork))
                    {
                        debugPrintf("Unknown socket state on Skt: %d\n", socketNum);
                    }

                    //test the socket to see if its really in an error state
                    uint8_t tmp;
                    BaseType_t ret = FreeRTOS_recv( xConnectedSocket, &tmp, 1, 0 );
                    if( ret < 0 )
                    {
                        CheckSocketError(ret);
                        Close(); //close the socket
                    }
                }
            } break;
                
            case SocketState::clientDisconnecting:
            {
                //client disconnecting (planned) - close gracefully after we have finished receiving from buffers
                //check for any remaining data in buffers
                BaseType_t rxlen = FreeRTOS_recvcount( xConnectedSocket ); //bytes not received from buffers
                if( rxlen > 0 ){
                    ReceiveData();
                }
                else
                {
                    if (protocol == FtpDataProtocol)
                    {
                        localPort = 0;                    // don't re-listen automatically
                    }
                    
                    //Send Shutdown request and gracefully shutdown
                    FreeRTOS_shutdown( xConnectedSocket, FREERTOS_SHUT_RDWR );
                    state = SocketState::closing;
                }
            } break;
                
            case SocketState::closing:
                whenConnected = millis(); // reuse whenConnected to have a shutdown timeout
                state = SocketState::closing2;
                //fall through to closing2:
            case SocketState::closing2:
            {
                /* Wait for the socket to send remaining packets and disconnect gracefully (indicated by FreeRTOS_recv()
                returning a FREERTOS_EINVAL error) before closing the socket. */
                uint8_t tmp;
                if( FreeRTOS_recv( xConnectedSocket, &tmp, 1, 0 ) < 0 ||  (millis() - whenConnected) >= SocketShutdownTimeoutMillis)
                {
                    if (reprap.Debug(moduleNetwork))
                    {
                        debugPrintf("Waited %ld ms for Graceful Shutdown\n", (millis() - whenConnected));
                    }

                    FreeRTOS_closesocket( xConnectedSocket ); //close the socket
                    xConnectedSocket = nullptr;
                    ReInit();
                }
            } break;

            default:
                break;

        } //end switch
    }
}



//returns true if the socket was in Error
bool RTOSPlusTCPEthernetSocket::CheckSocketError(BaseType_t val)
{
    //If there was not enough memory for the socket to be able to create either an Rx or Tx stream then -pdFREERTOS_ERRNO_ENOMEM is returned.
    //If the socket was closed or got closed then -pdFREERTOS_ERRNO_ENOTCONN is returned.
    //If the socket received a signal, causing the read operation to be aborted, then -pdFREERTOS_ERRNO_EINTR is returned.
    //If the socket is not valid, is not a TCP socket, or is not bound then -pdFREERTOS_ERRNO_EINVAL is returned;

    if( ( val < 0 ) && ( val != -pdFREERTOS_ERRNO_EWOULDBLOCK ) )
    {
        if (reprap.Debug(moduleNetwork))
        {
            debugPrintf("SocketError on Skt: %d Closing Connection\n", socketNum);

            if(val == -pdFREERTOS_ERRNO_ENOMEM)     debugPrintf("  - Not enough memory for Socket\n");
            else if(val == -pdFREERTOS_ERRNO_ENOTCONN)   debugPrintf("  - Socket was closed or got closed\n");
            else if(val == -pdFREERTOS_ERRNO_EINTR)      debugPrintf("  - Read operation was aborted\n");
            else if(val == -pdFREERTOS_ERRNO_EINVAL)     debugPrintf("  - Socket not valid\n");
            else debugPrintf("  - Other socket error code: %d", (int)val);
        }

        Terminate();
        
        return true;
    }
    
    return false;
}


// Try to receive more incoming data from the socket. The mutex is alrady owned.
void RTOSPlusTCPEthernetSocket::ReceiveData()
{
    BaseType_t len = FreeRTOS_recvcount( xConnectedSocket );
    
	if (len > 0)
	{
		NetworkBuffer * const lastBuffer = NetworkBuffer::FindLast(receivedData);

        if (lastBuffer != nullptr && lastBuffer->SpaceLeft() >= (uint16_t) len)
		{
            BaseType_t bReceived = FreeRTOS_recv( xConnectedSocket, lastBuffer->UnwrittenData(),lastBuffer->SpaceLeft(), 0 );// socket, pointer to buffer, how big is the buffer, flags
            
            if( bReceived > 0)
            {
                lastBuffer->dataLength += bReceived;
                if (reprap.Debug(moduleNetwork))
                {
                    debugPrintf("Appended %u bytes on skt: %d\n", (unsigned int)bReceived, socketNum);
                }
            }
            else
            {
                CheckSocketError(bReceived);
            }
		}
		else if (NetworkBuffer::Count(receivedData) < MaxBuffersPerSocket)
		{
            NetworkBuffer * const buf = NetworkBuffer::Allocate();
            if (buf != nullptr)
            {
                BaseType_t bReceived = FreeRTOS_recv( xConnectedSocket, buf->Data(),buf->SpaceLeft(), 0 );// socket, pointer to buffer, how big is the buffer, flags
                
                if( bReceived > 0){
                    buf->dataLength = (size_t)bReceived;
                    NetworkBuffer::AppendToList(&receivedData, buf);
                    if (reprap.Debug(moduleNetwork))
                    {
                        //debugPrintf("Received %u bytes on skt: %d\n", (unsigned int)bReceived, socketNum);
                    }
                }
                else
                {
                    buf->Release();// release the buffer we allocated above but did not use
                    CheckSocketError(bReceived);
                }
            }
		}
		else
        {
            if (reprap.Debug(moduleNetwork))
            {
                debugPrintf("No buffer available to receive data on skt:%d\n", socketNum);
            }
        }
	}
}

// Discard any received data for this transaction. The mutex is already owned.
void RTOSPlusTCPEthernetSocket::DiscardReceivedData()
{
	while (receivedData != nullptr)
	{
		receivedData = receivedData->Release();
	}
}

// Send the data, returning the length buffered
size_t RTOSPlusTCPEthernetSocket::Send(const uint8_t *data, size_t length)
{
	MutexLocker lock(interface->interfaceMutex);
    
	if (CanSend() && length != 0 )
	{
		// Check for previous send complete
		if (FreeRTOS_outstanding(xConnectedSocket) > 0)									// are we already sending?
        {
            return 0;
        }
        
        BaseType_t canWriteBytes = FreeRTOS_maywrite(xConnectedSocket);
        if(canWriteBytes < 0)
        {
            //Error on Socket
            if (reprap.Debug(moduleNetwork))
            {
                debugPrintf("Send error on Skt: %d\n", socketNum);
            }
            CheckSocketError(canWriteBytes);
            
            return 0;
        }
        
        //fill up the remaining TX buffer
        if (length >  (uint16_t) canWriteBytes)
        {
            length = (uint16_t) canWriteBytes;
        }
        
        BaseType_t ret = FreeRTOS_send( xConnectedSocket, data, length, 0 );
        if( (ret < 0) && ( ret != -pdFREERTOS_ERRNO_EWOULDBLOCK ))
        {
            //Error on socket
            if (reprap.Debug(moduleNetwork))
            {
                debugPrintf("Send error on Skt: %d Err Code: %d\n", socketNum,(int16_t )ret );
            }
            CheckSocketError(ret);
            return 0;
        }
        else
        {
            if (reprap.Debug(moduleNetwork))
            {
                debugPrintf("Queued %d bytes (of %d bytes) on Skt: %d\n", (int16_t )ret, length, socketNum );
            }
        }
        
        length = (size_t) ret; //ret holds how much data we actually were able to send
    
		return length;
	}
	return 0;
}

// wait for the interface to send the outstanding data
void RTOSPlusTCPEthernetSocket::Send()
{
}


// End
