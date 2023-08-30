#include "neonmake.h"

#include <string.h>
#include <sstream>
// #include <math.h>
// #include <iomanip>
// #include <ostream>
// #include "string_utils.h"
#include "../../include/debug.h"

/**
 * ctor
 */
NeonMake::NeonMake()
{
    Debug_printf("NeonMake::ctor()\r\n");
    _protocol = nullptr;
    _doc = "";
}

/**
 * dtor
 */
NeonMake::~NeonMake()
{
    Debug_printf("NeonMake::dtor()\r\n");
    _protocol = nullptr;
}

/**
 * Attach protocol handler
 */
void NeonMake::setProtocol(NetworkProtocol *newProtocol)
{
    Debug_printf("NeonMake::setProtocol()\r\n");
    _protocol = newProtocol;
}

/**
 * Parse data from protocol
 */
bool NeonMake::parse()
{
    NetworkStatus ns;

    if (_protocol == nullptr)
    {
        Debug_printf("NeonMake::parse() - NULL protocol.\r\n");
        return false;
    }

    _protocol->status(&ns);

    _parseBuffer = "";
    while (ns.connected)
    {
        _protocol->read(ns.rxBytesWaiting);
        _parseBuffer += *_protocol->receiveBuffer;
        _protocol->receiveBuffer->clear();
        _protocol->status(&ns);
        vTaskDelay(10);
    }
    Debug_printf("S: %s\r\n", _parseBuffer.c_str());

    _doc = compile(_parseBuffer);
    if (_doc.empty())
    {
        Debug_printf("NeonMake::parse() - Could not parse ADF\r\n");
        return false;
    }

    neon_bytes_remaining = readDocLen();

    return true;
}

/**
 * Return compiled Neon doc
 */
int NeonMake::readDocLen()
{
    return _doc.size();
}

/**
 * Return compiled Neon doc
 */
bool NeonMake::readDoc(uint8_t *rx_buf, unsigned short len)
{    
    if (_doc.empty())
        return true; // error

    memcpy(rx_buf, _doc.data(), len);

    return false; // no error.
}

/**
 * Compile ADF source to binary doc
 */
string NeonMake::compile(string adf)
{
    // TODO: actually compile the thing
    // but just inverse atascii for now to show it works
    stringstream ss;
    for (int i = 0; i < adf.length(); i++) {
        char c = adf[i];
        ss << (char)((c > 96 && c < 123) ? (c | 0x80) : c);
    }
    return ss.str();
}


bool NeonMake::status(NetworkStatus *s)
{
    Debug_printf("NeonMake::status(%u) %s\r\n", neon_bytes_remaining, _doc.c_str());
    s->connected = true;
    s->rxBytesWaiting = neon_bytes_remaining;
    s->error = neon_bytes_remaining == 0 ? 136 : 0;
    return false;
}