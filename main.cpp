#include <unistd.h>
#include <cstring>
#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>
#include <thread>
#include <sys/ioctl.h>
#include <signal.h>
#include "config_file.h"
#include "netsock.h"
#include "netutil.h"
#include "history.h"

using std::string;
using std::map;
using std::cerr;
using std::cout;


// Messages need to fit into a UDP packet
const int MAX_MESSAGE_LEN = 65000;

// This maps a string to an integer
typedef std::map<string,int> MapStrInt;

// These values come from our configuration file
struct
{
    int32_t     serverPort;
    MapStrInt   serviceMap;
    int32_t     udpPort;
} config;

// Non-configuration global variables
struct 
{
    NetSock     server;
    string      configFile="tcp_conduit.conf";
    int         serverResetPipe[2];
    char        tcpBuffer[MAX_MESSAGE_LEN+1];
    char        udpBuffer[66000];
    addrinfo_t  addrinfo; 
    int         senderfd;
    bool        tcpConnected;
} g;

void resetSocketThread(int port, int fd);
void drain(int fd);
void readMessages(int pipefd);
void sendMessageToService();
void udpServerThread(int port);

//=============================================================================
// Reads the command line options. 
// Sets global variables:
//   g.configFile
//=============================================================================
void readOptions(const char** argv)
{
    // Skip over the executable name
    ++argv;

    // Loop through the tokens on the command line...
    while (*argv)
    {
        const char* p = *argv++;
        
        // If this option begins with "--", reduce that to a single "-"
        if (p[0] == '-' && p[1] == '-') ++p;
        
        // Fetch the command line option as a string
        string option = p;

        // Is the user giving us the name of a config file?
        if (option == "-config" && *argv)
        {
            g.configFile = *argv++;
            continue;
        }

        // If we get here, its an invalid command line
        cerr << "Invalid option: " + option + "\n";
        exit(1);
    }
}
//=============================================================================


//=============================================================================
// Read the configuration file 
//
// Passed: name of the config file
//
// On exit: all the fields in the global "config" structure are filled in
//=============================================================================
void readConfigFile(string filename)
{
    CConfigFile c;
    CConfigScript script;

    // Read the config file and complain if we can't
    if (!c.read(filename, false))
    {
        cerr << "Not found: " + g.configFile + "\n";
        exit(1);
    }

    try
    {
        // Fetch the TCP port we listen on
        c.get("server_port", &config.serverPort);

        // Fetch the map of service names to port numbers
        c.get("services", &script);

        // Fetch the port number that the UDP server will listen on
        c.get("udp_port", &config.udpPort);
        
        // Read the map of service-names to port-numbers
        while (script.get_next_line())
        {
            string name = script.get_next_token();
            int    port = script.get_next_int();
            config.serviceMap[name] = port;
        }
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        exit(1);
    }
}
//=============================================================================




//=============================================================================
// Execution begins here
//=============================================================================
int main(int argc, const char** argv)
{
    // Tell the world who (and what version) we are!
    cout << "tcp_conduit v" << SW_VERSION << "\n";

    // We want to ensure that writing to a closed pipe doesn't kill us!
    signal(SIGPIPE, SIG_IGN);

    // Read the command-line options
    readOptions(argv);

    // Read the configuration file
    readConfigFile(g.configFile);

    // Create a pipe between our thread and the "resetSocketThread"
    pipe(g.serverResetPipe);

    // Fetch the file descriptor of the read-side of the pipe
    int pipefd = g.serverResetPipe[0];

    // Create the addrinfo structure for sending UDP messages
    if (!NetUtil::get_server_addrinfo(SOCK_DGRAM, "localhost", 1, AF_INET, &g.addrinfo))
    {
        cerr << "Failed to create addrinfo structure\n";
        exit(1);        
    }

    // Create the UDP sender socket
    g.senderfd = socket(g.addrinfo.family, g.addrinfo.socktype, g.addrinfo.protocol);

    // If that failed, tell the caller
    if (g.senderfd < 0)
    {   
        cerr << "Failed to create UDP socket\n";
        exit(1);
    }


    // Launch the "resetSocketThread".   That thread will inform us (via the
    // pipe) whenever someone connects to the server it's running.   When this
    // happens, it's a sign that we should close our own server.
    std::thread th1(
                    resetSocketThread,
                    config.serverPort + 1,
                    g.serverResetPipe[1]
                );
    th1.detach();

    // Launch the UDP server thread
    std::thread th2(udpServerThread, config.udpPort);
    th2.detach();

    // Sit in a loop reading messages, re-opening the server socket as neccessary
    while (true)
    {
        // Throw away any data in the pipe
        drain(pipefd);

        if (!g.server.create_server(config.serverPort))
        {
            cerr << "Failed to create server on port " << config.serverPort << "\n";
            exit(1);
        }

        // Wait for a client to connect to our server
        g.server.listen_and_accept();

        // Let the UDP server know that we're connected
        g.tcpConnected = true;

        // Indicate to an engineer that we have a connection
        cout << "Connection from " << g.server.get_peer_address() << "\n";

        // Read and handle messages
        readMessages(pipefd);

        // Tell the UDP server that we're no longer connected
        g.tcpConnected = false;

        // Close the server.  We'll re-open it at the top of the loop
        g.server.close();

        // Display a debugging message
        cout << "Server closed, listening for connection\n";
    }

};
//=============================================================================



//=============================================================================
// drains the data from a file descriptor
//=============================================================================
void drain(int fd)
{
    int length = 0;
    char buffer[256];

    // Loop, reading from the file descriptor until there is
    // no more data available to read
    while (true)
    {
        ioctl(fd, FIONREAD, &length);
        if (length < 1) break;
        if (length > sizeof(buffer)) length = sizeof(buffer);
        read(fd, buffer, length);
    }
   
}
//=============================================================================




//=============================================================================
// This thread writes a byte to file-descriptor 'fd' anytime a client connects
// to the TCP server that we're running.  This provides a convenient method for
// the outside world to notify us "Hey, reset your main server socket so I can 
// connect to it!"
//=============================================================================
void resetSocketThread(int port, int pipefd)
{
    NetSock socket;
    char    zero = 0;

    while (true)
    {
        // Create a socket
        if (!socket.create_server(port))
        {
            cerr << "Can't create TCP socket on port " << port << "\n";
            exit(1);
        }

        // Wait for a connection on the second
        socket.listen_and_accept();

        // Close the socket immediatly
        socket.close();

        // Write a byte to file-descriptor "pipefd" to notify the 
        // other thread that it should close its server socket
        // and re-open it.
        write(pipefd, &zero, 1);
    }
}
//=============================================================================


//=============================================================================
// This waits for data to be available from either the server or the pipe. It
// will also detect is the server was closed by the peer
//
// Returns: -1 = We need to close and re-open the server
//          >0 = The number of bytes available for reading from the server
//
// This routine will never return 0
//=============================================================================
int serverBytesAvailable(int pipefd)
{
    fd_set  rfds;
    
    // Find out how many bytes available on the socket
    int bytesAvailable = g.server.bytes_available();

    // If for any reason that's invalid, return -1
    if (bytesAvailable < 0) return -1;

    // If there is data available to read, tell the caller
    if (bytesAvailable > 0) return bytesAvailable;

    // Fetch the socket descriptor;
    int sd = g.server.sd();

    // Clear our file descriptor set
    FD_ZERO(&rfds);

    // We're going to wait on data from these two descriptors;
    FD_SET(sd,     &rfds);
    FD_SET(pipefd, &rfds);

    // Figure out what the highest descriptor is
    int maxfd = (sd > pipefd) ? sd : pipefd;

    // Wait for one of the descriptors to become available for reading
    if (select(maxfd+1, &rfds, NULL, NULL, NULL) < 1) return -1;

    // If there's data waiting in the pipe, then the calling thread is
    // going to bail on whatever it's doing, close the server, and reopen it.
    if (FD_ISSET(pipefd, &rfds)) return -1;

    // This shouldn't be possible!
    if (!FD_ISSET(sd, &rfds)) return -1;

    // Find out how many bytes available for reading from the socket
    bytesAvailable = g.server.bytes_available();

    // If there's not at least one byte available to read, it means
    // the socket was closed by the peer
    if (bytesAvailable < 1) return -1;

    // Tell the caller how much data is available to read
    return bytesAvailable;
}
//=============================================================================


//=============================================================================
// Find out how many bytes to fetch for the message that follows.
//
// We expect the client to send us between 1 and 5 digits, followed by a
// linefeed
// 
// Return -1 if the server should be closed and reopened
//=============================================================================
int fetchMessageLength(int pipefd)
{
    char buffer[6];
    char c, *ptr, *origin;
    
    // Fetch the socket descriptor
    int sd = g.server.sd();

    // Get a byte pointer to the caller's buffer
    origin = ptr = buffer;

    // We're going to leave room to place a nul at the end of the input
    uint32_t buffSize = sizeof(buffer) - 1;

    // Loop until either an error or until we see a space
    while (true)
    {
        // Check to see if there is a byte available
        int available = serverBytesAvailable(pipefd);
        if (available < 1) return -1;

        // Fetch a single byte from the socket
        if (recv(sd, &c, 1, 0) < 1) return -1;

        // Always ignore leading linefeeds and spaces
        if (ptr == origin)
        {
            if (c == '\n') continue;
            if (c == ' ' ) continue;
        }

        // Ignore carriage returns just in case this is a user using telnet
        if (c == 13) continue;

        // We are expecting only digits
        if (c >= '0' && c <= '9')
        {
            if ((ptr - origin) < buffSize) *ptr++ = c;
            continue;            
        }

        // On linefeed, we're done accepting characters
        if (c == '\n') break;

        // If we get here, the peer sent us something besides digits!
        return -1;
    }

    //  Terminate the output string
    *ptr = 0;

    // Decode the ASCII digits to a binary number
    int value = atoi(buffer);

    // This should never happen unless the client sends us "0" as a value!
    if (value < 1) return -1;

    // We are never going to support messages larger than 65000 bytes because
    // we want the messages to fit inside of a UDP packet
    if (value > MAX_MESSAGE_LEN) return -1;

    // Tell the caller how many bytes are in the message to follow
    return value;
}
//=============================================================================


//=============================================================================
// reads message from the server and handles them
//=============================================================================
void readMessages(int pipefd)
{
    // Fetch the socket descriptor of the server
    int sd = g.server.sd();

    while (true)
    {
        // Find out how many bytes to fetch into our message buffer
        int bytesRemaining = fetchMessageLength(pipefd);

        // If this is less than 1, time to close the server and re-open it
        if (bytesRemaining < 1) return;

        // Nul-terminate the message that is arriving into tcpBuffer
        g.tcpBuffer[bytesRemaining] = 0;

        // Point to the message buffer
        char* ptr = g.tcpBuffer;

        // Fetch the incoming message
        while (bytesRemaining)
        {
            // If we need to close the server, we're done
            if (serverBytesAvailable(pipefd) < 1) return;

            // Fetch data from the server to the tcpBuffer
            int bytesFetched = recv(sd, ptr, bytesRemaining, 0);

            // If there was no data fetched, close the server and re-open it
            if (bytesFetched < 1) return;

            // Bump the pointer for the next pass
            ptr += bytesFetched;

            // Figure out how many bytes remain to be fetched
            bytesRemaining -= bytesFetched;
        }

        // Send this message to the appropriate service
        sendMessageToService();
    }
}
//=============================================================================


//=============================================================================
// Sends the message out the correct UDP port
//=============================================================================
void sendMessageToService()
{
    // Is there a space in the message buffer?
    char* p = strchr(g.tcpBuffer, ' ');

    // If there's not, then this message is malformed
    if (p == nullptr)
    {
        cerr << "Malformed message: " << g.tcpBuffer << "\n";  
        return;
    }

    // Stick nul there to terminate the service name
    *p = 0;

    // Now go find the start of the JSON 
    while (*++p == ' ');

    // p had better now point to a JSON object!
    if (*p != '{')
    {
        cerr << "Malformed message: " << p << "\n";  
        return;
    }

    // Go see if this is a service name we recognize
    auto it = config.serviceMap.find(g.tcpBuffer);
    
    // If it's not a service name we recognize, we're done
    if (it == config.serviceMap.end())
    {
        cerr << "Unknown service name: " << g.tcpBuffer << "\n";
        return;
    }

    // Fetch the port that this service is listening on
    int port = it->second;

    // Tell our addrinfo structure what port to send the message to
    g.addrinfo.set_port(port);

    // And send the JSON message to the service that wants it
    sendto(g.senderfd, p, strlen(p), 0, g.addrinfo, g.addrinfo.addrlen);
}
//=============================================================================


//=============================================================================
// makeServer() - Creates a UDP server on the specified port
//=============================================================================
static int makeServer(int port)
{
    uint32_t optval = 1;

    // Create the socket and complain if we can't
    int sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd < 0)
    {
        std::cerr << "Failed while creating UDP socket\n";
        exit(1);        
    }

    // Ensure the socket can be reused
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(optval));

    // Build the address structure of the UDP server
    struct sockaddr_in serveraddr; 
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)port);

    // Bind the socket to the port
    if (bind(sd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
    {
        std::cerr << "Unable to bind UDP server to port " << port << "\n";
        exit(1);
    }

    // Return the socket descriptor to the caller
    return sd;
}
//=============================================================================




//=============================================================================
// This thread listens for the arrival of UDP message and sends them back up
// to the client of the TCP server
//=============================================================================
void udpServerThread(int port)
{

    const int prefixLen = 6;
    char ascii[20];

    // Make our UDP server
    int sd = makeServer(port);

    // The first six bytes of the udpBuffer will contain 5 digits and a space
    char* msgBuffer = g.udpBuffer + prefixLen;
    int   msgBufferLen = sizeof(g.udpBuffer) - prefixLen;

    // Sit in a loop listening to messages and sending them to the server
    while (true)
    {
        // Wait for a UDP message to arrive
        int byteCount = recvfrom(sd, msgBuffer, msgBufferLen, 0, nullptr, nullptr);
        
        // If we have a message and a client is connected to the TCP server...
        if (byteCount > 0 && g.tcpConnected)
        {
            // Just in case we want to print the string while debugging
            msgBuffer[byteCount] = 0;

            // Turn 'byteCount' into five digits followed by a linefeed
            sprintf(ascii, "%5i\n", byteCount);

            // Copy those six characters to the front of udpBuffer
            memcpy(g.udpBuffer, ascii, prefixLen);

            // And send the entire message up to the TCP client
            g.server.send(g.udpBuffer, byteCount + prefixLen);
        }
    }
}
//=============================================================================